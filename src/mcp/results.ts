import { readFile } from "node:fs/promises";
import type { CallToolResult } from "@modelcontextprotocol/sdk/types.js";
import type { ToolName } from "../shared/contracts.js";
import { isJsonObject, type JsonValue } from "../shared/json.js";
import { ERROR_CODES, UnrealBlueprintError } from "../shared/errors.js";

export async function successfulToolResult(name: ToolName, result: JsonValue): Promise<CallToolResult> {
  const response: CallToolResult = {
    content: [{ type: "text", text: JSON.stringify(result, null, 2) }],
    structuredContent: { result }
  };
  if (name !== "unreal_viewport_capture") return response;
  if (!isJsonObject(result) || typeof result.filePath !== "string" || result.mimeType !== "image/png"
    || !Number.isSafeInteger(result.width) || !Number.isSafeInteger(result.height)) {
    throw new UnrealBlueprintError(ERROR_CODES.INVALID_RESPONSE, "Editor capture did not return a PNG path and dimensions");
  }
  const bytes = await readFile(result.filePath);
  if (bytes.length < 24 || !bytes.subarray(0, 8).equals(Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]))
    || bytes.readUInt32BE(16) !== result.width || bytes.readUInt32BE(20) !== result.height
    || bytes.length !== result.byteLength) {
    throw new UnrealBlueprintError(ERROR_CODES.INVALID_RESPONSE, "Saved viewport PNG does not match the Editor capture metadata");
  }
  response.content.push({ type: "image", mimeType: "image/png", data: bytes.toString("base64") });
  return response;
}
