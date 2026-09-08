import { mkdir, rm, writeFile } from "node:fs/promises";
import { resolve } from "node:path";
import { afterEach, describe, expect, it } from "vitest";
import { successfulToolResult } from "../../src/mcp/results.js";
import { toolAnnotations, toolSchemas } from "../../src/mcp/tools.js";

const testDirectory = resolve(".codex-unreal-blueprint/viewport-test");
afterEach(async () => { await rm(testDirectory, { recursive: true, force: true }); });

describe("Editor viewport tools", () => {
  it("requires an explicit viewport and keeps camera edits non-destructive but non-read-only", () => {
    expect(toolAnnotations.unreal_viewport_control).toMatchObject({ readOnlyHint: false, destructiveHint: false });
    expect(toolAnnotations.unreal_viewport_capture.readOnlyHint).toBe(true);
    expect(toolSchemas.unreal_viewport_capture.safeParse({ outputPath: "E:/Reports/capture.png" }).success).toBe(false);
    expect(toolSchemas.unreal_viewport_capture.safeParse({ viewportId: "view-1", outputPath: "E:/Reports/capture.png" }).success).toBe(true);
    expect(toolSchemas.unreal_viewport_list.safeParse({ assetPath: "/Game/Test" }).success).toBe(false);
  });

  it("accepts a returned camera snapshot for exact restoration and rejects invalid controls", () => {
    const target = { requestId: "camera-1", viewportId: "view-1" };
    expect(toolSchemas.unreal_viewport_control.safeParse({ ...target, action: "set_camera", camera: {
      location: { x: 100, y: 0, z: 20 }, lookAt: { x: 0, y: 0, z: 0 },
      rotation: { pitch: -10, yaw: 180, roll: 0 }, orthoZoom: 1000, fieldOfView: 90
    } }).success).toBe(true);
    for (const factor of [0, -1, Number.POSITIVE_INFINITY, Number.NaN]) {
      expect(toolSchemas.unreal_viewport_control.safeParse({ ...target, action: "zoom", factor }).success).toBe(false);
    }
    expect(toolSchemas.unreal_viewport_control.safeParse({ ...target, action: "zoom", factor: 0.5, delta: {} }).success).toBe(false);
    expect(toolSchemas.unreal_viewport_control.safeParse({ ...target, action: "set_camera", camera: {} }).success).toBe(false);
    expect(toolSchemas.unreal_viewport_control.safeParse({ action: "activate", viewportId: "view-1" }).success).toBe(false);
    expect(toolSchemas.unreal_viewport_control.safeParse({ ...target, action: "pan", delta: { x: 1, y: 2 } }).success).toBe(false);
    expect(toolSchemas.unreal_viewport_control.safeParse({ requestId: "open-1", action: "open", assetPath: "/Game/Test.Test" }).success).toBe(true);
  });

  it("returns the saved PNG as native MCP image content and preserves capture evidence", async () => {
    const png = Buffer.from("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/l9sAAAAASUVORK5CYII=", "base64");
    await mkdir(testDirectory, { recursive: true });
    const filePath = resolve(testDirectory, "capture.png");
    await writeFile(filePath, png);
    const metadata = { filePath, width: 1, height: 1, byteLength: png.length, mimeType: "image/png", evidence: "editor-viewport-pixels" };
    const result = await successfulToolResult("unreal_viewport_capture", metadata);
    expect(result.structuredContent).toEqual({ result: metadata });
    expect(result.content[1]).toEqual({ type: "image", mimeType: "image/png", data: png.toString("base64") });
    await expect(successfulToolResult("unreal_viewport_capture", { ...metadata, width: 2 })).rejects.toMatchObject({ code: "INVALID_RESPONSE" });
    await writeFile(filePath, "not a PNG");
    await expect(successfulToolResult("unreal_viewport_capture", metadata)).rejects.toMatchObject({ code: "INVALID_RESPONSE" });
  });

  it("does not turn ordinary tool results into file reads or fabricated image success", async () => {
    const result = await successfulToolResult("unreal_status", { filePath: "missing.png" });
    expect(result.content).toHaveLength(1);
    await expect(successfulToolResult("unreal_viewport_capture", { filePath: "missing.png" })).rejects.toMatchObject({ code: "INVALID_RESPONSE" });
  });
});
