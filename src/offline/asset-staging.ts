import { copyFile, mkdir, mkdtemp, readFile, readdir, rm, stat, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { basename, dirname, extname, isAbsolute, join, relative, resolve } from "node:path";
import { setTimeout as delay } from "node:timers/promises";
import { ERROR_CODES, UnrealBlueprintError } from "../shared/errors.js";
import type { JsonValue } from "../shared/json.js";

export const DEFAULT_OFFLINE_STAGING_MAX_CACHED_ASSETS = 64;
export const HARD_OFFLINE_STAGING_MAX_CACHED_ASSETS = 512;

export interface OfflineStagingOptions {
  readonly enabled: boolean;
  readonly maxCachedAssets: number;
}

export interface StagedAssetCopy {
  readonly sourceFilePath: string;
  readonly stagedFilePath: string;
  readonly sourceContentRoot?: string;
  readonly stagedContentRoot?: string;
}

export interface OfflineStagingSummary {
  readonly [key: string]: JsonValue;
  readonly used: boolean;
  readonly sourceAssetCount: number;
  readonly copiedFileCount: number;
  readonly companionFileCount: number;
  readonly maxCachedAssets: number;
  readonly cachedAssetCount: number;
  readonly evictedAssetCount: number;
  readonly retention: "rolling-cache" | "not-required";
  readonly scope: "requested-packages-and-companions";
}

interface CacheEntry {
  readonly path: string;
  readonly assetCount: number;
  readonly modifiedAt: number;
}

const COMPANION_EXTENSIONS = [".uexp", ".ubulk", ".uptnl"] as const;
const LOCK_RETRY_MS = 50;
const LOCK_TIMEOUT_MS = 30_000;
const STALE_LOCK_MS = 60 * 60 * 1_000;

function stagingRoot(): string {
  return process.env.LOCALAPPDATA
    ? join(process.env.LOCALAPPDATA, "CodexUnrealBlueprint", "offline-staging")
    : join(tmpdir(), "CodexUnrealBlueprint", "offline-staging");
}

function inferContentRoot(filePath: string): string | undefined {
  const normalized = resolve(filePath).replaceAll("\\", "/");
  const marker = "/Content/";
  const index = normalized.toLowerCase().indexOf(marker.toLowerCase());
  return index < 0 ? undefined : normalized.slice(0, index + "/Content".length);
}

function relativeInside(root: string, filePath: string): string {
  const value = relative(resolve(root), resolve(filePath));
  if (value === "" || value === ".." || value.startsWith("../") || value.startsWith("..\\") || isAbsolute(value)) {
    throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, `Offline staging asset must be inside contentRoot: ${filePath}`);
  }
  return value;
}

async function existingPackageFiles(assetPath: string): Promise<string[]> {
  const source = resolve(assetPath);
  if (![".uasset", ".umap"].includes(extname(source).toLowerCase())) {
    throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, `Offline staging requires a .uasset or .umap file: ${assetPath}`);
  }
  try {
    const info = await stat(source);
    if (!info.isFile()) throw new Error("not a file");
  } catch (error) {
    throw new UnrealBlueprintError(ERROR_CODES.OFFLINE_STAGING_FAILED, `Cannot stage missing or unreadable asset: ${source}`, { cause: error });
  }
  const stem = source.slice(0, -extname(source).length);
  const files = [source];
  for (const extension of COMPANION_EXTENSIONS) {
    const candidate = `${stem}${extension}`;
    try {
      if ((await stat(candidate)).isFile()) files.push(candidate);
    } catch (error) {
      if (!(error instanceof Error && "code" in error && error.code === "ENOENT")) throw error;
    }
  }
  return files;
}

async function copyStableFile(source: string, destination: string): Promise<void> {
  const before = await stat(source);
  await mkdir(dirname(destination), { recursive: true });
  await copyFile(source, destination);
  const [after, copied] = await Promise.all([stat(source), stat(destination)]);
  if (before.size !== after.size || before.mtimeMs !== after.mtimeMs || copied.size !== after.size) {
    throw new UnrealBlueprintError(ERROR_CODES.OFFLINE_STAGING_FAILED,
      `Asset changed while its offline snapshot was being copied: ${source}`);
  }
}

async function processIsAlive(pid: number): Promise<boolean> {
  try {
    process.kill(pid, 0);
    return true;
  } catch (error) {
    return error instanceof Error && "code" in error && error.code === "EPERM";
  }
}

async function staleLock(lockPath: string): Promise<boolean> {
  let modifiedAt: number;
  try {
    modifiedAt = (await stat(lockPath)).mtimeMs;
  } catch (error) {
    if (error instanceof Error && "code" in error && error.code === "ENOENT") return true;
    throw new UnrealBlueprintError(ERROR_CODES.OFFLINE_STAGING_FAILED,
      `Cannot inspect offline staging cache lock: ${lockPath}`, { cause: error });
  }
  try {
    const raw = await readFile(join(lockPath, "owner.json"), "utf8");
    const owner = JSON.parse(raw) as { pid?: unknown };
    if (typeof owner.pid === "number" && await processIsAlive(owner.pid)) return false;
    return Date.now() - modifiedAt >= STALE_LOCK_MS || typeof owner.pid === "number";
  } catch {
    return Date.now() - modifiedAt >= STALE_LOCK_MS;
  }
}

async function acquireCacheLock(root: string): Promise<() => Promise<void>> {
  const lockPath = join(root, ".lock");
  const deadline = Date.now() + LOCK_TIMEOUT_MS;
  while (true) {
    let acquired = false;
    try {
      await mkdir(lockPath);
      acquired = true;
    } catch (error) {
      if (!(error instanceof Error && "code" in error && error.code === "EEXIST")) {
        throw new UnrealBlueprintError(ERROR_CODES.OFFLINE_STAGING_FAILED, `Cannot lock offline staging cache: ${root}`, { cause: error });
      }
      if (await staleLock(lockPath)) {
        await rm(lockPath, { recursive: true, force: true });
        continue;
      }
      if (Date.now() >= deadline) {
        throw new UnrealBlueprintError(ERROR_CODES.OFFLINE_STAGING_FAILED,
          `Timed out waiting for offline staging cache lock: ${root}`);
      }
      await delay(LOCK_RETRY_MS);
    }
    if (!acquired) continue;
    try {
      await writeFile(join(lockPath, "owner.json"), JSON.stringify({ pid: process.pid, acquiredAt: new Date().toISOString() }), "utf8");
      return async () => {
        try {
          await rm(lockPath, { recursive: true, force: true });
        } catch (error) {
          throw new UnrealBlueprintError(ERROR_CODES.OFFLINE_STAGING_FAILED,
            `Cannot release offline staging cache lock: ${root}`, { cause: error });
        }
      };
    } catch (error) {
      await rm(lockPath, { recursive: true, force: true });
      throw new UnrealBlueprintError(ERROR_CODES.OFFLINE_STAGING_FAILED,
        `Cannot initialize offline staging cache lock: ${root}`, { cause: error });
    }
  }
}

async function cacheEntries(root: string): Promise<CacheEntry[]> {
  const entries: CacheEntry[] = [];
  for (const run of await readdir(root, { withFileTypes: true })) {
    if (!run.isDirectory() || !run.name.startsWith("run-")) continue;
    const path = join(root, run.name);
    const assets = (await readdir(path, { withFileTypes: true }))
      .filter((entry) => entry.isDirectory() && entry.name.startsWith("asset-"));
    const info = await stat(path);
    entries.push({ path, assetCount: assets.length, modifiedAt: info.mtimeMs });
  }
  return entries.sort((left, right) => left.modifiedAt - right.modifiedAt);
}

async function makeCapacity(root: string, incomingAssetCount: number, maximum: number): Promise<{
  readonly retainedAssetCount: number;
  readonly evictedAssetCount: number;
}> {
  const entries = await cacheEntries(root);
  let retainedAssetCount = entries.reduce((total, entry) => total + entry.assetCount, 0);
  let evictedAssetCount = 0;
  for (const entry of entries) {
    if (retainedAssetCount + incomingAssetCount <= maximum) break;
    await rm(entry.path, { recursive: true, force: true });
    retainedAssetCount -= entry.assetCount;
    evictedAssetCount += entry.assetCount;
  }
  return { retainedAssetCount, evictedAssetCount };
}

export async function withStagedAssetCopies<T>(assetPaths: readonly string[], contentRoot: string | undefined,
  options: OfflineStagingOptions, run: (assets: readonly StagedAssetCopy[]) => Promise<T>,
  cacheRoot = stagingRoot()): Promise<{
    readonly result: T;
    readonly staging: OfflineStagingSummary;
  }> {
  if (!Number.isSafeInteger(options.maxCachedAssets) || options.maxCachedAssets < 1
      || options.maxCachedAssets > HARD_OFFLINE_STAGING_MAX_CACHED_ASSETS) {
    throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT,
      `offlineStaging.maxCachedAssets must be an integer from 1 to ${HARD_OFFLINE_STAGING_MAX_CACHED_ASSETS}`);
  }
  const uniqueAssets = [...new Set(assetPaths.map((value) => resolve(value)))];
  if (!options.enabled) {
    const direct = uniqueAssets.map((sourceFilePath) => ({
      sourceFilePath,
      stagedFilePath: sourceFilePath,
      ...(contentRoot === undefined ? {} : { sourceContentRoot: resolve(contentRoot), stagedContentRoot: resolve(contentRoot) })
    }));
    return {
      result: await run(direct),
      staging: {
        used: false,
        sourceAssetCount: uniqueAssets.length,
        copiedFileCount: 0,
        companionFileCount: 0,
        maxCachedAssets: options.maxCachedAssets,
        cachedAssetCount: 0,
        evictedAssetCount: 0,
        retention: "not-required",
        scope: "requested-packages-and-companions"
      }
    };
  }
  const packageFiles = await Promise.all(uniqueAssets.map(existingPackageFiles));
  const copiedFileCount = packageFiles.reduce((total, files) => total + files.length, 0);
  const companionFileCount = copiedFileCount - uniqueAssets.length;
  const root = resolve(cacheRoot);
  await mkdir(root, { recursive: true });
  const releaseLock = await acquireCacheLock(root);
  let stageRoot: string | undefined;
  try {
    const capacity = await makeCapacity(root, uniqueAssets.length, options.maxCachedAssets);
    stageRoot = await mkdtemp(join(root, "run-"));
    const stagedAssets: StagedAssetCopy[] = [];
    for (let index = 0; index < uniqueAssets.length; index += 1) {
      const sourceFilePath = uniqueAssets[index] as string;
      const sourceRoot = contentRoot === undefined ? inferContentRoot(sourceFilePath) : resolve(contentRoot);
      const relativePath = sourceRoot === undefined ? basename(sourceFilePath) : relativeInside(sourceRoot, sourceFilePath);
      const stagedContentRoot = join(stageRoot, `asset-${index}`, "Content");
      const stagedFilePath = join(stagedContentRoot, relativePath);
      for (const source of packageFiles[index] as string[]) {
        const destination = source === sourceFilePath ? stagedFilePath : join(dirname(stagedFilePath), basename(source));
        await copyStableFile(source, destination);
      }
      stagedAssets.push({
        sourceFilePath,
        stagedFilePath,
        ...(sourceRoot === undefined ? {} : { sourceContentRoot: sourceRoot }),
        stagedContentRoot
      });
    }
    const result = await run(stagedAssets);
    const currentEvictionCount = Math.max(0, uniqueAssets.length - options.maxCachedAssets);
    for (let index = 0; index < currentEvictionCount; index += 1) {
      await rm(join(stageRoot, `asset-${index}`), { recursive: true, force: true });
    }
    return {
      result,
      staging: {
        used: true,
        sourceAssetCount: uniqueAssets.length,
        copiedFileCount,
        companionFileCount,
        maxCachedAssets: options.maxCachedAssets,
        cachedAssetCount: capacity.retainedAssetCount + uniqueAssets.length - currentEvictionCount,
        evictedAssetCount: capacity.evictedAssetCount + currentEvictionCount,
        retention: "rolling-cache",
        scope: "requested-packages-and-companions"
      }
    };
  } catch (error) {
    try {
      if (stageRoot !== undefined) await rm(stageRoot, { recursive: true, force: true });
    } catch (cleanupError) {
      throw new UnrealBlueprintError(ERROR_CODES.OFFLINE_STAGING_FAILED,
        `Offline staging failed and its incomplete snapshot could not be removed: ${stageRoot}`, {
          context: { details: { originalError: error instanceof Error ? error.message : String(error) } },
          cause: cleanupError
        });
    }
    if (error instanceof UnrealBlueprintError) throw error;
    throw new UnrealBlueprintError(ERROR_CODES.OFFLINE_STAGING_FAILED, "Offline staging cache operation failed", { cause: error });
  } finally {
    await releaseLock();
  }
}
