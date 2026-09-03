import { randomUUID } from "node:crypto";
import { rpcMethodForTool, type ToolName } from "./contracts.js";
import { ERROR_CODES, UnrealBlueprintError } from "./errors.js";
import type { JsonObject } from "./json.js";
import { isRpcFailure, parseRpcMessage, rpcFailureToError, type RpcRequest, type RpcSuccess } from "./protocol.js";

/** UTF-8 request-file protocol consumed by -run=PiUnrealBlueprint. */
export type CommandletRequest = RpcRequest;
/** UTF-8 result-file protocol emitted by -run=PiUnrealBlueprint. */
export type CommandletSuccess = RpcSuccess;

export function createCommandletRpcRequest(method: string, params: JsonObject, id = randomUUID()): CommandletRequest {
  if (method.trim().length === 0) throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, "Commandlet RPC method must not be empty");
  return { jsonrpc: "2.0", id, method, params };
}

export function createCommandletRequest(tool: ToolName, params: JsonObject, id = randomUUID()): CommandletRequest {
  return createCommandletRpcRequest(rpcMethodForTool(tool), params, id);
}

export function parseCommandletResult(raw: string, expectedId: string): CommandletSuccess {
  let value: unknown;
  try {
    value = JSON.parse(raw);
  } catch (cause) {
    throw new UnrealBlueprintError(ERROR_CODES.INVALID_RESPONSE, "Commandlet result is not valid UTF-8 JSON", { cause });
  }
  const message = parseRpcMessage(value);
  if (!("id" in message) || "method" in message) {
    throw new UnrealBlueprintError(ERROR_CODES.INVALID_RESPONSE, "Commandlet result is not a JSON-RPC response");
  }
  if (message.id !== expectedId) {
    throw new UnrealBlueprintError(ERROR_CODES.INVALID_RESPONSE, `Commandlet result id does not match request id '${expectedId}'`);
  }
  if (isRpcFailure(message)) throw rpcFailureToError(message);
  return message;
}
