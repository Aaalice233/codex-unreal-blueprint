import { readdir, readFile } from "node:fs/promises";
import { createConnection } from "node:net";
import { join, win32 } from "node:path";
import { ERROR_CODES, UnrealBlueprintError } from "../shared/errors.js";
import { isJsonObject, type JsonObject } from "../shared/json.js";
import { parseProtocolVersion } from "../shared/protocol.js";

export interface EditorSession {
  readonly editorSessionId: string;
  readonly pid: number;
  readonly uproject: string;
  readonly engineVersion: string;
  readonly host: "127.0.0.1";
  readonly port: number;
  readonly authToken: string;
  readonly pluginVersion: string;
  readonly protocolVersion: string;
  readonly capabilities: JsonObject;
  readonly startedAt: string;
  readonly descriptorPath: string;
}

export interface SessionQuery {
  readonly editorSessionId?: string;
  readonly uproject?: string;
}

export interface DiscoveryOptions {
  readonly sessionsDirectory?: string;
  readonly isProcessAlive?: (pid: number) => boolean;
  readonly isSessionReachable?: (session: EditorSession) => Promise<boolean>;
  readonly readSessionFile?: (descriptorPath: string) => Promise<string>;
}

export function defaultSessionsDirectory(environment: NodeJS.ProcessEnv = process.env): string {
  const localAppData = environment.LOCALAPPDATA;
  if (!localAppData) {
    throw new UnrealBlueprintError(
      ERROR_CODES.SESSION_DIRECTORY_UNAVAILABLE,
      "LOCALAPPDATA is not set; cannot discover Unreal Editor sessions"
    );
  }
  return win32.join(localAppData, "PiUnrealBlueprint", "sessions");
}

export function canonicalizeUproject(path: string): string {
  if (path.includes("\0") || !win32.isAbsolute(path)) {
    throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, `uproject must be an absolute Windows path: ${path}`);
  }
  const normalized = win32.normalize(path.replaceAll("/", "\\"));
  if (win32.extname(normalized).toLowerCase() !== ".uproject") {
    throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, `uproject must identify a .uproject file: ${path}`);
  }
  return normalized.toLowerCase();
}

function processIsAlive(pid: number): boolean {
  try {
    process.kill(pid, 0);
    return true;
  } catch (error) {
    // Windows reports EPERM for nonexistent PIDs; Pi and UE run as the same user, so it is not a usable liveness signal there.
    return process.platform !== "win32" && isNodeError(error) && error.code === "EPERM";
  }
}

function isNodeError(error: unknown): error is NodeJS.ErrnoException {
  return error instanceof Error && "code" in error;
}

function sessionIsReachable(session: EditorSession): Promise<boolean> {
  return new Promise((resolve) => {
    const socket = createConnection({ host: session.host, port: session.port });
    let settled = false;
    const finish = (reachable: boolean) => {
      if (settled) return;
      settled = true;
      socket.destroy();
      resolve(reachable);
    };
    socket.setTimeout(250, () => finish(false));
    socket.once("connect", () => finish(true));
    socket.once("error", () => finish(false));
  });
}

function requireString(object: JsonObject, key: string): string {
  const value = object[key];
  if (typeof value !== "string" || value.length === 0) throw new Error(`${key} must be a non-empty string`);
  return value;
}

function parseSession(value: unknown, descriptorPath: string): EditorSession {
  if (!isJsonObject(value)) throw new Error("descriptor root must be an object");
  const pid = value.pid;
  const port = value.port;
  if (!Number.isSafeInteger(pid) || (pid as number) <= 0) throw new Error("pid must be a positive integer");
  if (!Number.isSafeInteger(port) || (port as number) < 1 || (port as number) > 65_535) throw new Error("port is invalid");
  if (value.host !== "127.0.0.1") throw new Error("host must be 127.0.0.1");
  if (!isJsonObject(value.capabilities)) throw new Error("capabilities must be an object");
  const startedAt = requireString(value, "startedAt");
  if (Number.isNaN(Date.parse(startedAt))) throw new Error("startedAt must be an ISO timestamp");
  const editorSessionId = requireString(value, "editorSessionId");
  const uproject = requireString(value, "uproject");
  const authToken = requireString(value, "authToken");
  const protocolVersion = requireString(value, "protocolVersion");
  canonicalizeUproject(uproject);
  parseProtocolVersion(protocolVersion);
  if (authToken.trim() !== authToken) throw new Error("authToken must not have surrounding whitespace");
  return {
    editorSessionId,
    pid: pid as number,
    uproject,
    engineVersion: requireString(value, "engineVersion"),
    host: "127.0.0.1",
    port: port as number,
    authToken,
    pluginVersion: requireString(value, "pluginVersion"),
    protocolVersion,
    capabilities: value.capabilities,
    startedAt,
    descriptorPath
  };
}

interface DiscoveryScan {
  readonly directory: string;
  readonly sessions: EditorSession[];
  readonly stale: EditorSession[];
  readonly invalid: string[];
  readonly descriptorCount: number;
}

function matchesQuery(session: EditorSession, query: SessionQuery, canonicalQueryUproject: string | undefined): boolean {
  if (query.editorSessionId !== undefined && session.editorSessionId !== query.editorSessionId) return false;
  return canonicalQueryUproject === undefined || canonicalizeUproject(session.uproject) === canonicalQueryUproject;
}

async function scanSessions(query: SessionQuery, options: DiscoveryOptions): Promise<DiscoveryScan> {
  const directory = options.sessionsDirectory ?? defaultSessionsDirectory();
  const alive = options.isProcessAlive ?? processIsAlive;
  // Injected PID checks are used by deterministic unit tests; production discovery also proves the descriptor port is live.
  const reachable = options.isSessionReachable ?? (options.isProcessAlive ? async () => true : sessionIsReachable);
  const readDescriptor = options.readSessionFile ?? ((path: string) => readFile(path, "utf8"));
  const canonicalQueryUproject = query.uproject === undefined ? undefined : canonicalizeUproject(query.uproject);
  let names: string[];
  try {
    names = (await readdir(directory)).filter((name) => name.toLowerCase().endsWith(".json")).sort();
  } catch (error) {
    if (isNodeError(error) && error.code === "ENOENT") return { directory, sessions: [], stale: [], invalid: [], descriptorCount: 0 };
    throw new UnrealBlueprintError(ERROR_CODES.SESSION_DIRECTORY_UNAVAILABLE, `Cannot read session directory: ${directory}`, { cause: error });
  }

  const sessions: EditorSession[] = [];
  const stale: EditorSession[] = [];
  const invalid: string[] = [];
  for (const name of names) {
    const descriptorPath = join(directory, name);
    try {
      const session = parseSession(JSON.parse(await readDescriptor(descriptorPath)), descriptorPath);
      if (!matchesQuery(session, query, canonicalQueryUproject)) continue;
      if (!alive(session.pid) || !(await reachable(session))) {
        stale.push(session);
        continue;
      }
      sessions.push(session);
    } catch (error) {
      invalid.push(`${name}: ${error instanceof Error ? error.message : "invalid descriptor"}`);
    }
  }
  sessions.sort((left, right) => Date.parse(right.startedAt) - Date.parse(left.startedAt));
  return { directory, sessions, stale, invalid, descriptorCount: names.length };
}

export async function discoverSessions(query: SessionQuery = {}, options: DiscoveryOptions = {}): Promise<EditorSession[]> {
  const scan = await scanSessions(query, options);
  if (scan.sessions.length === 0 && scan.invalid.length > 0 && scan.invalid.length === scan.descriptorCount) {
    throw new UnrealBlueprintError(ERROR_CODES.SESSION_INVALID, "No valid Unreal Editor session descriptors were found", {
      context: { details: { directory: scan.directory, invalid: scan.invalid } }
    });
  }
  return scan.sessions;
}

export async function selectSession(query: SessionQuery = {}, options: DiscoveryOptions = {}): Promise<EditorSession> {
  const scan = await scanSessions(query, options);
  if (scan.sessions.length === 0) {
    if (scan.stale.length > 0) {
      throw new UnrealBlueprintError(ERROR_CODES.SESSION_STALE, "Matching Unreal Editor session descriptors refer to exited processes", {
        retryable: true,
        context: { details: { editorSessionIds: scan.stale.map((session) => session.editorSessionId) } }
      });
    }
    if (scan.invalid.length > 0 && scan.invalid.length === scan.descriptorCount) {
      throw new UnrealBlueprintError(ERROR_CODES.SESSION_INVALID, "No valid Unreal Editor session descriptors were found", {
        context: { details: { directory: scan.directory, invalid: scan.invalid } }
      });
    }
    const target = query.uproject === undefined ? "the requested project" : query.uproject;
    throw new UnrealBlueprintError(
      ERROR_CODES.SESSION_NOT_FOUND,
      `No running UE4.27 Editor session was found for ${target}. Open the project with PiUnrealBlueprint enabled.`,
      { retryable: true }
    );
  }
  if (scan.sessions.length > 1) {
    throw new UnrealBlueprintError(
      ERROR_CODES.SESSION_AMBIGUOUS,
      "Multiple Unreal Editor sessions match; specify editorSessionId",
      {
        context: {
          details: {
            candidates: scan.sessions.map((session) => ({
              editorSessionId: session.editorSessionId,
              pid: session.pid,
              uproject: session.uproject,
              startedAt: session.startedAt
            }))
          }
        }
      }
    );
  }
  return scan.sessions[0] as EditorSession;
}
