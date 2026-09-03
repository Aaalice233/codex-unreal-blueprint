import { ERROR_CODES, UnrealBlueprintError } from "./errors.js";
import type { JsonObject } from "./json.js";

export const TOOL_NAMES = [
  "unreal_status",
  "unreal_doctor",
  "unreal_search",
  "blueprint_capabilities",
  "blueprint_inspect",
  "blueprint_validate",
  "blueprint_apply",
  "blueprint_job",
  "blueprint_verify"
] as const;

export type ToolName = (typeof TOOL_NAMES)[number];

const RPC_METHODS: Readonly<Record<ToolName, string>> = {
  unreal_status: "unreal.status",
  unreal_doctor: "unreal.doctor",
  unreal_search: "unreal.search",
  blueprint_capabilities: "blueprint.capabilities",
  blueprint_inspect: "blueprint.inspect",
  blueprint_validate: "blueprint.validate",
  blueprint_apply: "blueprint.apply",
  blueprint_job: "blueprint.job",
  blueprint_verify: "blueprint.verify"
};

export function rpcMethodForTool(toolName: ToolName): string {
  return RPC_METHODS[toolName];
}

export interface Operation extends JsonObject {
  readonly operation: string;
}

export function assertWriteRequestId(params: JsonObject, operationName: string): void {
  if (typeof params.requestId !== "string" || params.requestId.trim().length === 0) {
    throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, `${operationName} requires a non-empty requestId`);
  }
}
