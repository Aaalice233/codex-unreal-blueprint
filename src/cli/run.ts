import { readFile } from "node:fs/promises";
import { spawn } from "node:child_process";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { CLI_COMMANDS, type CliCommand, type ToolName } from "../shared/contracts.js";
import { asUnrealBlueprintError, ERROR_CODES, UnrealBlueprintError } from "../shared/errors.js";
import { assertJsonValue, isJsonObject, type JsonObject, type JsonValue } from "../shared/json.js";
import { discoverSessions, invokeWriteWithRecovery, withUnrealClient, type UnrealClientOptions } from "../client/index.js";
import { invokeCommandlet } from "./commandlet.js";
import { managePlugin } from "./plugin-manager.js";

export interface CliIo {
  readonly stdout: (text: string) => void;
  readonly stderr: (text: string) => void;
  readonly readTextFile: (path: string) => Promise<string>;
}

export interface CliDependencies {
  readonly invoke?: (tool: ToolName, params: JsonObject, options: UnrealClientOptions & { timeoutMs?: number; headless?: boolean; engineRoot?: string }) => Promise<JsonValue>;
  readonly plugin?: typeof managePlugin;
  readonly setup?: (input: JsonObject) => Promise<JsonValue>;
}

interface ParsedArguments {
  readonly command: CliCommand;
  readonly pluginAction?: "install" | "update" | "remove";
  readonly json: boolean;
  readonly input: JsonObject;
  readonly uproject?: string;
  readonly editorSessionId?: string;
  readonly timeoutMs?: number;
  readonly headless: boolean;
  readonly engineRoot?: string;
}

const HELP = `pi-unreal-blueprint <command> [options]

Commands:
  setup | doctor | status | search | inspect | capabilities
  validate | apply | job | verify
  plugin install|update|remove

Options:
  --input <json|@file>   Command parameters as one JSON object
  --uproject <path>      Match the exact .uproject session
  --session <id>         Select an exact editorSessionId
  --timeout <ms>         RPC/Commandlet timeout
  --headless             Run through UE4Editor-Cmd Commandlet
  --engine-root <path>   UE4.27 root for headless or engine install
  --json                 Stable machine-readable output
  --help                  Show this help

Headless behavior:
  apply waits for a terminal Job snapshot; job query/wait/cancel require an interactive Editor session.
`;

function requireValue(argv: readonly string[], index: number, flag: string): string {
  const value = argv[index + 1];
  if (value === undefined || value.startsWith("--")) {
    throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, `${flag} requires a value`);
  }
  return value;
}

async function parseInput(raw: string | undefined, io: CliIo): Promise<JsonObject> {
  if (raw === undefined) return {};
  const text = raw.startsWith("@") ? await io.readTextFile(raw.slice(1)) : raw;
  let value: unknown;
  try {
    value = JSON.parse(text);
  } catch (error) {
    throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, "--input must be valid JSON or @file", { cause: error });
  }
  assertJsonValue(value);
  if (!isJsonObject(value)) throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, "--input root must be a JSON object");
  return value;
}

async function parseArguments(argv: readonly string[], io: CliIo): Promise<ParsedArguments | "help"> {
  if (argv.length === 0 || argv.includes("--help") || argv.includes("-h")) return "help";
  const commandValue = argv[0];
  if (commandValue === undefined || !CLI_COMMANDS.includes(commandValue as CliCommand)) {
    throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, `Unknown command: ${commandValue ?? ""}`);
  }
  const command = commandValue as CliCommand;
  let index = 1;
  let pluginAction: ParsedArguments["pluginAction"];
  if (command === "plugin") {
    const action = argv[index++];
    if (action !== "install" && action !== "update" && action !== "remove") {
      throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, "plugin requires install, update, or remove");
    }
    pluginAction = action;
  }
  let json = false;
  let rawInput: string | undefined;
  let uproject: string | undefined;
  let editorSessionId: string | undefined;
  let timeoutMs: number | undefined;
  let headless = false;
  let engineRoot: string | undefined;
  while (index < argv.length) {
    const flag = argv[index];
    switch (flag) {
      case "--json":
        json = true;
        index += 1;
        break;
      case "--headless":
        headless = true;
        index += 1;
        break;
      case "--engine-root":
        engineRoot = requireValue(argv, index, flag);
        index += 2;
        break;
      case "--input":
        rawInput = requireValue(argv, index, flag);
        index += 2;
        break;
      case "--uproject":
        uproject = requireValue(argv, index, flag);
        index += 2;
        break;
      case "--session":
        editorSessionId = requireValue(argv, index, flag);
        index += 2;
        break;
      case "--timeout": {
        const rawTimeout = requireValue(argv, index, flag);
        timeoutMs = Number(rawTimeout);
        if (!Number.isSafeInteger(timeoutMs) || timeoutMs <= 0 || timeoutMs > 600_000) {
          throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, "--timeout must be an integer from 1 to 600000");
        }
        index += 2;
        break;
      }
      default:
        throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, `Unknown argument: ${flag ?? ""}`);
    }
  }
  if (headless && editorSessionId !== undefined) {
    throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, "--headless cannot be combined with --session");
  }
  if ((command === "setup" || command === "plugin") && (editorSessionId !== undefined || timeoutMs !== undefined || headless)) {
    throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, `${command} does not accept --session, --timeout, or --headless`);
  }
  if (engineRoot !== undefined && command !== "setup" && command !== "plugin" && !headless) {
    throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, "--engine-root requires setup, plugin, or --headless");
  }
  return {
    command,
    ...(pluginAction === undefined ? {} : { pluginAction }),
    json,
    input: await parseInput(rawInput, io),
    ...(uproject === undefined ? {} : { uproject }),
    ...(editorSessionId === undefined ? {} : { editorSessionId }),
    ...(timeoutMs === undefined ? {} : { timeoutMs }),
    headless,
    ...(engineRoot === undefined ? {} : { engineRoot })
  };
}

function commandToInvocation(parsed: ParsedArguments): { tool: ToolName; params: JsonObject } {
  switch (parsed.command) {
    case "doctor": return { tool: "unreal_doctor", params: parsed.input };
    case "status": return { tool: "unreal_status", params: parsed.input };
    case "search": return { tool: "unreal_search", params: parsed.input };
    case "inspect": return { tool: "blueprint_inspect", params: parsed.input };
    case "capabilities": return { tool: "blueprint_capabilities", params: parsed.input };
    case "validate": return { tool: "blueprint_validate", params: parsed.input };
    case "apply": return { tool: "blueprint_apply", params: parsed.input };
    case "job": return { tool: "blueprint_job", params: parsed.input };
    case "verify": return { tool: "blueprint_verify", params: parsed.input };
    case "setup":
    case "plugin":
      throw new UnrealBlueprintError(ERROR_CODES.INTERNAL, `${parsed.command} must be handled before RPC routing`);
  }
}

async function defaultInvoke(tool: ToolName, params: JsonObject, options: UnrealClientOptions & { timeoutMs?: number; headless?: boolean; engineRoot?: string }): Promise<JsonValue> {
  if (options.headless === true) {
    const uproject = options.session?.uproject;
    if (uproject === undefined) throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, "--headless requires --uproject");
    return invokeCommandlet(tool, params, {
      uproject,
      ...(options.engineRoot === undefined ? {} : { engineRoot: options.engineRoot }),
      ...(options.timeoutMs === undefined ? {} : { timeoutMs: options.timeoutMs })
    });
  }
  if (tool === "unreal_status" && options.session?.editorSessionId === undefined) {
    const sessions = await discoverSessions(options.session, options.discovery);
    if (sessions.length !== 1) {
      return {
        connected: false,
        ambiguous: sessions.length > 1,
        sessions: sessions.map((session) => ({
          editorSessionId: session.editorSessionId,
          pid: session.pid,
          uproject: session.uproject,
          engineVersion: session.engineVersion,
          pluginVersion: session.pluginVersion,
          protocolVersion: session.protocolVersion
        }))
      };
    }
    const session = sessions[0];
    if (session === undefined) throw new UnrealBlueprintError(ERROR_CODES.INTERNAL, "Session discovery returned an inconsistent result");
    return withUnrealClient({ ...options, session: { editorSessionId: session.editorSessionId } }, (client) => client.invoke(tool, params, options.timeoutMs === undefined ? {} : { timeoutMs: options.timeoutMs }));
  }
  if (tool === "blueprint_apply") {
    return invokeWriteWithRecovery(options, tool, params, options.timeoutMs === undefined ? {} : { timeoutMs: options.timeoutMs });
  }
  return withUnrealClient(options, (client) => client.invoke(tool, params, options.timeoutMs === undefined ? {} : { timeoutMs: options.timeoutMs }));
}

function currentPackageRoot(): string {
  const directory = dirname(fileURLToPath(import.meta.url));
  return directory.includes(`${process.platform === "win32" ? "\\" : "/"}dist${process.platform === "win32" ? "\\" : "/"}`)
    ? resolve(directory, "../../../../..")
    : resolve(directory, "../..");
}

async function defaultSetup(input: JsonObject): Promise<JsonValue> {
  const stringArguments = [
    ["config", "-Config"],
    ["scope", "-Scope"],
    ["uproject", "-UProject"],
    ["engineRoot", "-EngineRoot"],
    ["pluginTarget", "-PluginTarget"],
    ["piAgentDir", "-PiAgentDir"],
    ["piSource", "-PiSource"]
  ] as const;
  const booleanArguments = [
    ["skipPiInstall", "-SkipPiInstall"],
    ["skipUnrealBuild", "-SkipUnrealBuild"],
    ["dryRun", "-DryRun"]
  ] as const;
  const script = resolve(currentPackageRoot(), "scripts/setup.ps1");
  const args = ["-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", script];
  for (const [field, flag] of stringArguments) {
    const value = input[field];
    if (value === undefined) continue;
    if (typeof value !== "string" || value.trim().length === 0) {
      throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, `setup input.${field} must be a non-empty string`);
    }
    args.push(flag, value);
  }
  if (input.scope !== undefined && input.scope !== "project" && input.scope !== "engine") {
    throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, "setup input.scope must be 'project' or 'engine'");
  }
  for (const [field, flag] of booleanArguments) {
    const value = input[field];
    if (value === undefined) continue;
    if (typeof value !== "boolean") throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, `setup input.${field} must be a boolean`);
    if (value) args.push(flag);
  }
  const executable = process.platform === "win32" ? "pwsh.exe" : "pwsh";
  return new Promise((resolvePromise, reject) => {
    const child = spawn(executable, args, { windowsHide: true, stdio: ["ignore", "pipe", "pipe"] });
    let stdout = "";
    let stderr = "";
    child.stdout.setEncoding("utf8");
    child.stderr.setEncoding("utf8");
    child.stdout.on("data", (chunk: string) => { stdout += chunk; });
    child.stderr.on("data", (chunk: string) => { stderr += chunk; });
    child.once("error", (cause) => reject(new UnrealBlueprintError(ERROR_CODES.SETUP_FAILED, "Failed to start scripts/setup.ps1", { cause })));
    child.once("exit", (code) => {
      if (code !== 0) {
        reject(new UnrealBlueprintError(ERROR_CODES.SETUP_FAILED, `setup.ps1 failed with exit code ${code ?? -1}`, { context: { details: { exitCode: code ?? -1, stdout, stderr } } }));
        return;
      }
      resolvePromise({ setup: "completed", script, stdout, stderr });
    });
  });
}

function uncertainWriteState(value: JsonValue): "partial" | "stateUnknown" | undefined {
  if (!isJsonObject(value)) return undefined;
  if (value.stateUnknown === true) return "stateUnknown";
  if (value.partial === true) return "partial";
  for (const key of ["result", "error", "details", "failureReport"]) {
    const nested = value[key];
    if (isJsonObject(nested)) {
      const state = uncertainWriteState(nested);
      if (state !== undefined) return state;
    }
  }
  return undefined;
}

function isTerminalFailure(value: JsonValue): boolean {
  return isJsonObject(value) && (value.phase === "Failed" || value.phase === "Cancelled"
    || value.terminalPhase === "Failed" || value.terminalPhase === "Cancelled");
}

function writeSuccess(io: CliIo, parsed: ParsedArguments, result: JsonValue): void {
  if (parsed.json) {
    io.stdout(`${JSON.stringify({ ok: true, command: parsed.command, result })}\n`);
    return;
  }
  io.stdout(`${parsed.command}: success / 成功\n${JSON.stringify(result, null, 2)}\n`);
}

function writeFailure(io: CliIo, json: boolean, error: unknown): void {
  const normalized = asUnrealBlueprintError(error);
  if (json) {
    io.stdout(`${JSON.stringify({ ok: false, error: normalized.toJSON() })}\n`);
    return;
  }
  io.stderr(`Error / 错误 [${normalized.code}]: ${normalized.message}\n`);
}

export function cliExitCodeForError(error: unknown): number {
  const code = asUnrealBlueprintError(error).code;
  if (code === ERROR_CODES.INVALID_ARGUMENT) return 2;
  if (code === ERROR_CODES.SETUP_FAILED || code.startsWith("PLUGIN_")) return 3;
  if (code.startsWith("SESSION_") || code.startsWith("CONNECTION_") || code === ERROR_CODES.AUTHENTICATION_FAILED || code === ERROR_CODES.PROTOCOL_MISMATCH) return 4;
  if (code.startsWith("COMMANDLET_")) return 5;
  return 1;
}

export async function runCli(argv: readonly string[], io: CliIo, dependencies: CliDependencies = {}): Promise<number> {
  let json = argv.includes("--json");
  try {
    const parsed = await parseArguments(argv, io);
    if (parsed === "help") {
      io.stdout(HELP);
      return 0;
    }
    json = parsed.json;
    if (parsed.command === "setup") {
      const input: JsonObject = {
        ...parsed.input,
        ...(parsed.uproject === undefined ? {} : { uproject: parsed.uproject }),
        ...(parsed.engineRoot === undefined ? {} : { engineRoot: parsed.engineRoot })
      };
      const result = await (dependencies.setup ?? defaultSetup)(input);
      writeSuccess(io, parsed, result);
      return 0;
    }
    if (parsed.command === "plugin") {
      const input: JsonObject = {
        ...parsed.input,
        ...(parsed.uproject === undefined ? {} : { uproject: parsed.uproject }),
        ...(parsed.engineRoot === undefined ? {} : { engineRoot: parsed.engineRoot })
      };
      const result = await (dependencies.plugin ?? managePlugin)(parsed.pluginAction as "install" | "update" | "remove", input);
      writeSuccess(io, parsed, result);
      return 0;
    }
    if (parsed.headless && parsed.command === "job") {
      throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT,
        "Headless job query/wait/cancel is unavailable because jobs are process-local; headless apply already waits for a terminal result");
    }
    const invocation = commandToInvocation(parsed);
    const session = {
      ...(parsed.editorSessionId === undefined ? {} : { editorSessionId: parsed.editorSessionId }),
      ...(parsed.uproject === undefined ? {} : { uproject: parsed.uproject })
    };
    const invoke = dependencies.invoke ?? defaultInvoke;
    const result = await invoke(invocation.tool, invocation.params, {
      session,
      ...(parsed.timeoutMs === undefined ? {} : { timeoutMs: parsed.timeoutMs }),
      ...(parsed.headless ? { headless: true } : {}),
      ...(parsed.engineRoot === undefined ? {} : { engineRoot: parsed.engineRoot })
    });
    const uncertain = uncertainWriteState(result);
    if (uncertain !== undefined || isTerminalFailure(result)) {
      const code = uncertain === "stateUnknown"
        ? ERROR_CODES.WRITE_STATE_UNKNOWN
        : uncertain === "partial" ? ERROR_CODES.WRITE_PARTIAL : ERROR_CODES.JOB_TERMINAL_FAILURE;
      const message = uncertain === "stateUnknown"
        ? "The write completed with unknown asset state"
        : uncertain === "partial" ? "The write completed with partially saved assets" : "The requested job ended without success";
      throw new UnrealBlueprintError(code, message, isJsonObject(result) ? { context: { details: result } } : {});
    }
    writeSuccess(io, parsed, result);
    return 0;
  } catch (error) {
    writeFailure(io, json, error);
    return cliExitCodeForError(error);
  }
}

export const nodeCliIo: CliIo = {
  stdout: (text) => process.stdout.write(text),
  stderr: (text) => process.stderr.write(text),
  readTextFile: (path) => readFile(path, "utf8")
};
