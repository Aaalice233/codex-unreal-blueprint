import { describe, expect, it } from "vitest";
import { DEFAULT_MAX_FRAME_BYTES, encodeFrame, FrameDecoder } from "../../src/shared/framing.js";
import { UnrealBlueprintError } from "../../src/shared/errors.js";

function decode(frame: Buffer, splits: readonly number[]): unknown[] {
  const decoder = new FrameDecoder();
  const output: unknown[] = [];
  let offset = 0;
  for (const length of splits) {
    output.push(...decoder.push(frame.subarray(offset, offset + length)));
    offset += length;
  }
  output.push(...decoder.push(frame.subarray(offset)));
  decoder.finish();
  return output;
}

describe("length-prefixed framing", () => {
  it("decodes a frame split across arbitrary TCP chunks", () => {
    const value = { jsonrpc: "2.0", id: 1, result: { text: "蓝图" } } as const;
    expect(decode(encodeFrame(value), [1, 2, 3, 4])).toEqual([value]);
  });

  it("decodes multiple frames from one chunk", () => {
    const decoder = new FrameDecoder();
    const chunk = Buffer.concat([encodeFrame({ a: 1 }), encodeFrame({ b: 2 })]);
    expect(decoder.push(chunk)).toEqual([{ a: 1 }, { b: 2 }]);
  });

  it("accepts the exact 8 MiB boundary and rejects one byte above it", () => {
    const exactPayload = "x".repeat(DEFAULT_MAX_FRAME_BYTES - 2);
    expect(encodeFrame(exactPayload).readUInt32BE(0)).toBe(DEFAULT_MAX_FRAME_BYTES);
    expect(() => encodeFrame(`${exactPayload}x`)).toThrowError(UnrealBlueprintError);

    const decoder = new FrameDecoder();
    const oversizedHeader = Buffer.alloc(4);
    oversizedHeader.writeUInt32BE(DEFAULT_MAX_FRAME_BYTES + 1);
    expect(() => decoder.push(oversizedHeader)).toThrowError(UnrealBlueprintError);
  });

  it("handles every split around the four-byte header and payload boundary", () => {
    const value = { text: "boundary" } as const;
    const frame = encodeFrame(value);
    for (let split = 0; split <= frame.byteLength; split += 1) {
      const decoder = new FrameDecoder();
      expect([
        ...decoder.push(frame.subarray(0, split)),
        ...decoder.push(frame.subarray(split))
      ]).toEqual([value]);
      decoder.finish();
    }
  });

  it("rejects invalid UTF-8 instead of replacing bytes", () => {
    const decoder = new FrameDecoder();
    expect(() => decoder.push(Buffer.from([0, 0, 0, 1, 0xff]))).toThrow(/UTF-8 JSON/);
  });

  it("reports an incomplete frame when the stream closes", () => {
    const decoder = new FrameDecoder();
    decoder.push(Buffer.from([0, 0, 0, 5, 0x7b]));
    expect(() => decoder.finish()).toThrow(/incomplete frame/);
  });
});
