import { readFile, writeFile } from "node:fs/promises";
import { describe, expect, it } from "vitest";
import { invokeCommandlet, type CommandletProcessRunner } from "../../src/cli/commandlet.js";

function argumentPath(args: readonly string[], prefix: string): string {
  const value = args.find((argument) => argument.startsWith(prefix));
  if (value === undefined) throw new Error(`missing ${prefix}`);
  return value.slice(prefix.length);
}

describe("Commandlet launcher", () => {
  it("uses request/result files and preserves the namespaced RPC contract", async () => {
    let observedMethod = "";
    const processRunner: CommandletProcessRunner = async (_executable, args) => {
      const requestPath = argumentPath(args, "-Request=");
      const resultPath = argumentPath(args, "-Result=");
      const request = JSON.parse(await readFile(requestPath, "utf8")) as { id: string; method: string };
      observedMethod = request.method;
      await writeFile(resultPath, JSON.stringify({ jsonrpc: "2.0", id: request.id, result: { verified: true } }), "utf8");
      return { code: 0, stdout: "", stderr: "" };
    };

    await expect(invokeCommandlet("blueprint_verify", { assetPaths: ["/Game/BP_Test"] }, {
      uproject: "E:/Game/Game.uproject",
      executable: "E:/UE_4.27/Engine/Binaries/Win64/UE4Editor-Cmd.exe",
      processRunner
    })).resolves.toEqual({ verified: true });
    expect(observedMethod).toBe("blueprint.verify");
  });

  it("returns only a terminal apply snapshot", async () => {
    const processRunner: CommandletProcessRunner = async (_executable, args) => {
      const requestPath = argumentPath(args, "-Request=");
      const resultPath = argumentPath(args, "-Result=");
      const request = JSON.parse(await readFile(requestPath, "utf8")) as { id: string };
      await writeFile(resultPath, JSON.stringify({
        jsonrpc: "2.0",
        id: request.id,
        result: {
          jobId: "job-terminal",
          requestId: "headless-terminal",
          phase: "Succeeded",
          terminal: true,
          result: { success: true, packages: [{ packageName: "/Game/BP_Test", saved: true, verified: true }] }
        }
      }), "utf8");
      return { code: 0, stdout: "", stderr: "" };
    };

    await expect(invokeCommandlet("blueprint_apply", {
      requestId: "headless-terminal",
      operations: [{ operation: "asset.create" }]
    }, {
      uproject: "E:/Game/Game.uproject",
      executable: "E:/UE_4.27/Engine/Binaries/Win64/UE4Editor-Cmd.exe",
      processRunner
    })).resolves.toMatchObject({ terminal: true, phase: "Succeeded", result: { success: true } });
  });

  it("rejects a commandlet that exits after only accepting apply", async () => {
    const processRunner: CommandletProcessRunner = async (_executable, args) => {
      const requestPath = argumentPath(args, "-Request=");
      const resultPath = argumentPath(args, "-Result=");
      const request = JSON.parse(await readFile(requestPath, "utf8")) as { id: string };
      await writeFile(resultPath, JSON.stringify({
        jsonrpc: "2.0", id: request.id,
        result: { jobId: "job-queued", requestId: "headless-queued", phase: "Queued", terminal: false }
      }), "utf8");
      return { code: 0, stdout: "", stderr: "" };
    };

    await expect(invokeCommandlet("blueprint_apply", {
      requestId: "headless-queued",
      operations: [{ operation: "asset.create" }]
    }, {
      uproject: "E:/Game/Game.uproject",
      executable: "E:/UE_4.27/Engine/Binaries/Win64/UE4Editor-Cmd.exe",
      processRunner
    })).rejects.toMatchObject({ code: "COMMANDLET_RESULT_MISSING", retryable: true });
  });

  it("rejects process-local job operations in headless mode", async () => {
    await expect(invokeCommandlet("blueprint_job", { action: "wait", jobId: "job-1" }, {
      uproject: "E:/Game/Game.uproject",
      executable: "E:/UE_4.27/Engine/Binaries/Win64/UE4Editor-Cmd.exe"
    })).rejects.toMatchObject({ code: "INVALID_ARGUMENT" });
  });

  it("queries the original requestId after a lost headless write response without replaying apply", async () => {
    const methods: string[] = [];
    const processRunner: CommandletProcessRunner = async (_executable, args) => {
      const requestPath = argumentPath(args, "-Request=");
      const resultPath = argumentPath(args, "-Result=");
      const request = JSON.parse(await readFile(requestPath, "utf8")) as { id: string; method: string };
      methods.push(request.method);
      if (request.method === "blueprint.request") {
        await writeFile(resultPath, JSON.stringify({
          jsonrpc: "2.0",
          id: request.id,
          result: {
            version: 1,
            requestId: "headless-lost",
            method: "blueprint.apply",
            payloadHash: "hash",
            jobId: "job-1",
            state: "terminal",
            terminalPhase: "Failed",
            acceptedAt: "2026-01-01T00:00:00Z",
            updatedAt: "2026-01-01T00:00:01Z",
            interrupted: true,
            recoveryRequired: true,
            result: { partial: true, stateUnknown: true }
          }
        }), "utf8");
      }
      return { code: request.method === "blueprint.apply" ? 3 : 0, stdout: "", stderr: "" };
    };

    await expect(invokeCommandlet("blueprint_apply", {
      requestId: "headless-lost",
      operations: [{ operation: "asset.createBlueprint" }]
    }, {
      uproject: "E:/Game/Game.uproject",
      executable: "E:/UE_4.27/Engine/Binaries/Win64/UE4Editor-Cmd.exe",
      processRunner
    })).resolves.toMatchObject({ requestId: "headless-lost", interrupted: true });
    expect(methods).toEqual(["blueprint.apply", "blueprint.request"]);
  });
});
