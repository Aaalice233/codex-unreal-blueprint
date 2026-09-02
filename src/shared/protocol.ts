import { ERROR_CODES, UnrealBlueprintError, type ErrorContext } from "./errors.js";
import { assertJsonValue, isJsonObject, type JsonObject, type JsonValue } from "./json.js";

export const JSON_RPC_VERSION = "2.0" as const;
export const CLIENT_PROTOCOL_VERSION = "1.0.0";
export type RpcId = string | number;

export interface ProtocolVersion {
  readonly major: number;
  readonly minor: number;
  readonly patch: number;
  readonly prerelease?: string;
}

const SEMVER_PATTERN = /^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$/;

export function parseProtocolVersion(value: string): ProtocolVersion {
  const match = SEMVER_PATTERN.exec(value);
  if (match === null) {
    throw new UnrealBlueprintError(ERROR_CODES.PROTOCOL_MISMATCH, `Protocol version is not valid SemVer: ${value}`);
  }
  const major = Number(match[1]);
  const minor = Number(match[2]);
  const patch = Number(match[3]);
  if (![major, minor, patch].every(Number.isSafeInteger)) {
    throw new UnrealBlueprintError(ERROR_CODES.PROTOCOL_MISMATCH, `Protocol version exceeds safe SemVer range: ${value}`);
  }
  const prerelease = match[4];
  if (prerelease?.split(".").some((identifier) => /^\d+$/.test(identifier) && identifier.length > 1 && identifier.startsWith("0")) === true) {
    throw new UnrealBlueprintError(ERROR_CODES.PROTOCOL_MISMATCH, `Protocol version is not valid SemVer: ${value}`);
  }
  return {
    major,
    minor,
    patch,
    ...(prerelease === undefined ? {} : { prerelease })
  };
}

export function negotiateProtocolVersion(clientVersion: string, editorVersion: string): string {
  const client = parseProtocolVersion(clientVersion);
  const editor = parseProtocolVersion(editorVersion);
  if (client.major !== editor.major) {
    throw new UnrealBlueprintError(
      ERROR_CODES.PROTOCOL_MISMATCH,
      `Incompatible protocol versions: client ${clientVersion}, Editor ${editorVersion}`,
      { context: { details: { clientVersion, editorVersion } } }
    );
  }
  return editorVersion;
}

export interface RpcRequest {
  readonly jsonrpc: typeof JSON_RPC_VERSION;
  readonly id: RpcId;
  readonly method: string;
  readonly params?: JsonObject;
}

export interface RpcNotification {
  readonly jsonrpc: typeof JSON_RPC_VERSION;
  readonly method: string;
  readonly params?: JsonObject;
}

export interface RpcSuccess {
  readonly jsonrpc: typeof JSON_RPC_VERSION;
  readonly id: RpcId;
  readonly result: JsonValue;
}

export interface RpcErrorData extends JsonObject {
  readonly stableCode?: string;
  readonly retryable?: boolean;
  readonly assetPath?: string;
  readonly operationIndex?: number;
  readonly ueCallsite?: string;
  readonly requestId?: string;
  readonly jobId?: string;
  readonly compilerMessages?: JsonValue[];
}

export interface RpcFailure {
  readonly jsonrpc: typeof JSON_RPC_VERSION;
  readonly id: RpcId | null;
  readonly error: {
    readonly code: number;
    readonly message: string;
    readonly data?: RpcErrorData;
  };
}

export type RpcResponse = RpcSuccess | RpcFailure;
export type RpcMessage = RpcRequest | RpcNotification | RpcResponse;

function ownKeysOnly(value: JsonObject, keys: readonly string[]): boolean {
  return Object.keys(value).every((key) => keys.includes(key));
}

function isRpcId(value: unknown): value is RpcId {
  return typeof value === "string" || (typeof value === "number" && Number.isSafeInteger(value));
}

function validateRpcErrorData(data: JsonObject): void {
  const stringFields = ["stableCode", "assetPath", "ueCallsite", "requestId", "jobId"] as const;
  for (const field of stringFields) {
    if (data[field] !== undefined && typeof data[field] !== "string") {
      throw new UnrealBlueprintError(ERROR_CODES.INVALID_RESPONSE, `JSON-RPC error data.${field} must be a string`);
    }
  }
  if (typeof data.stableCode === "string" && data.stableCode.length === 0) {
    throw new UnrealBlueprintError(ERROR_CODES.INVALID_RESPONSE, "JSON-RPC error data.stableCode must not be empty");
  }
  if (data.retryable !== undefined && typeof data.retryable !== "boolean") {
    throw new UnrealBlueprintError(ERROR_CODES.INVALID_RESPONSE, "JSON-RPC error data.retryable must be a boolean");
  }
  if (data.operationIndex !== undefined && (!Number.isSafeInteger(data.operationIndex) || (data.operationIndex as number) < 0)) {
    throw new UnrealBlueprintError(ERROR_CODES.INVALID_RESPONSE, "JSON-RPC error data.operationIndex must be a non-negative integer");
  }
  if (data.compilerMessages !== undefined && !Array.isArray(data.compilerMessages)) {
    throw new UnrealBlueprintError(ERROR_CODES.INVALID_RESPONSE, "JSON-RPC error data.compilerMessages must be an array");
  }
}

export function parseRpcMessage(value: unknown): RpcMessage {
  assertJsonValue(value);
  if (!isJsonObject(value) || value.jsonrpc !== JSON_RPC_VERSION) {
    throw new UnrealBlueprintError(ERROR_CODES.INVALID_RESPONSE, "Message is not JSON-RPC 2.0");
  }

  if (typeof value.method === "string") {
    if (!ownKeysOnly(value, ["jsonrpc", "id", "method", "params"])) {
      throw new UnrealBlueprintError(ERROR_CODES.INVALID_RESPONSE, "JSON-RPC request contains unknown fields");
    }
    if (value.params !== undefined && !isJsonObject(value.params)) {
      throw new UnrealBlueprintError(ERROR_CODES.INVALID_RESPONSE, "JSON-RPC params must be an object");
    }
    if (value.id === undefined) return value as unknown as RpcNotification;
    if (!isRpcId(value.id)) throw new UnrealBlueprintError(ERROR_CODES.INVALID_RESPONSE, "Invalid JSON-RPC request id");
    return value as unknown as RpcRequest;
  }

  if (!ownKeysOnly(value, ["jsonrpc", "id", "result", "error"])) {
    throw new UnrealBlueprintError(ERROR_CODES.INVALID_RESPONSE, "JSON-RPC response contains unknown fields");
  }
  if (value.result !== undefined && value.error === undefined && isRpcId(value.id)) {
    return value as unknown as RpcSuccess;
  }
  if (isJsonObject(value.error) && (isRpcId(value.id) || value.id === null)) {
    if (!ownKeysOnly(value.error, ["code", "message", "data"]) || !Number.isInteger(value.error.code) || typeof value.error.message !== "string") {
      throw new UnrealBlueprintError(ERROR_CODES.INVALID_RESPONSE, "Malformed JSON-RPC error object");
    }
    if (value.error.data !== undefined) {
      if (!isJsonObject(value.error.data)) {
        throw new UnrealBlueprintError(ERROR_CODES.INVALID_RESPONSE, "JSON-RPC error data must be an object");
      }
      validateRpcErrorData(value.error.data);
    }
    return value as unknown as RpcFailure;
  }
  throw new UnrealBlueprintError(ERROR_CODES.INVALID_RESPONSE, "Malformed JSON-RPC response");
}

export function isRpcFailure(response: RpcResponse): response is RpcFailure {
  return "error" in response;
}

export function rpcFailureToError(response: RpcFailure): UnrealBlueprintError {
  const data = response.error.data;
  const context: ErrorContext = {
    ...(typeof data?.assetPath === "string" ? { assetPath: data.assetPath } : {}),
    ...(typeof data?.operationIndex === "number" ? { operationIndex: data.operationIndex } : {}),
    ...(typeof data?.ueCallsite === "string" ? { ueCallsite: data.ueCallsite } : {}),
    ...(typeof data?.requestId === "string" ? { requestId: data.requestId } : {}),
    ...(typeof data?.jobId === "string" ? { jobId: data.jobId } : {}),
    ...(Array.isArray(data?.compilerMessages) ? { compilerMessages: data.compilerMessages } : {}),
    ...{ details: { rpcCode: response.error.code, ...(data ?? {}) } }
  };
  return new UnrealBlueprintError(
    typeof data?.stableCode === "string" ? data.stableCode : ERROR_CODES.RPC_ERROR,
    response.error.message,
    {
      retryable: data?.retryable === true,
      ...(Object.keys(context).length === 0 ? {} : { context })
    }
  );
}
