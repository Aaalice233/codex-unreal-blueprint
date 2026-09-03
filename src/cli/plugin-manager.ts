import { createHash } from "node:crypto";
import { access, copyFile, mkdir, readFile, readdir, rm, stat, writeFile } from "node:fs/promises";
import { dirname, join, relative, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";
import { ERROR_CODES, UnrealBlueprintError } from "../shared/errors.js";
import { isJsonObject, type JsonObject } from "../shared/json.js";

const MANIFEST_NAME = ".pi-unreal-blueprint.manifest.json";

interface ManagedFile {
  readonly path: string;
  readonly sha256: string;
}

interface ManagedManifest {
  readonly version: 1;
  readonly source: string;
  readonly generatedAtUtc: string;
  readonly files: ManagedFile[];
}

export interface PluginCommandResult extends JsonObject {
  readonly action: "install" | "update" | "remove";
  readonly target: string;
  readonly changedFiles: number;
  readonly removedFiles: number;
}

function packageRoot(): string {
  let current = dirname(fileURLToPath(import.meta.url));
  for (let depth = 0; depth < 8; depth += 1) {
    if (current.endsWith(`${sep}src${sep}cli`) || current.endsWith(`${sep}src${sep}cli${sep}dist${sep}src${sep}cli`)) {
      const candidate = current.includes(`${sep}dist${sep}`)
        ? resolve(current, "../../../../..")
        : resolve(current, "../..");
      return candidate;
    }
    current = dirname(current);
  }
  throw new UnrealBlueprintError(ERROR_CODES.SETUP_FAILED, "Cannot locate the pi-unreal-blueprint package root");
}

async function exists(path: string): Promise<boolean> {
  try {
    await access(path);
    return true;
  } catch {
    return false;
  }
}

function requireString(input: JsonObject, key: string): string | undefined {
  const value = input[key];
  if (value === undefined) return undefined;
  if (typeof value !== "string" || value.trim().length === 0) {
    throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, `${key} must be a non-empty string`);
  }
  return value;
}

export function resolvePluginTarget(input: JsonObject): string {
  const explicit = requireString(input, "pluginTarget");
  if (explicit !== undefined) return resolve(explicit);
  const scope = input.scope ?? "project";
  if (scope !== "project" && scope !== "engine") {
    throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, "scope must be 'project' or 'engine'");
  }
  if (scope === "engine") {
    const engineRoot = requireString(input, "engineRoot");
    if (engineRoot === undefined) throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, "engineRoot is required for engine scope");
    return resolve(engineRoot, "Engine/Plugins/Developer/PiUnrealBlueprint");
  }
  const uproject = requireString(input, "uproject");
  if (uproject === undefined || !uproject.toLowerCase().endsWith(".uproject")) {
    throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, "uproject is required for project scope");
  }
  return resolve(dirname(uproject), "Plugins/PiUnrealBlueprint");
}

async function listFiles(root: string, current = root): Promise<string[]> {
  const result: string[] = [];
  for (const entry of await readdir(current, { withFileTypes: true })) {
    if (["Binaries", "Intermediate", "Saved", MANIFEST_NAME].includes(entry.name)) continue;
    const absolute = join(current, entry.name);
    if (entry.isDirectory()) result.push(...await listFiles(root, absolute));
    else if (entry.isFile()) result.push(relative(root, absolute).replaceAll(sep, "/"));
  }
  return result.sort();
}

async function hashFile(path: string): Promise<string> {
  return createHash("sha256").update(await readFile(path)).digest("hex");
}

function safeManagedPath(target: string, relativePath: string): string {
  if (relativePath.length === 0 || relativePath.startsWith("/") || relativePath.includes("..") || relativePath.includes("\\")) {
    throw new UnrealBlueprintError(ERROR_CODES.PLUGIN_MANIFEST_INVALID, `Unsafe managed path: ${relativePath}`);
  }
  const absolute = resolve(target, relativePath);
  const root = `${resolve(target)}${sep}`.toLowerCase();
  if (!absolute.toLowerCase().startsWith(root)) {
    throw new UnrealBlueprintError(ERROR_CODES.PLUGIN_MANIFEST_INVALID, `Managed path escapes plugin target: ${relativePath}`);
  }
  return absolute;
}

async function readManifest(target: string): Promise<ManagedManifest | undefined> {
  const path = join(target, MANIFEST_NAME);
  if (!await exists(path)) return undefined;
  let value: unknown;
  try {
    value = JSON.parse(await readFile(path, "utf8"));
  } catch (cause) {
    throw new UnrealBlueprintError(ERROR_CODES.PLUGIN_MANIFEST_INVALID, `Cannot parse plugin manifest: ${path}`, { cause });
  }
  if (!isJsonObject(value) || value.version !== 1 || !Array.isArray(value.files)) {
    throw new UnrealBlueprintError(ERROR_CODES.PLUGIN_MANIFEST_INVALID, `Invalid plugin manifest: ${path}`);
  }
  const files = value.files.map((item) => {
    if (!isJsonObject(item) || typeof item.path !== "string" || typeof item.sha256 !== "string") {
      throw new UnrealBlueprintError(ERROR_CODES.PLUGIN_MANIFEST_INVALID, `Invalid managed file in manifest: ${path}`);
    }
    safeManagedPath(target, item.path);
    return { path: item.path, sha256: item.sha256 };
  });
  return { version: 1, source: typeof value.source === "string" ? value.source : "", generatedAtUtc: typeof value.generatedAtUtc === "string" ? value.generatedAtUtc : "", files };
}

async function removeEmptyParents(path: string, target: string): Promise<void> {
  let current = dirname(path);
  while (current.toLowerCase().startsWith(`${resolve(target)}${sep}`.toLowerCase())) {
    try {
      if ((await readdir(current)).length > 0) return;
      await rm(current, { recursive: false });
    } catch {
      return;
    }
    current = dirname(current);
  }
}

export async function managePlugin(action: "install" | "update" | "remove", input: JsonObject): Promise<PluginCommandResult> {
  const target = resolvePluginTarget(input);
  const oldManifest = await readManifest(target);
  if (action === "install" && oldManifest !== undefined) {
    throw new UnrealBlueprintError(ERROR_CODES.PLUGIN_ALREADY_INSTALLED, `Plugin is already managed at ${target}; use plugin update`);
  }
  if ((action === "update" || action === "remove") && oldManifest === undefined) {
    throw new UnrealBlueprintError(ERROR_CODES.PLUGIN_NOT_INSTALLED, `No managed plugin installation exists at ${target}`);
  }

  let removedFiles = 0;
  if (action === "remove") {
    for (const file of oldManifest?.files ?? []) {
      const path = safeManagedPath(target, file.path);
      if (await exists(path)) {
        if (await hashFile(path) !== file.sha256) {
          throw new UnrealBlueprintError(ERROR_CODES.PLUGIN_CONFLICT, `Refusing to remove a locally modified managed plugin file: ${path}`);
        }
        await rm(path);
        removedFiles += 1;
        await removeEmptyParents(path, target);
      }
    }
    await rm(join(target, MANIFEST_NAME), { force: true });
    return { action, target, changedFiles: 0, removedFiles };
  }

  const source = join(packageRoot(), "unreal/PiUnrealBlueprint");
  if (!await exists(join(source, "PiUnrealBlueprint.uplugin"))) {
    throw new UnrealBlueprintError(ERROR_CODES.SETUP_FAILED, `Packaged UE plugin source is missing: ${source}`);
  }
  const sourceFiles = await listFiles(source);
  const sourceSet = new Set(sourceFiles);
  for (const file of oldManifest?.files ?? []) {
    if (!sourceSet.has(file.path)) {
      const path = safeManagedPath(target, file.path);
      if (await exists(path)) {
        if (await hashFile(path) !== file.sha256) {
          throw new UnrealBlueprintError(ERROR_CODES.PLUGIN_CONFLICT, `Refusing to remove a locally modified obsolete plugin file: ${path}`);
        }
        await rm(path);
        removedFiles += 1;
        await removeEmptyParents(path, target);
      }
    }
  }

  const oldByPath = new Map((oldManifest?.files ?? []).map((file) => [file.path, file]));
  const managedFiles: ManagedFile[] = [];
  let changedFiles = 0;
  for (const relativePath of sourceFiles) {
    const sourcePath = join(source, relativePath);
    const targetPath = safeManagedPath(target, relativePath);
    const sha256 = await hashFile(sourcePath);
    const old = oldByPath.get(relativePath);
    if (await exists(targetPath)) {
      const targetStat = await stat(targetPath);
      if (!targetStat.isFile()) throw new UnrealBlueprintError(ERROR_CODES.PLUGIN_CONFLICT, `Plugin target is not a file: ${targetPath}`);
      const currentHash = await hashFile(targetPath);
      if (old === undefined && currentHash !== sha256) {
        throw new UnrealBlueprintError(ERROR_CODES.PLUGIN_CONFLICT, `Refusing to overwrite unmanaged plugin file: ${targetPath}`);
      }
      if (old !== undefined && currentHash !== old.sha256 && currentHash !== sha256) {
        throw new UnrealBlueprintError(ERROR_CODES.PLUGIN_CONFLICT, `Refusing to overwrite a locally modified managed plugin file: ${targetPath}`);
      }
      if (currentHash === sha256) {
        managedFiles.push({ path: relativePath, sha256 });
        continue;
      }
    }
    await mkdir(dirname(targetPath), { recursive: true });
    await copyFile(sourcePath, targetPath);
    if (await hashFile(targetPath) !== sha256) throw new UnrealBlueprintError(ERROR_CODES.PLUGIN_COPY_FAILED, `Copied plugin file failed SHA-256 verification: ${targetPath}`);
    changedFiles += 1;
    managedFiles.push({ path: relativePath, sha256 });
  }
  const manifest: ManagedManifest = { version: 1, source: source.replaceAll(sep, "/"), generatedAtUtc: new Date().toISOString(), files: managedFiles };
  await mkdir(target, { recursive: true });
  await writeFile(join(target, MANIFEST_NAME), `${JSON.stringify(manifest, null, 2)}\n`, "utf8");
  return { action, target, changedFiles, removedFiles };
}
