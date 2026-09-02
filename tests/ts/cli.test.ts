import { describe, expect, it } from "vitest";
import { runCli, type CliIo } from "../../src/cli/run.js";
import type { ToolName } from "../../src/shared/contracts.js";

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

  it("returns a stable machine-readable error when no Editor is connected", async () => {
    const output = capture();
    const exitCode = await runCli(["status", "--json"], output.io, {
      invoke: async () => {
        const error = new Error("No running UE4.27 Editor session");
        error.name = "SESSION_NOT_FOUND";
        throw error;
      }
    });
    expect(exitCode).toBe(1);
    const value = JSON.parse(output.stdout.join("")) as { ok: boolean; error: { code: string; message: string } };
    expect(value.ok).toBe(false);
    expect(value.error.message).toContain("No running UE4.27 Editor session");
  });

  it("does not claim setup succeeded while the UE installer is absent", async () => {
    const output = capture();
    const exitCode = await runCli(["setup", "--json"], output.io);
    expect(exitCode).toBe(1);
    expect(JSON.parse(output.stdout.join("")).error.code).toBe("SETUP_UNAVAILABLE");
  });

  it("forces restore onto blueprint_history with action restore", async () => {
    const output = capture();
    let observed: unknown;
    const exitCode = await runCli(
      ["restore", "--input", "{\"jobId\":\"job-1\",\"requestId\":\"restore-1\"}"],
      output.io,
      { invoke: async (_tool, params) => {
        observed = params;
        return { jobId: "restore-job" };
      } }
    );
    expect(exitCode).toBe(0);
    expect(observed).toEqual({ jobId: "job-1", requestId: "restore-1", action: "restore" });
  });
});
