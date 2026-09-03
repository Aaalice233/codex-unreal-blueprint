import { execFile } from "node:child_process";
import { readdir, readFile } from "node:fs/promises";
import { dirname, extname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { promisify } from "node:util";
import { ERROR_CODES, UnrealBlueprintError } from "../shared/errors.js";
import { assertJsonValue, isJsonObject, type JsonObject, type JsonValue } from "../shared/json.js";
import {
  DEFAULT_OFFLINE_STAGING_MAX_CACHED_ASSETS,
  type OfflineStagingOptions,
  type OfflineStagingSummary,
  type StagedAssetCopy,
  withStagedAssetCopies
} from "./asset-staging.js";

const execFileAsync = promisify(execFile);
const MAX_INSPECTOR_OUTPUT = 128 * 1024 * 1024;

function pluginRoot(): string {
  return resolve(dirname(fileURLToPath(import.meta.url)), "../..");
}

function inspectorScript(): string {
  return resolve(pluginRoot(), "offline/Inspect-UAsset.ps1");
}

function stringArray(value: JsonValue | undefined): string[] {
  return Array.isArray(value) ? value.filter((item): item is string => typeof item === "string") : [];
}

function offlineSpecializations(result: JsonValue): string[] {
  if (!isJsonObject(result)) return [];
  const exports = Array.isArray(result.Exports) ? result.Exports : [];
  const classes = new Set(exports.flatMap((item) =>
    isJsonObject(item) && typeof item.ClassName === "string" ? [item.ClassName] : []));
  const specializations = new Set<string>();
  if ([...classes].some((name) => name === "Material" || name.startsWith("MaterialInstance"))) specializations.add("material");
  if (classes.has("NiagaraSystem") || (Array.isArray(result.NiagaraEmitters) && result.NiagaraEmitters.length > 0)) {
    specializations.add("niagaraSystem");
  }
  if (classes.has("AnimMontage")) specializations.add("animMontage");
  if (classes.has("WidgetBlueprintGeneratedClass")) {
    specializations.add("blueprint");
    specializations.add("umg");
  } else if (classes.has("AnimBlueprintGeneratedClass")) {
    specializations.add("blueprint");
    specializations.add("animBlueprint");
  } else if (classes.has("BlueprintGeneratedClass")) {
    specializations.add("blueprint");
  }
  return [...specializations].sort();
}

async function runPowerShell(arguments_: string[]): Promise<string> {
  try {
    const environment = { ...process.env };
    if (process.platform === "win32" && environment.PATHEXT === undefined) {
      // MCP hosts intentionally pass a reduced environment. PowerShell needs PATHEXT from process startup
      // to invoke dotnet even when the executable path is absolute.
      environment.PATHEXT = ".COM;.EXE;.BAT;.CMD";
    }
    const result = await execFileAsync("pwsh", ["-NoProfile", "-File", inspectorScript(), ...arguments_], {
      windowsHide: true,
      maxBuffer: MAX_INSPECTOR_OUTPUT,
      encoding: "utf8",
      env: environment
    });
    return result.stdout.replace(/^\uFEFF/, "").trim();
  } catch (error) {
    const detail = error instanceof Error ? error.message : String(error);
    throw new UnrealBlueprintError(ERROR_CODES.RPC_ERROR, `Offline UAsset inspection failed: ${detail}`);
  }
}

function replacePathPrefix(value: string, staged: string, source: string): string {
  return value.toLowerCase().startsWith(staged.toLowerCase()) ? source + value.slice(staged.length) : value;
}

function restoreSourcePaths(value: JsonValue, asset: StagedAssetCopy): JsonValue {
  if (typeof value === "string") {
    let restored = replacePathPrefix(value, asset.stagedFilePath, asset.sourceFilePath);
    if (asset.stagedContentRoot !== undefined && asset.sourceContentRoot !== undefined) {
      restored = replacePathPrefix(restored, asset.stagedContentRoot, asset.sourceContentRoot);
    }
    return restored;
  }
  if (Array.isArray(value)) return value.map((item) => restoreSourcePaths(item, asset));
  if (!isJsonObject(value)) return value;
  return Object.fromEntries(Object.entries(value).map(([key, item]) => [key, restoreSourcePaths(item, asset)]));
}

async function parseOfflineAsset(asset: StagedAssetCopy, searchTerms: string[]): Promise<JsonValue> {
  const arguments_ = ["-Path", asset.stagedFilePath, "-Format", "json", "-Search", ...(searchTerms.length > 0 ? searchTerms : [""])];
  if (asset.stagedContentRoot !== undefined) arguments_.push("-ContentRoot", asset.stagedContentRoot);
  const output = await runPowerShell(arguments_);
  try {
    const parsed: unknown = JSON.parse(output);
    assertJsonValue(parsed);
    return restoreSourcePaths(parsed, asset);
  } catch (error) {
    throw new UnrealBlueprintError(ERROR_CODES.INVALID_RESPONSE, "Offline inspector returned invalid JSON", {
      context: { details: { output: output.slice(0, 4_096), cause: error instanceof Error ? error.message : String(error) } }
    });
  }
}

function offlineEnvelope(parsed: JsonValue, sourceFilePath: string, staging?: OfflineStagingSummary): JsonValue {
  const specializations = offlineSpecializations(parsed);
  const support = {
    generic: true,
    specialized: specializations.length > 0,
    editable: false,
    specializations,
    highestLayer: specializations.length > 0 ? "specialized" : "generic"
  };
  return {
    mode: "offline",
    evidence: "serialized-package",
    editable: false,
    sourceFilePath,
    facets: { support },
    ...(staging === undefined ? {} : { staging }),
    result: parsed
  };
}

export async function inspectOfflineAsset(filePath: string, contentRoot?: string, searchTerms: string[] = [],
  staging: OfflineStagingOptions = { enabled: false, maxCachedAssets: DEFAULT_OFFLINE_STAGING_MAX_CACHED_ASSETS }): Promise<JsonValue> {
  const staged = await withStagedAssetCopies([filePath], contentRoot, staging, async (assets) => {
    const asset = assets[0];
    if (asset === undefined) throw new UnrealBlueprintError(ERROR_CODES.INTERNAL, "Offline staging returned no asset");
    return parseOfflineAsset(asset, searchTerms);
  });
  return offlineEnvelope(staged.result, resolve(filePath), staged.staging);
}

function jsonEqual(left: JsonValue | undefined, right: JsonValue | undefined): boolean {
  return JSON.stringify(left) === JSON.stringify(right);
}

function changedTopLevelFields(left: JsonValue, right: JsonValue): string[] {
  if (!isJsonObject(left) || !isJsonObject(right)) return jsonEqual(left, right) ? [] : ["result"];
  const keys = [...new Set([...Object.keys(left), ...Object.keys(right)])].sort();
  return keys.filter((key) => !jsonEqual(left[key], right[key]));
}

export async function compareOfflineAssets(baseFilePath: string, targetFilePath: string, contentRoot?: string,
  searchTerms: string[] = [],
  staging: OfflineStagingOptions = { enabled: false, maxCachedAssets: DEFAULT_OFFLINE_STAGING_MAX_CACHED_ASSETS }): Promise<JsonValue> {
  // The first invocation may populate the shared content-addressed build cache.
  // Keep the pair sequential so a cold cache cannot start two dotnet builds into the same directory.
  const staged = await withStagedAssetCopies([baseFilePath, targetFilePath], contentRoot, staging, async (assets) => {
    const bySource = new Map(assets.map((asset) => [asset.sourceFilePath.toLowerCase(), asset]));
    const baseAsset = bySource.get(resolve(baseFilePath).toLowerCase());
    const targetAsset = bySource.get(resolve(targetFilePath).toLowerCase());
    if (baseAsset === undefined || targetAsset === undefined) {
      throw new UnrealBlueprintError(ERROR_CODES.INTERNAL, "Offline staging did not return both comparison assets");
    }
    const baseParsed = await parseOfflineAsset(baseAsset, searchTerms);
    const targetParsed = await parseOfflineAsset(targetAsset, searchTerms);
    return {
      base: offlineEnvelope(baseParsed, resolve(baseFilePath)),
      target: offlineEnvelope(targetParsed, resolve(targetFilePath))
    };
  });
  const { base, target } = staged.result;
  const baseResult = isJsonObject(base) ? base.result : null;
  const targetResult = isJsonObject(target) ? target.result : null;
  return {
    mode: "offline",
    evidence: "serialized-package",
    editable: false,
    staging: staged.staging,
    identical: jsonEqual(baseResult, targetResult),
    changedFields: changedTopLevelFields(baseResult ?? null, targetResult ?? null),
    base,
    target
  };
}

function packagePathFromTarget(target: string, contentRoot?: string): string {
  if (target.startsWith("/Game/")) return target.split(/[.:]/, 1)[0] as string;
  if (contentRoot === undefined) {
    const normalized = resolve(target).replaceAll("\\", "/");
    const marker = "/Content/";
    const index = normalized.toLowerCase().indexOf(marker.toLowerCase());
    if (index < 0) throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, "contentRoot is required when target is outside a Content directory");
    contentRoot = normalized.slice(0, index + "/Content".length);
  }
  const normalizedRoot = resolve(contentRoot).replaceAll("\\", "/").replace(/\/$/, "");
  const normalizedTarget = resolve(target).replaceAll("\\", "/");
  if (!normalizedTarget.toLowerCase().startsWith(`${normalizedRoot.toLowerCase()}/`)) {
    throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, "target must be inside contentRoot");
  }
  return `/Game/${normalizedTarget.slice(normalizedRoot.length + 1).replace(/\.(uasset|umap)$/i, "")}`;
}

async function assetFiles(root: string): Promise<string[]> {
  const result: string[] = [];
  const queue = [resolve(root)];
  while (queue.length > 0) {
    const directory = queue.pop() as string;
    for (const entry of await readdir(directory, { withFileTypes: true })) {
      const path = join(directory, entry.name);
      if (entry.isDirectory()) queue.push(path);
      else if ([".uasset", ".umap", ".uexp"].includes(extname(entry.name).toLowerCase())) result.push(path);
    }
  }
  return result.sort();
}

export async function findOfflineReferencers(target: string, searchRoot: string, contentRoot?: string,
  offset = 0, limit = 500): Promise<JsonValue> {
  const packagePath = packagePathFromTarget(target, contentRoot);
  const utf8 = Buffer.from(packagePath, "utf8");
  const utf16 = Buffer.from(packagePath, "utf16le");
  const matches: JsonValue[] = [];
  for (const file of await assetFiles(searchRoot)) {
    const bytes = await readFile(file);
    const utf8Index = bytes.indexOf(utf8);
    const utf16Index = bytes.indexOf(utf16);
    if (utf8Index >= 0 || utf16Index >= 0) {
      matches.push({
        file,
        encoding: utf8Index >= 0 ? "UTF8/ASCII" : "UTF16-LE",
        packagePath
      });
    }
  }
  const end = Math.min(matches.length, offset + limit);
  return {
    mode: "offline",
    evidence: "binary-reference-match",
    editable: false,
    packagePath,
    searchRoot: resolve(searchRoot),
    referencers: {
      total: matches.length,
      items: matches.slice(offset, end),
      ...(end < matches.length ? { nextCursor: String(end) } : {})
    }
  };
}

export function offlineArguments(parameters: JsonObject): {
  contentRoot?: string;
  searchTerms: string[];
  staging: OfflineStagingOptions;
} {
  const requested = isJsonObject(parameters.offlineStaging) ? parameters.offlineStaging : {};
  return {
    ...(typeof parameters.contentRoot === "string" ? { contentRoot: parameters.contentRoot } : {}),
    searchTerms: stringArray(parameters.searchTerms),
    staging: {
      enabled: requested.enabled === true,
      maxCachedAssets: typeof requested.maxCachedAssets === "number"
        ? requested.maxCachedAssets
        : DEFAULT_OFFLINE_STAGING_MAX_CACHED_ASSETS
    }
  };
}
