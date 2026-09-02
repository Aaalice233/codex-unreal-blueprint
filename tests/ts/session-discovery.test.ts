import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { afterEach, describe, expect, it } from "vitest";
import {
  canonicalizeUproject,
  defaultSessionsDirectory,
  discoverSessions,
  selectSession
} from "../../src/client/session-discovery.js";
import { UnrealBlueprintError } from "../../src/shared/errors.js";

const cleanup: string[] = [];
afterEach(async () => {
  await Promise.all(cleanup.splice(0).map((path) => rm(path, { recursive: true, force: true })));
});

async function fixture(): Promise<string> {
  const directory = await mkdtemp(join(tmpdir(), "pi-ubp-sessions-"));
  cleanup.push(directory);
  return directory;
}

async function writeSession(directory: string, id: string, overrides: Record<string, unknown> = {}): Promise<void> {
  await writeFile(join(directory, `${id}.json`), JSON.stringify({
    editorSessionId: id,
    pid: 100,
    uproject: "E:/Master/LuaSocial.uproject",
    engineVersion: "4.27.2",
    host: "127.0.0.1",
    port: id === "one" ? 32101 : 32102,
    authToken: `token-${id}`,
    pluginVersion: "1.0.0",
    protocolVersion: "1.0.0",
    capabilities: { blueprint: true },
    startedAt: id === "one" ? "2026-01-01T00:00:00.000Z" : "2026-01-02T00:00:00.000Z",
    ...overrides
  }), "utf8");
}

describe("Editor session discovery", () => {
  it("matches an exact uproject path with Windows case and slash normalization", async () => {
    const directory = await fixture();
    await writeSession(directory, "one");
    const sessions = await discoverSessions(
      { uproject: "e:\\master\\LuaSocial.uproject" },
      { sessionsDirectory: directory, isProcessAlive: () => true }
    );
    expect(sessions.map((session) => session.editorSessionId)).toEqual(["one"]);
  });

  it("drops stale PID descriptors", async () => {
    const directory = await fixture();
    await writeSession(directory, "one");
    expect(await discoverSessions({}, { sessionsDirectory: directory, isProcessAlive: () => false })).toEqual([]);
  });

  it("requires explicit selection when multiple Editors match", async () => {
    const directory = await fixture();
    await writeSession(directory, "one");
    await writeSession(directory, "two");
    await expect(selectSession(
      { uproject: "E:/Master/LuaSocial.uproject" },
      { sessionsDirectory: directory, isProcessAlive: () => true }
    )).rejects.toMatchObject({ code: "SESSION_AMBIGUOUS" } satisfies Partial<UnrealBlueprintError>);
  });

  it("reports malformed descriptors instead of treating them as a healthy session", async () => {
    const directory = await fixture();
    await writeFile(join(directory, "broken.json"), "{not-json", "utf8");
    await expect(discoverSessions({}, { sessionsDirectory: directory, isProcessAlive: () => true }))
      .rejects.toMatchObject({ code: "SESSION_INVALID" } satisfies Partial<UnrealBlueprintError>);
  });

  it("ignores a damaged descriptor when a matching healthy Editor exists", async () => {
    const directory = await fixture();
    await writeFile(join(directory, "broken.json"), "{not-json", "utf8");
    await writeSession(directory, "one");
    await expect(selectSession({}, { sessionsDirectory: directory, isProcessAlive: () => true }))
      .resolves.toMatchObject({ editorSessionId: "one" });
  });

  it("distinguishes an expired descriptor from a missing Editor", async () => {
    const directory = await fixture();
    await writeSession(directory, "one");
    await expect(selectSession(
      { editorSessionId: "one" },
      { sessionsDirectory: directory, isProcessAlive: () => false }
    )).rejects.toMatchObject({ code: "SESSION_STALE" } satisfies Partial<UnrealBlueprintError>);
  });

  it("reports a permission-unreadable descriptor without a fake fallback", async () => {
    const directory = await fixture();
    await writeSession(directory, "one");
    const denied = Object.assign(new Error("permission denied"), { code: "EACCES" });
    await expect(discoverSessions({}, {
      sessionsDirectory: directory,
      isProcessAlive: () => true,
      readSessionFile: async () => { throw denied; }
    })).rejects.toMatchObject({ code: "SESSION_INVALID" } satisfies Partial<UnrealBlueprintError>);
  });

  it("selects only the exact project when different Editors are running", async () => {
    const directory = await fixture();
    await writeSession(directory, "one");
    await writeSession(directory, "two", { uproject: "E:/Other/LuaSocial.uproject" });
    await expect(selectSession(
      { uproject: "E:/Master/Folder/../LuaSocial.uproject" },
      { sessionsDirectory: directory, isProcessAlive: () => true }
    )).resolves.toMatchObject({ editorSessionId: "one" });
  });

  it("uses the Windows LOCALAPPDATA session directory on every host", () => {
    expect(defaultSessionsDirectory({ LOCALAPPDATA: "C:\\Users\\Alice\\AppData\\Local" }))
      .toBe("C:\\Users\\Alice\\AppData\\Local\\PiUnrealBlueprint\\sessions");
  });

  it("rejects relative and non-uproject paths instead of resolving them against cwd", () => {
    expect(() => canonicalizeUproject("Master/LuaSocial.uproject")).toThrow(/absolute Windows path/);
    expect(() => canonicalizeUproject("E:/Master/LuaSocial.txt")).toThrow(/\.uproject file/);
  });
});
