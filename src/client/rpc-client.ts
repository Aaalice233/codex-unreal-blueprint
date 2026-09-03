import { createConnection, type Socket } from "node:net";
import { ERROR_CODES, UnrealBlueprintError } from "../shared/errors.js";
import { encodeFrame, FrameDecoder, type FramingLimits } from "../shared/framing.js";
import { assertJsonValue, isJsonObject, type JsonObject, type JsonValue } from "../shared/json.js";
import {
  CLIENT_PROTOCOL_VERSION,
  JSON_RPC_VERSION,
  isRpcFailure,
  negotiateProtocolVersion,
  parseRpcMessage,
  rpcFailureToError,
  type RpcId,
  type RpcNotification,
  type RpcResponse
} from "../shared/protocol.js";
import type { EditorSession } from "./session-discovery.js";

export interface RpcClientOptions extends FramingLimits {
  readonly connectTimeoutMs?: number;
  readonly requestTimeoutMs?: number;
}

interface PendingRequest {
  readonly resolve: (value: JsonValue) => void;
  readonly reject: (error: unknown) => void;
  readonly timeout: NodeJS.Timeout;
  readonly removeAbortListener: () => void;
}

export type NotificationHandler = (notification: RpcNotification) => void;

function requireTimeout(value: number, name: string): number {
  if (!Number.isSafeInteger(value) || value < 1) {
    throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, `${name} must be a positive integer`);
  }
  return value;
}

export class RpcClient {
  readonly #socket: Socket;
  readonly #decoder: FrameDecoder;
  readonly #defaultTimeoutMs: number;
  readonly #framingLimits: FramingLimits;
  readonly #pending = new Map<RpcId, PendingRequest>();
  readonly #notificationHandlers = new Set<NotificationHandler>();
  #nextId = 1;
  #closed = false;
  negotiatedProtocolVersion: string | undefined;

  private constructor(socket: Socket, options: RpcClientOptions) {
    this.#socket = socket;
    this.#decoder = new FrameDecoder(options);
    this.#framingLimits = options;
    this.#defaultTimeoutMs = requireTimeout(options.requestTimeoutMs ?? 15_000, "requestTimeoutMs");
    socket.on("data", (chunk) => this.#onData(chunk));
    socket.on("error", (error) => this.#closeWithError(new UnrealBlueprintError(ERROR_CODES.CONNECTION_CLOSED, error.message, { retryable: true, cause: error })));
    socket.on("close", () => {
      if (this.#closed) return;
      try {
        this.#decoder.finish();
        this.#closeWithError(new UnrealBlueprintError(ERROR_CODES.CONNECTION_CLOSED, "Unreal Editor connection closed", { retryable: true }));
      } catch (error) {
        this.#closeWithError(error);
      }
    });
  }

  static async connect(session: EditorSession, options: RpcClientOptions = {}): Promise<RpcClient> {
    negotiateProtocolVersion(CLIENT_PROTOCOL_VERSION, session.protocolVersion);
    const timeoutMs = requireTimeout(options.connectTimeoutMs ?? 3_000, "connectTimeoutMs");
    const socket = await new Promise<Socket>((resolve, reject) => {
      const candidate = createConnection({ host: session.host, port: session.port });
      const onError = (error: Error): void => {
        clearTimeout(timeout);
        reject(new UnrealBlueprintError(ERROR_CODES.CONNECTION_FAILED, `Cannot connect to Unreal Editor at ${session.host}:${session.port}: ${error.message}`, { retryable: true, cause: error }));
      };
      const timeout = setTimeout(() => {
        candidate.destroy();
        reject(new UnrealBlueprintError(ERROR_CODES.CONNECTION_FAILED, `Timed out connecting to ${session.host}:${session.port}`, { retryable: true }));
      }, timeoutMs);
      candidate.once("connect", () => {
        clearTimeout(timeout);
        candidate.removeListener("error", onError);
        resolve(candidate);
      });
      candidate.once("error", onError);
    });

    let client: RpcClient;
    try {
      client = new RpcClient(socket, options);
    } catch (error) {
      socket.destroy();
      throw error;
    }
    try {
      const result = await client.request("session.authenticate", {
        editorSessionId: session.editorSessionId,
        authToken: session.authToken,
        protocolVersion: CLIENT_PROTOCOL_VERSION,
        client: "codex-unreal-blueprint"
      });
      if (!isJsonObject(result) || result.authenticated !== true) {
        throw new UnrealBlueprintError(ERROR_CODES.AUTHENTICATION_FAILED, "Editor rejected session authentication", {
          context: { editorSessionId: session.editorSessionId }
        });
      }
      if (typeof result.protocolVersion !== "string") {
        throw new UnrealBlueprintError(ERROR_CODES.PROTOCOL_MISMATCH, "Authentication response omitted protocolVersion");
      }
      client.negotiatedProtocolVersion = negotiateProtocolVersion(CLIENT_PROTOCOL_VERSION, result.protocolVersion);
      return client;
    } catch (error) {
      client.close();
      throw error;
    }
  }

  onNotification(handler: NotificationHandler): () => void {
    this.#notificationHandlers.add(handler);
    return () => this.#notificationHandlers.delete(handler);
  }

  request(method: string, params: JsonObject = {}, options: { timeoutMs?: number; signal?: AbortSignal } = {}): Promise<JsonValue> {
    if (this.#closed) return Promise.reject(new UnrealBlueprintError(ERROR_CODES.CONNECTION_CLOSED, "Unreal Editor connection is closed", { retryable: true }));
    if (method.trim().length === 0) return Promise.reject(new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, "RPC method must not be empty"));
    let timeoutMs: number;
    try {
      assertJsonValue(params);
      timeoutMs = requireTimeout(options.timeoutMs ?? this.#defaultTimeoutMs, "timeoutMs");
    } catch (error) {
      return Promise.reject(error instanceof UnrealBlueprintError
        ? error
        : new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, error instanceof Error ? error.message : "Invalid RPC parameters", { cause: error }));
    }
    if (options.signal?.aborted) {
      return Promise.reject(new UnrealBlueprintError(ERROR_CODES.REQUEST_ABORTED, `RPC request aborted: ${method}`));
    }
    const id = this.#nextId++;
    let frame: Buffer;
    try {
      frame = encodeFrame({ jsonrpc: JSON_RPC_VERSION, id, method, params }, this.#framingLimits);
    } catch (error) {
      return Promise.reject(error);
    }
    return new Promise<JsonValue>((resolve, reject) => {
      const onAbort = (): void => {
        this.#settle(id, new UnrealBlueprintError(ERROR_CODES.REQUEST_ABORTED, `RPC request aborted: ${method}`), true);
      };
      const timeout = setTimeout(() => {
        this.#settle(id, new UnrealBlueprintError(ERROR_CODES.REQUEST_TIMEOUT, `RPC request timed out after ${timeoutMs}ms: ${method}`, {
          retryable: true,
          context: { details: { method, timeoutMs } }
        }), true);
      }, timeoutMs);
      const signal = options.signal;
      const removeAbortListener = (): void => signal?.removeEventListener("abort", onAbort);
      this.#pending.set(id, { resolve, reject, timeout, removeAbortListener });
      signal?.addEventListener("abort", onAbort, { once: true });
      try {
        this.#socket.write(frame);
      } catch (error) {
        this.#settle(id, new UnrealBlueprintError(ERROR_CODES.CONNECTION_CLOSED, `Failed to send RPC request: ${method}`, {
          retryable: true,
          cause: error
        }), true);
      }
    });
  }

  close(): void {
    if (this.#closed) return;
    this.#closed = true;
    this.#socket.destroy();
    this.#rejectAll(new UnrealBlueprintError(ERROR_CODES.CONNECTION_CLOSED, "Unreal Editor connection closed"));
  }

  #onData(chunk: Buffer): void {
    try {
      for (const value of this.#decoder.push(chunk)) {
        const message = parseRpcMessage(value);
        if ("method" in message) {
          if (!("id" in message)) for (const handler of this.#notificationHandlers) handler(message);
          continue;
        }
        this.#handleResponse(message);
      }
    } catch (error) {
      this.#closeWithError(error instanceof UnrealBlueprintError ? error : new UnrealBlueprintError(ERROR_CODES.INVALID_RESPONSE, "Failed to decode Unreal Editor response", { cause: error }));
    }
  }

  #handleResponse(response: RpcResponse): void {
    if (response.id === null) return;
    const pending = this.#pending.get(response.id);
    if (pending === undefined) return;
    this.#pending.delete(response.id);
    clearTimeout(pending.timeout);
    pending.removeAbortListener();
    if (isRpcFailure(response)) pending.reject(rpcFailureToError(response));
    else pending.resolve(response.result);
  }

  #settle(id: RpcId, value: JsonValue | unknown, reject: boolean): void {
    const pending = this.#pending.get(id);
    if (pending === undefined) return;
    this.#pending.delete(id);
    clearTimeout(pending.timeout);
    pending.removeAbortListener();
    if (reject) pending.reject(value);
    else pending.resolve(value as JsonValue);
  }

  #closeWithError(error: unknown): void {
    if (this.#closed) return;
    this.#closed = true;
    this.#socket.destroy();
    this.#rejectAll(error);
  }

  #rejectAll(error: unknown): void {
    for (const [id] of this.#pending) this.#settle(id, error, true);
  }
}
