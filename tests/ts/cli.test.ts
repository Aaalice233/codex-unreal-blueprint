import { describe, expect, it } from "vitest";
import { runCli, type CliIo } from "../../src/cli/run.js";
import type { ToolName } from "../../src/shared/contracts.js";
import { ERROR_CODES, UnrealBlueprintError } from "../../src/shared/errors.js";

function capture(): { io: CliIo; stdout: string[]; stderr: string[] } {
  const stdout: string[] = [];
  const stderr: string[] = [];
  return {
    stdout,
    stderr,
    io: {
      stdout: (text) => stdout.push(text),
      stderr: (text) => stderr.push(text),
      readTextFile: async () => {
        throw new Error("unexpected file read");
      }
    }
  };
}

describe("stable CLI boundary", () => {
  it("routes status to the same operation boundary and emits JSON", async () => {
    const output = capture();
    const calls: ToolName[] = [];
    const exitCode = await runCli(
      ["status", "--json", "--uproject", "E:/Master/LuaSocial.uproject"],
      output.io,
      {
        invoke: async (tool) => {
          calls.push(tool);
          return { connected: true };
        }
      }
    );
    expect(exitCode).toBe(0);
    expect(calls).toEqual(["unreal_status"]);
    expect(JSON.parse(output.stdout.join(""))).toEqual({
      ok: true,
      command: "status",
      result: { connected: true }
    });
  });

  it("returns a stable machine-readable connection error and exit code", async () => {
    const output = capture();
    const exitCode = await runCli(["status", "--json"], output.io, {
      invoke: async () => {
        throw new UnrealBlueprintError(ERROR_CODES.SESSION_NOT_FOUND, "No running UE4.27 Editor session");
      }
    });
    expect(exitCode).toBe(4);
    const value = JSON.parse(output.stdout.join("")) as { ok: boolean; error: { code: string; message: string } };
    expect(value.ok).toBe(false);
    expect(value.error.code).toBe("SESSION_NOT_FOUND");
  });

  it("forwards project and engine setup options to the setup implementation", async () => {
    const output = capture();
    let observed: unknown;
    const exitCode = await runCli([
      "setup", "--json", "--uproject", "E:/Game/Game.uproject", "--engine-root", "E:/UE_4.27",
      "--input", "{\"scope\":\"engine\",\"skipPiInstall\":true}"
    ], output.io, {
      setup: async (input) => {
        observed = input;
        return { setup: "completed" };
      }
    });
    expect(exitCode).toBe(0);
    expect(observed).toEqual({
      scope: "engine",
      skipPiInstall: true,
      uproject: "E:/Game/Game.uproject",
      engineRoot: "E:/UE_4.27"
    });
  });

  it("forwards headless commandlet options without selecting an Editor", async () => {
    const output = capture();
    let observed: unknown;
    const exitCode = await runCli([
      "verify", "--headless", "--uproject", "E:/Game/Game.uproject", "--engine-root", "E:/UE_4.27",
      "--input", "{\"assetPaths\":[\"/Game/BP_Test\"]}"
    ], output.io, {
      invoke: async (tool, params, options) => {
        observed = { tool, params, options };
        return { verified: true };
      }
    });
    expect(exitCode).toBe(0);
    expect(observed).toMatchObject({
      tool: "blueprint_verify",
      options: { headless: true, engineRoot: "E:/UE_4.27", session: { uproject: "E:/Game/Game.uproject" } }
    });
  });

  it("does not promise process-local job controls in headless mode", async () => {
    const output = capture();
    let invoked = false;
    const exitCode = await runCli([
      "job", "--headless", "--uproject", "E:/Game/Game.uproject", "--engine-root", "E:/UE_4.27", "--json",
      "--input", "{\"action\":\"query\",\"jobId\":\"job-from-old-process\"}"
    ], output.io, {
      invoke: async () => {
        invoked = true;
        return {};
      }
    });
    expect(exitCode).toBe(2);
    expect(invoked).toBe(false);
    expect(JSON.parse(output.stdout.join(""))).toMatchObject({
      ok: false,
      error: { code: "INVALID_ARGUMENT" }
    });
  });

  it("uses exit code 2 for invalid CLI input", async () => {
    const output = capture();
    expect(await runCli(["status", "--timeout", "0", "--json"], output.io)).toBe(2);
    expect(JSON.parse(output.stdout.join("")).error.code).toBe("INVALID_ARGUMENT");
  });

  it("does not report partial or unknown write state as CLI success", async () => {
    const output = capture();
    const exitCode = await runCli([
      "job", "--json", "--input", "{\"action\":\"query\",\"requestId\":\"request-1\"}"
    ], output.io, {
      invoke: async () => ({
        requestId: "request-1",
        result: { partial: true, stateUnknown: true, failureReport: { unknownAssets: ["/Game/BP_Test"] } }
      })
    });
    expect(exitCode).toBe(1);
    const value = JSON.parse(output.stdout.join(""));
    expect(value).toMatchObject({
      ok: false,
      error: {
        code: "WRITE_STATE_UNKNOWN",
        context: { details: { result: { failureReport: { unknownAssets: ["/Game/BP_Test"] } } } }
      }
    });
  });
});
