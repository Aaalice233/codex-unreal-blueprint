import { ERROR_CODES, UnrealBlueprintError } from "./errors.js";
import { jsonDepth, type JsonValue } from "./json.js";

export const DEFAULT_MAX_FRAME_BYTES = 8 * 1024 * 1024;
export const DEFAULT_MAX_JSON_DEPTH = 64;
const HEADER_BYTES = 4;
const UTF8_DECODER = new TextDecoder("utf-8", { fatal: true });

export interface FramingLimits {
  readonly maxFrameBytes?: number;
  readonly maxJsonDepth?: number;
}

function requirePositiveInteger(value: number, name: string): number {
  if (!Number.isSafeInteger(value) || value < 1) {
    throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, `${name} must be a positive integer`);
  }
  return value;
}

function resolveLimits(limits: FramingLimits): { maxFrameBytes: number; maxJsonDepth: number } {
  return {
    maxFrameBytes: requirePositiveInteger(limits.maxFrameBytes ?? DEFAULT_MAX_FRAME_BYTES, "maxFrameBytes"),
    maxJsonDepth: requirePositiveInteger(limits.maxJsonDepth ?? DEFAULT_MAX_JSON_DEPTH, "maxJsonDepth")
  };
}

export function encodeFrame(value: JsonValue, limits: FramingLimits = {}): Buffer {
  const { maxFrameBytes, maxJsonDepth } = resolveLimits(limits);
  if (jsonDepth(value) > maxJsonDepth) {
    throw new UnrealBlueprintError(ERROR_CODES.FRAME_MALFORMED, `JSON depth exceeds ${maxJsonDepth}`);
  }
  const payload = Buffer.from(JSON.stringify(value), "utf8");
  if (payload.byteLength === 0 || payload.byteLength > maxFrameBytes) {
    throw new UnrealBlueprintError(ERROR_CODES.FRAME_TOO_LARGE, `Frame is ${payload.byteLength} bytes; limit is ${maxFrameBytes}`);
  }
  const frame = Buffer.allocUnsafe(HEADER_BYTES + payload.byteLength);
  frame.writeUInt32BE(payload.byteLength, 0);
  payload.copy(frame, HEADER_BYTES);
  return frame;
}

export class FrameDecoder {
  readonly #maxFrameBytes: number;
  readonly #maxJsonDepth: number;
  #buffer = Buffer.alloc(0);

  constructor(limits: FramingLimits = {}) {
    const resolved = resolveLimits(limits);
    this.#maxFrameBytes = resolved.maxFrameBytes;
    this.#maxJsonDepth = resolved.maxJsonDepth;
  }

  push(chunk: Uint8Array): JsonValue[] {
    if (chunk.byteLength === 0) return [];
    this.#buffer = this.#buffer.byteLength === 0
      ? Buffer.from(chunk)
      : Buffer.concat([this.#buffer, chunk], this.#buffer.byteLength + chunk.byteLength);
    const messages: JsonValue[] = [];

    while (this.#buffer.byteLength >= HEADER_BYTES) {
      const length = this.#buffer.readUInt32BE(0);
      if (length === 0) throw new UnrealBlueprintError(ERROR_CODES.FRAME_MALFORMED, "Zero-length frame is invalid");
      if (length > this.#maxFrameBytes) {
        throw new UnrealBlueprintError(ERROR_CODES.FRAME_TOO_LARGE, `Frame declares ${length} bytes; limit is ${this.#maxFrameBytes}`);
      }
      if (this.#buffer.byteLength < HEADER_BYTES + length) break;
      const payload = this.#buffer.subarray(HEADER_BYTES, HEADER_BYTES + length);
      this.#buffer = this.#buffer.subarray(HEADER_BYTES + length);
      let value: unknown;
      try {
        value = JSON.parse(UTF8_DECODER.decode(payload));
      } catch (error) {
        throw new UnrealBlueprintError(ERROR_CODES.FRAME_MALFORMED, "Frame payload is not valid UTF-8 JSON", { cause: error });
      }
      if (jsonDepth(value as JsonValue) > this.#maxJsonDepth) {
        throw new UnrealBlueprintError(ERROR_CODES.FRAME_MALFORMED, `JSON depth exceeds ${this.#maxJsonDepth}`);
      }
      messages.push(value as JsonValue);
    }
    return messages;
  }

  finish(): void {
    if (this.#buffer.byteLength !== 0) {
      throw new UnrealBlueprintError(ERROR_CODES.FRAME_MALFORMED, `Connection ended with ${this.#buffer.byteLength} incomplete frame bytes`);
    }
  }
}
