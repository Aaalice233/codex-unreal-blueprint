import { assertWriteRequestId, rpcMethodForTool, type ToolName } from "../shared/contracts.js";
import { ERROR_CODES, UnrealBlueprintError } from "../shared/errors.js";
import type { JsonObject, JsonValue } from "../shared/json.js";
import { RpcClient, type RpcClientOptions } from "./rpc-client.js";
import {
  parseJobProgress,
  parseJobSnapshot,
  parseRequestJournalRecord,
  parseRequestJournalStatus,
  type JobProgress,
  type JobSnapshot,
  type RequestJournalRecord,
  type RequestJournalStatus
} from "./jobs.js";
import { selectSession, type DiscoveryOptions, type EditorSession, type SessionQuery } from "./session-discovery.js";

export interface UnrealClientOptions {
  readonly session?: SessionQuery;
  readonly discovery?: DiscoveryOptions;
  readonly rpc?: RpcClientOptions;
  readonly signal?: AbortSignal;
}

export class UnrealClient {
  readonly session: EditorSession;
  readonly #rpc: RpcClient;

  private constructor(session: EditorSession, rpc: RpcClient) {
    this.session = session;
    this.#rpc = rpc;
  }

  static async connect(options: UnrealClientOptions = {}): Promise<UnrealClient> {
    const session = await selectSession(options.session, options.discovery, options.signal);
    const rpc = await RpcClient.connect(session, options.rpc, options.signal);
    return new UnrealClient(session, rpc);
  }

  async invoke(toolName: ToolName, params: JsonObject, options: { signal?: AbortSignal; timeoutMs?: number } = {}): Promise<JsonValue> {
    if (toolName === "blueprint_apply") assertWriteRequestId(params, toolName);
    if (toolName === "blueprint_job" && params.action === "cancel" && typeof params.jobId !== "string") {
      throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, "blueprint_job cancel requires jobId");
    }
    return this.#rpc.request(rpcMethodForTool(toolName), params, options);
  }

  async getJob(jobId: string): Promise<JobSnapshot> {
    if (jobId.trim().length === 0) throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, "jobId must not be empty");
    return parseJobSnapshot(await this.invoke("blueprint_job", { action: "query", jobId }));
  }

  async waitJob(jobId: string, options: { timeoutMs?: number; signal?: AbortSignal } = {}): Promise<JobSnapshot> {
    if (jobId.trim().length === 0) throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, "jobId must not be empty");
    const timeoutMs = options.timeoutMs ?? 30_000;
    if (!Number.isSafeInteger(timeoutMs) || timeoutMs < 0 || timeoutMs > 600_000) {
      throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, "job wait timeoutMs must be an integer between 0 and 600000");
    }
    return parseJobSnapshot(await this.invoke("blueprint_job", { action: "wait", jobId, timeoutMs }, {
      ...(options.signal === undefined ? {} : { signal: options.signal }),
      timeoutMs: Math.max(1, timeoutMs + 1_000)
    }));
  }

  async queryWriteRequest(
    requestId: string,
    options: { signal?: AbortSignal; timeoutMs?: number } = {}
  ): Promise<RequestJournalRecord> {
    if (requestId.trim().length === 0) throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, "requestId must not be empty");
    const record = parseRequestJournalRecord(await this.#rpc.request("blueprint.request", { action: "query", requestId }, options));
    if (record.requestId !== requestId) {
      throw new UnrealBlueprintError(ERROR_CODES.INVALID_RESPONSE, "Editor returned a journal record for a different requestId");
    }
    return record;
  }

  async getRequestJournalStatus(options: { signal?: AbortSignal; timeoutMs?: number } = {}): Promise<RequestJournalStatus> {
    return parseRequestJournalStatus(await this.#rpc.request("blueprint.request", { action: "status" }, options));
  }

  async cancelJob(jobId: string): Promise<JobSnapshot> {
    if (jobId.trim().length === 0) throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, "jobId must not be empty");
    return parseJobSnapshot(await this.invoke("blueprint_job", { action: "cancel", jobId }));
  }

  onJobProgress(handler: (progress: JobProgress) => void): () => void {
    return this.#rpc.onNotification((notification) => {
      if (notification.method === "blueprint.job.progress" && notification.params !== undefined) {
        handler(parseJobProgress(notification.params));
      }
    });
  }

  close(): void {
    this.#rpc.close();
  }
}

export interface RecoverWriteOptions {
  readonly signal?: AbortSignal;
  readonly timeoutMs?: number;
  readonly queryAttempts?: number;
  readonly queryDelayMs?: number;
  readonly queryTimeoutMs?: number;
}

export type WriteToolName = "blueprint_apply";

function requireRecoveryInteger(value: number, name: string, minimum: number, maximum: number): number {
  if (!Number.isSafeInteger(value) || value < minimum || value > maximum) {
    throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT,
      `${name} must be an integer between ${minimum} and ${maximum}`);
  }
  return value;
}

async function waitForRecoveryDelay(delayMs: number, signal?: AbortSignal): Promise<void> {
  if (signal?.aborted) throw new UnrealBlueprintError(ERROR_CODES.REQUEST_ABORTED, "Write recovery query aborted");
  if (delayMs === 0) return;
  await new Promise<void>((resolve, reject) => {
    const onAbort = (): void => {
      clearTimeout(timeout);
      reject(new UnrealBlueprintError(ERROR_CODES.REQUEST_ABORTED, "Write recovery query aborted"));
    };
    const timeout = setTimeout(() => {
      signal?.removeEventListener("abort", onAbort);
      resolve();
    }, delayMs);
    signal?.addEventListener("abort", onAbort, { once: true });
  });
}

export async function invokeWriteWithRecovery(
  clientOptions: UnrealClientOptions,
  toolName: WriteToolName,
  params: JsonObject,
  options: RecoverWriteOptions = {}
): Promise<JsonValue> {
  assertWriteRequestId(params, toolName);
  const requestId = params.requestId as string;
  const attempts = requireRecoveryInteger(options.queryAttempts ?? 3, "queryAttempts", 1, 20);
  const delayMs = requireRecoveryInteger(options.queryDelayMs ?? 100, "queryDelayMs", 0, 10_000);
  const queryTimeoutMs = requireRecoveryInteger(options.queryTimeoutMs ?? 5_000, "queryTimeoutMs", 1, 30_000);
  const expectedMethod = rpcMethodForTool(toolName);
  const client = await UnrealClient.connect(clientOptions);
  try {
    return await client.invoke(toolName, params, {
      ...(options.signal === undefined ? {} : { signal: options.signal }),
      ...(options.timeoutMs === undefined ? {} : { timeoutMs: options.timeoutMs })
    });
  } catch (error) {
    const failure = error instanceof UnrealBlueprintError ? error : new UnrealBlueprintError(ERROR_CODES.INTERNAL, "Write request failed", { cause: error });
    if (!failure.retryable) throw failure;
    client.close();

    let lastQueryError: unknown;
    for (let attempt = 0; attempt < attempts; attempt += 1) {
      if (options.signal?.aborted) throw new UnrealBlueprintError(ERROR_CODES.REQUEST_ABORTED, "Write recovery query aborted");
      if (attempt > 0) await waitForRecoveryDelay(delayMs, options.signal);
      let queryClient: UnrealClient | undefined;
      try {
        queryClient = await UnrealClient.connect(clientOptions);
        const record = await queryClient.queryWriteRequest(requestId, {
          ...(options.signal === undefined ? {} : { signal: options.signal }),
          timeoutMs: queryTimeoutMs
        });
        if (record.method !== expectedMethod) {
          throw new UnrealBlueprintError(ERROR_CODES.INVALID_RESPONSE,
            `Editor returned requestId '${requestId}' for method '${record.method}', expected '${expectedMethod}'`);
        }
        return record as unknown as JsonValue;
      } catch (queryError) {
        if (queryError instanceof UnrealBlueprintError && !queryError.retryable) throw queryError;
        lastQueryError = queryError;
      } finally {
        queryClient?.close();
      }
    }
    throw new UnrealBlueprintError(ERROR_CODES.REQUEST_TIMEOUT,
      `Write response was lost and requestId '${requestId}' could not be queried; the write was not replayed`, {
        retryable: true,
        context: { requestId },
        cause: lastQueryError ?? failure
      });
  } finally {
    client.close();
  }
}

export async function withUnrealClient<T>(options: UnrealClientOptions, run: (client: UnrealClient) => Promise<T>): Promise<T> {
  const client = await UnrealClient.connect(options);
  try {
    return await run(client);
  } finally {
    client.close();
  }
}
