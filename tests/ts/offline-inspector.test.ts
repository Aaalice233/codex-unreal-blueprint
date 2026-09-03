import { access, mkdtemp, mkdir, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { setTimeout as delay } from "node:timers/promises";
import { afterEach, describe, expect, it } from "vitest";
import { findOfflineReferencers } from "../../src/offline/uasset-inspector.js";
import { withStagedAssetCopies } from "../../src/offline/asset-staging.js";

const roots: string[] = [];

afterEach(async () => {
  await Promise.all(roots.splice(0).map((root) => rm(root, { recursive: true, force: true })));
});

describe("bundled offline UAsset support", () => {
  it("retains package snapshots in a rolling cache without counting companions as assets", async () => {
    const root = await mkdtemp(join(tmpdir(), "codex-unreal-stage-source-"));
    roots.push(root);
    const cache = join(root, "cache");
    const content = join(root, "Content");
    await mkdir(join(content, "FX"), { recursive: true });
    const asset = join(content, "FX", "NS_Test.uasset");
    const companion = join(content, "FX", "NS_Test.uexp");
    await writeFile(asset, "asset", "utf8");
    await writeFile(companion, "companion", "utf8");
    let stagedFile = "";

    const outcome = await withStagedAssetCopies([asset], content, { enabled: true, maxCachedAssets: 2 }, async ([copy]) => {
      expect(copy).toBeDefined();
      stagedFile = copy!.stagedFilePath;
      expect(stagedFile).not.toBe(asset);
      expect(stagedFile.replaceAll("\\", "/")).toMatch(/\/Content\/FX\/NS_Test\.uasset$/);
      expect(await readFile(stagedFile, "utf8")).toBe("asset");
      expect(await readFile(stagedFile.replace(/\.uasset$/i, ".uexp"), "utf8")).toBe("companion");
      return "parsed";
    }, cache);

    expect(outcome).toEqual({
      result: "parsed",
      staging: {
        used: true,
        sourceAssetCount: 1,
        copiedFileCount: 2,
        companionFileCount: 1,
        maxCachedAssets: 2,
        cachedAssetCount: 1,
        evictedAssetCount: 0,
        retention: "rolling-cache",
        scope: "requested-packages-and-companions"
      }
    });
    await expect(access(stagedFile)).resolves.toBeUndefined();
  });

  it("evicts the oldest cached asset and continues copying when the folder is full", async () => {
    const root = await mkdtemp(join(tmpdir(), "codex-unreal-stage-limit-"));
    roots.push(root);
    const cache = join(root, "cache");
    const content = join(root, "Content");
    await mkdir(content, { recursive: true });
    const assets = ["A", "B", "C"].map((name) => join(content, `${name}.uasset`));
    await Promise.all(assets.map((asset, index) => writeFile(asset, `asset-${index}`, "utf8")));
    const stagedFiles: string[] = [];
    let finalOutcome: Awaited<ReturnType<typeof withStagedAssetCopies<string>>> | undefined;
    for (const asset of assets) {
      finalOutcome = await withStagedAssetCopies([asset], content, { enabled: true, maxCachedAssets: 2 }, async ([copy]) => {
        stagedFiles.push(copy!.stagedFilePath);
        return asset;
      }, cache);
      await delay(5);
    }

    expect(finalOutcome?.staging).toMatchObject({
      sourceAssetCount: 1,
      cachedAssetCount: 2,
      evictedAssetCount: 1,
      maxCachedAssets: 2
    });
    await expect(access(stagedFiles[0] as string)).rejects.toThrow();
    await expect(access(stagedFiles[1] as string)).resolves.toBeUndefined();
    await expect(access(stagedFiles[2] as string)).resolves.toBeUndefined();
  });

  it("parses a request larger than the retained-cache limit and trims only after parsing", async () => {
    const root = await mkdtemp(join(tmpdir(), "codex-unreal-stage-batch-"));
    roots.push(root);
    const cache = join(root, "cache");
    const content = join(root, "Content");
    await mkdir(content, { recursive: true });
    const assets = [join(content, "Base.uasset"), join(content, "Target.uasset")];
    await Promise.all(assets.map((asset) => writeFile(asset, "asset", "utf8")));
    const stagedFiles: string[] = [];

    const outcome = await withStagedAssetCopies(assets, content, { enabled: true, maxCachedAssets: 1 }, async (copies) => {
      stagedFiles.push(...copies.map((copy) => copy.stagedFilePath));
      await Promise.all(stagedFiles.map((file) => access(file)));
      return "compared";
    }, cache);

    expect(outcome.staging).toMatchObject({ sourceAssetCount: 2, cachedAssetCount: 1, evictedAssetCount: 1 });
    await expect(access(stagedFiles[0] as string)).rejects.toThrow();
    await expect(access(stagedFiles[1] as string)).resolves.toBeUndefined();
  });

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
