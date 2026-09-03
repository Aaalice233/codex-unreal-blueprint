import { randomUUID } from "node:crypto";
import { mkdir, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { dirname, resolve, win32 } from "node:path";
import { spawn } from "node:child_process";
import { createCommandletRpcRequest, parseCommandletResult } from "../shared/commandlet.js";
import { assertWriteRequestId, rpcMethodForTool, type ToolName } from "../shared/contracts.js";
import { ERROR_CODES, UnrealBlueprintError } from "../shared/errors.js";
import { assertJsonValue, isJsonObject, type JsonObject, type JsonValue } from "../shared/json.js";

export interface CommandletProcessResult {
  readonly code: number;
  readonly stdout: string;
  readonly stderr: string;
}

export type CommandletProcessRunner = (executable: string, args: readonly string[], timeoutMs: number, signal?: AbortSignal) => Promise<CommandletProcessResult>;

export interface CommandletOptions {
  readonly uproject: string;
  readonly engineRoot?: string;
  readonly executable?: string;
  readonly timeoutMs?: number;
  readonly signal?: AbortSignal;
  readonly processRunner?: CommandletProcessRunner;
}

async function runProcess(executable: string, args: readonly string[], timeoutMs: number, signal?: AbortSignal): Promise<CommandletProcessResult> {
  return new Promise((resolvePromise, reject) => {
    const child = spawn(executable, args, { windowsHide: true, stdio: ["ignore", "pipe", "pipe"] });
    let stdout = "";
    let stderr = "";
    child.stdout.setEncoding("utf8");
    child.stderr.setEncoding("utf8");
    child.stdout.on("data", (chunk: string) => { stdout += chunk; });
    child.stderr.on("data", (chunk: string) => { stderr += chunk; });
    const abort = (): void => {
      child.kill();
    };
    signal?.addEventListener("abort", abort, { once: true });
    const timer = setTimeout(() => child.kill(), timeoutMs);
    child.once("error", (cause) => {
      clearTimeout(timer);
      signal?.removeEventListener("abort", abort);
      reject(new UnrealBlueprintError(ERROR_CODES.COMMANDLET_START_FAILED, `Failed to start UE4Editor-Cmd.exe: ${executable}`, { cause }));
    });
    child.once("exit", (code) => {
      clearTimeout(timer);
      signal?.removeEventListener("abort", abort);
      if (signal?.aborted) {
        reject(new UnrealBlueprintError(ERROR_CODES.REQUEST_ABORTED, "Commandlet was cancelled"));
        return;
      }
      resolvePromise({ code: code ?? -1, stdout, stderr });
    });
  });
}

async function invokeCommandletRpc(method: string, params: JsonObject, options: CommandletOptions): Promise<JsonValue> {
  const engineRoot = options.engineRoot ?? process.env.UE_ENGINE_ROOT;
  if (options.executable === undefined && engineRoot === undefined) {
    throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, "Headless execution requires --engine-root or UE_ENGINE_ROOT");
  }
  const executable = options.executable ?? resolve(engineRoot as string, "Engine/Binaries/Win64/UE4Editor-Cmd.exe");
  if (!win32.isAbsolute(options.uproject) || !options.uproject.toLowerCase().endsWith(".uproject")) {
    throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, "Headless execution requires an absolute .uproject path");
  }
  const timeoutMs = options.timeoutMs ?? 600_000;
  const runDirectory = resolve(tmpdir(), "pi-unreal-blueprint", randomUUID());
  const requestPath = resolve(runDirectory, "request.json");
  const resultPath = resolve(runDirectory, "result.json");
  const request = createCommandletRpcRequest(method, params);
  await mkdir(dirname(requestPath), { recursive: true });
  await writeFile(requestPath, `${JSON.stringify(request)}\n`, "utf8");
  try {
    const processResult = await (options.processRunner ?? runProcess)(executable, [
      options.uproject,
      "-run=PiUnrealBlueprint",
      `-Request=${requestPath}`,
      `-Result=${resultPath}`,
      "-unattended",
      "-nosplash",
      "-UTF8Output"
    ], timeoutMs, options.signal);
    let raw: string;
    try {
      raw = await readFile(resultPath, "utf8");
    } catch (cause) {
      throw new UnrealBlueprintError(ERROR_CODES.COMMANDLET_RESULT_MISSING, `Commandlet exited with code ${processResult.code} without a readable result`, {
        retryable: true,
        context: { details: { exitCode: processResult.code, stdout: processResult.stdout, stderr: processResult.stderr } }, cause
      });
    }
    const parsed = parseCommandletResult(raw, String(request.id));
    if (processResult.code !== 0) {
      throw new UnrealBlueprintError(ERROR_CODES.COMMANDLET_FAILED, `Commandlet returned exit code ${processResult.code} despite a success envelope`, {
        context: { details: { exitCode: processResult.code, stdout: processResult.stdout, stderr: processResult.stderr } }
      });
    }
    assertJsonValue(parsed.result);
    return parsed.result;
  } finally {
    await rm(runDirectory, { recursive: true, force: true });
  }
}

function assertTerminalHeadlessApply(result: JsonValue): void {
  if (!isJsonObject(result) || result.terminal !== true
    || (result.phase !== "Succeeded" && result.phase !== "Failed" && result.phase !== "Cancelled")) {
    throw new UnrealBlueprintError(ERROR_CODES.COMMANDLET_FAILED,
      "Headless blueprint.apply must return a terminal job snapshot; the write result is not safe to report as complete", {
        retryable: true,
        context: { details: { result } }
      });
  }
}

export async function invokeCommandlet(tool: ToolName, params: JsonObject, options: CommandletOptions): Promise<JsonValue> {
  if (tool === "blueprint_job") {
    throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT,
      "Headless job query/wait/cancel is unavailable because jobs are process-local; blueprint.apply already waits for a terminal result");
  }
  if (tool === "blueprint_apply") assertWriteRequestId(params, tool);
  const method = rpcMethodForTool(tool);
  try {
    const result = await invokeCommandletRpc(method, params, options);
    if (tool === "blueprint_apply") assertTerminalHeadlessApply(result);
    return result;
  } catch (error) {
    const requestId = params.requestId;
    const normalized = error instanceof UnrealBlueprintError ? error : undefined;
    if (tool !== "blueprint_apply" || typeof requestId !== "string" || requestId.trim().length === 0 || normalized?.retryable !== true) {
      throw error;
    }
    try {
      const record = await invokeCommandletRpc("blueprint.request", { action: "query", requestId }, options);
      if (!isJsonObject(record) || record.requestId !== requestId || record.method !== method
        || record.state !== "terminal"
        || (record.terminalPhase !== "Succeeded" && record.terminalPhase !== "Failed" && record.terminalPhase !== "Cancelled")) {
        throw new UnrealBlueprintError(ERROR_CODES.INVALID_RESPONSE, "Commandlet returned a non-terminal or mismatched request journal record");
      }
      return record;
    } catch (queryError) {
      throw new UnrealBlueprintError(ERROR_CODES.COMMANDLET_RESULT_MISSING,
        `Headless write response was lost and requestId '${requestId}' could not be queried; the write was not replayed`, {
          retryable: true,
          context: { requestId },
          cause: queryError
        });
    }
  }
}
