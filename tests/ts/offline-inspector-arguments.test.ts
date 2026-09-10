import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { afterEach, describe, expect, it, vi } from "vitest";

const run = vi.hoisted(() => ({ arguments: [] as string[] }));
vi.mock("node:child_process", () => ({
  execFile: (_file: string, args: string[], _options: unknown,
    callback: (error: null, result: { stdout: string; stderr: string }) => void) => {
    run.arguments = args;
    callback(null, { stdout: '{"Exports":[]}', stderr: "" });
  }
}));
import { inspectOfflineAsset } from "../../src/offline/uasset-inspector.js";

const roots: string[] = [];
afterEach(async () => {
  await Promise.all(roots.splice(0).map((root) => rm(root, { recursive: true, force: true })));
});

describe("offline inspector PowerShell argument boundary", () => {
  it("passes multiple search terms as the single -File parameter consumed by the script", async () => {
    const root = await mkdtemp(join(tmpdir(), "codex-offline-args-"));
    roots.push(root);
    const file = join(root, "Test.uasset");
    await writeFile(file, "fixture", "utf8");
    await expect(inspectOfflineAsset(file, undefined, ["Color_A", "Color_B", "Color_C", "Color_D"]))
      .resolves.toMatchObject({ mode: "offline", editable: false });
    const searchIndex = run.arguments.indexOf("-Search");
    expect(run.arguments.slice(searchIndex)).toEqual(["-Search", "Color_A,Color_B,Color_C,Color_D"]);
  });
});
