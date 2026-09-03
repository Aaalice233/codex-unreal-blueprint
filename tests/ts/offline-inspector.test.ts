import { mkdtemp, mkdir, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { afterEach, describe, expect, it } from "vitest";
import { findOfflineReferencers } from "../../src/offline/uasset-inspector.js";

const roots: string[] = [];

afterEach(async () => {
  await Promise.all(roots.splice(0).map((root) => rm(root, { recursive: true, force: true })));
});

describe("bundled offline UAsset support", () => {
  it("finds bounded UTF-8 and UTF-16 serialized reference evidence", async () => {
    const root = await mkdtemp(join(tmpdir(), "codex-unreal-offline-"));
    roots.push(root);
    const content = join(root, "Content");
    await mkdir(join(content, "Refs"), { recursive: true });
    const target = join(content, "Target.uasset");
    await writeFile(target, Buffer.from("target"));
    await writeFile(join(content, "Refs", "Utf8.uasset"), Buffer.from("prefix/Game/Targetsuffix", "utf8"));
    await writeFile(join(content, "Refs", "Utf16.uexp"), Buffer.from("prefix/Game/Targetsuffix", "utf16le"));
    await writeFile(join(content, "Refs", "Other.uasset"), Buffer.from("/Game/Other", "utf8"));

    const result = await findOfflineReferencers(target, content, content, 1, 1);
    expect(result).toMatchObject({
      mode: "offline",
      evidence: "binary-reference-match",
      editable: false,
      packagePath: "/Game/Target"
    });
    expect(result).toMatchObject({
      referencers: { total: 2, items: expect.any(Array) }
    });
    expect((result as { referencers: { items: unknown[] } }).referencers.items).toHaveLength(1);
  });
});
