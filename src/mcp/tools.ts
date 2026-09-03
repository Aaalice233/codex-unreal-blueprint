import { z } from "zod";
import { discoverSessions, invokeWriteWithRecovery, selectSession, withUnrealClient, type EditorSession, type SessionQuery } from "../client/index.js";
import { TOOL_NAMES, type ToolName } from "../shared/contracts.js";
import { asUnrealBlueprintError, ERROR_CODES, UnrealBlueprintError } from "../shared/errors.js";
import { assertJsonValue, isJsonObject, type JsonObject, type JsonValue } from "../shared/json.js";
import { compareOfflineAssets, findOfflineReferencers, inspectOfflineAsset, offlineArguments } from "../offline/uasset-inspector.js";

const sessionSchema = z.object({
  editorSessionId: z.string().min(1).optional(),
  uproject: z.string().min(1).optional()
}).strict().optional();
const jsonObjectSchema = z.record(z.unknown());
const operationSchema = z.object({ operation: z.string().min(1) }).passthrough();
const assetModeSchema = z.enum(["auto", "editor", "offline"]).optional();
const assetFacetsSchema = z.array(z.enum(["support", "generic", "properties", "dependencies", "referencers", "specialized"])).max(16).optional();
const propertyPathsSchema = z.array(z.string().min(1)).max(500).optional();
const offlineStagingSchema = z.object({
  enabled: z.boolean().optional(),
  maxCachedAssets: z.number().int().min(1).max(512).optional()
}).strict().optional();

export const toolSchemas = {
  unreal_status: z.object({ session: sessionSchema }).strict(),
  unreal_doctor: z.object({ session: sessionSchema }).strict(),
  unreal_search: z.object({
    session: sessionSchema,
    query: z.string().min(1),
    domain: z.enum(["asset", "class", "member", "property", "action", "operation"]).optional(),
    context: jsonObjectSchema.optional(),
    cursor: z.string().optional(),
    limit: z.number().int().min(1).max(200).optional()
  }).strict(),
  unreal_asset_inspect: z.object({
    session: sessionSchema,
    mode: assetModeSchema,
    assetPath: z.string().min(1).optional(),
    filePath: z.string().min(1).optional(),
    contentRoot: z.string().min(1).optional(),
    searchTerms: z.array(z.string().min(1)).max(100).optional(),
    offlineStaging: offlineStagingSchema,
    facets: assetFacetsSchema,
    propertyPaths: propertyPathsSchema,
    cursor: z.string().optional(),
    limit: z.number().int().min(1).max(500).optional()
  }).strict().refine((value) => value.assetPath !== undefined || value.filePath !== undefined,
    "unreal_asset_inspect requires assetPath or filePath"),
  unreal_asset_compare: z.object({
    session: sessionSchema,
    mode: assetModeSchema,
    baseAssetPath: z.string().min(1).optional(),
    targetAssetPath: z.string().min(1).optional(),
    baseFilePath: z.string().min(1).optional(),
    targetFilePath: z.string().min(1).optional(),
    contentRoot: z.string().min(1).optional(),
    searchTerms: z.array(z.string().min(1)).max(100).optional(),
    offlineStaging: offlineStagingSchema,
    facets: assetFacetsSchema,
    propertyPaths: propertyPathsSchema,
    cursor: z.string().optional(),
    limit: z.number().int().min(1).max(500).optional()
  }).strict().refine((value) =>
    (value.baseAssetPath !== undefined && value.targetAssetPath !== undefined)
      || (value.baseFilePath !== undefined && value.targetFilePath !== undefined),
  "unreal_asset_compare requires a complete Editor asset-path pair or offline file-path pair"),
  unreal_asset_referencers: z.object({
    session: sessionSchema,
    mode: assetModeSchema,
    assetPath: z.string().min(1).optional(),
    targetFilePath: z.string().min(1).optional(),
    searchRoot: z.string().min(1).optional(),
    contentRoot: z.string().min(1).optional(),
    recursive: z.boolean().optional(),
    cursor: z.string().optional(),
    limit: z.number().int().min(1).max(500).optional()
  }).strict().refine((value) => value.assetPath !== undefined || value.targetFilePath !== undefined,
    "unreal_asset_referencers requires assetPath or targetFilePath"),
  blueprint_capabilities: z.object({
    session: sessionSchema,
    domain: z.string().min(1).optional(),
    operationNames: z.array(z.string().min(1)).max(200).optional()
  }).strict(),
  blueprint_inspect: z.object({
    session: sessionSchema,
    assetPath: z.string().min(1),
    facets: z.array(z.string().min(1)).max(32).optional(),
    classDefaultPropertyPaths: z.array(z.string().min(1)).max(500).optional(),
    cursor: z.string().optional(),
    limit: z.number().int().min(1).max(500).optional()
  }).strict(),
  blueprint_validate: z.object({
    session: sessionSchema,
    operations: z.array(operationSchema).min(1).max(500),
    expectedStructureHashes: jsonObjectSchema.optional()
  }).strict(),
  blueprint_apply: z.object({
    session: sessionSchema,
    requestId: z.string().min(1).max(128),
    operations: z.array(operationSchema).min(1).max(500),
    expectedStructureHashes: jsonObjectSchema.optional()
  }).strict(),
  blueprint_job: z.union([
    z.object({ session: sessionSchema, action: z.literal("query"), jobId: z.string().min(1).optional(), requestId: z.string().min(1).max(128).optional() }).strict()
      .refine((value) => value.jobId !== undefined || value.requestId !== undefined, "query requires jobId or requestId"),
    z.object({ session: sessionSchema, action: z.literal("wait"), jobId: z.string().min(1).optional(), requestId: z.string().min(1).max(128).optional(), timeoutMs: z.number().int().min(0).max(600_000).optional() }).strict()
      .refine((value) => value.jobId !== undefined || value.requestId !== undefined, "wait requires jobId or requestId"),
    z.object({ session: sessionSchema, action: z.literal("cancel"), jobId: z.string().min(1) }).strict()
  ]),
  blueprint_verify: z.object({
    session: sessionSchema,
    assetPaths: z.array(z.string().min(1)).min(1).max(200),
    expectations: z.array(jsonObjectSchema).max(500).optional(),
    compile: z.boolean().optional(),
    reload: z.boolean().optional()
  }).strict()
} satisfies Record<ToolName, z.ZodTypeAny>;

export const toolDescriptions: Record<ToolName, string> = {
  unreal_status: "Discover or select the exact UE4.27 Editor and return session, PIE, source-control, dirty-package, and queue status.",
  unreal_doctor: "Check the UE plugin, protocol, project configuration, port, permissions, and build environment.",
  unreal_search: "Search Blueprint assets, classes, members, properties, graph actions, or operations with pagination.",
  unreal_asset_inspect: "Inspect any Unreal asset through layered capabilities; offline-only assets can be parsed from a rolling temporary cache without restarting the Editor.",
  unreal_asset_compare: "Compare two Unreal assets through the Editor or a rolling temporary offline cache and return structured changes.",
  unreal_asset_referencers: "Find precise Asset Registry referencers in Editor mode or serialized binary reference evidence in offline mode.",
  blueprint_capabilities: "Fetch strict operation parameter JSON Schemas and examples dynamically from the UE Operation Registry.",
  blueprint_inspect: "Read paged Blueprint facets, stable IDs, compile state, and structure hashes without modifying assets.",
  blueprint_validate: "Validate a one-shot operation list in memory without changing assets.",
  blueprint_apply: "Start an automatic transactional Blueprint write job using one unique requestId; no plugin confirmation dialog.",
  blueprint_job: "Query, wait for, or safely cancel a job; requestId resolves an uncertain write without replaying it.",
  blueprint_verify: "Independently compile, reload, and assert Blueprint disk structure."
};

export const toolAnnotations = Object.fromEntries(TOOL_NAMES.map((name) => [name, {
  readOnlyHint: !["blueprint_apply", "blueprint_job"].includes(name),
  destructiveHint: name === "blueprint_apply",
  openWorldHint: false
}])) as Record<ToolName, { readOnlyHint: boolean; destructiveHint: boolean; openWorldHint: boolean }>;

function splitParameters(value: unknown): { session: SessionQuery; rpcParams: JsonObject } {
  assertJsonValue(value);
  if (!isJsonObject(value)) throw new TypeError("MCP tool arguments must be an object");
  const rawSession = value.session;
  const sessionObject = isJsonObject(rawSession) ? rawSession : {};
  const session: SessionQuery = {
    ...(typeof sessionObject.editorSessionId === "string" ? { editorSessionId: sessionObject.editorSessionId } : {}),
    ...(typeof sessionObject.uproject === "string" ? { uproject: sessionObject.uproject } : process.env.CODEX_UNREAL_UPROJECT === undefined ? {} : { uproject: process.env.CODEX_UNREAL_UPROJECT })
  };
  const { session: _session, ...rpcParams } = value;
  return { session, rpcParams };
}

function invokeOptions(name: ToolName, rpcParams: JsonObject, signal?: AbortSignal): { signal?: AbortSignal; timeoutMs?: number } {
  const options: { signal?: AbortSignal; timeoutMs?: number } = signal === undefined ? {} : { signal };
  if (name === "blueprint_job" && rpcParams.action === "wait" && typeof rpcParams.timeoutMs === "number") {
    options.timeoutMs = Math.max(1, rpcParams.timeoutMs + 1_000);
  }
  return options;
}

const ASSET_TOOL_NAMES = new Set<ToolName>([
  "unreal_asset_inspect", "unreal_asset_compare", "unreal_asset_referencers"
]);

function pickEditorAssetParameters(name: ToolName, parameters: JsonObject): JsonObject {
  const allowed = name === "unreal_asset_inspect"
    ? ["assetPath", "facets", "propertyPaths", "cursor", "limit"]
    : name === "unreal_asset_compare"
      ? ["baseAssetPath", "targetAssetPath", "facets", "propertyPaths", "cursor", "limit"]
      : ["assetPath", "recursive", "cursor", "limit"];
  return Object.fromEntries(Object.entries(parameters).filter(([key]) => allowed.includes(key))) as JsonObject;
}

function canRunEditorAssetTool(name: ToolName, parameters: JsonObject): boolean {
  if (name === "unreal_asset_inspect") return typeof parameters.assetPath === "string";
  if (name === "unreal_asset_compare") return typeof parameters.baseAssetPath === "string" && typeof parameters.targetAssetPath === "string";
  return typeof parameters.assetPath === "string";
}

function canRunOfflineAssetTool(name: ToolName, parameters: JsonObject): boolean {
  if (name === "unreal_asset_inspect") return typeof parameters.filePath === "string";
  if (name === "unreal_asset_compare") return typeof parameters.baseFilePath === "string" && typeof parameters.targetFilePath === "string";
  return typeof parameters.targetFilePath === "string" && typeof parameters.searchRoot === "string";
}

async function invokeOfflineAssetTool(name: ToolName, parameters: JsonObject): Promise<JsonValue> {
  const { contentRoot, searchTerms, staging } = offlineArguments(parameters);
  if (name === "unreal_asset_inspect" && typeof parameters.filePath === "string") {
    return inspectOfflineAsset(parameters.filePath, contentRoot, searchTerms, staging);
  }
  if (name === "unreal_asset_compare" && typeof parameters.baseFilePath === "string" && typeof parameters.targetFilePath === "string") {
    return compareOfflineAssets(parameters.baseFilePath, parameters.targetFilePath, contentRoot, searchTerms, staging);
  }
  if (name === "unreal_asset_referencers" && typeof parameters.targetFilePath === "string" && typeof parameters.searchRoot === "string") {
    const { offset, limit } = offlinePagination(parameters);
    return findOfflineReferencers(parameters.targetFilePath, parameters.searchRoot, contentRoot, offset, limit);
  }
  throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, `${name} does not have the file paths required for offline mode`);
}

function offlinePagination(parameters: JsonObject): { offset: number; limit: number } {
  const cursor = typeof parameters.cursor === "string" ? parameters.cursor : "0";
  const offset = Number(cursor);
  if (!/^\d+$/.test(cursor) || !Number.isSafeInteger(offset)) {
    throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, "cursor must be a non-negative decimal offset");
  }
  return { offset, limit: typeof parameters.limit === "number" ? parameters.limit : 500 };
}

async function invokeLayeredAssetTool(name: ToolName, parameters: JsonObject, session: SessionQuery,
  signal?: AbortSignal): Promise<JsonValue> {
  const mode = typeof parameters.mode === "string" ? parameters.mode : "auto";
  if (mode === "offline") return invokeOfflineAssetTool(name, parameters);
  if (mode === "editor") {
    if (!canRunEditorAssetTool(name, parameters)) {
      throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, `${name} does not have the Unreal asset paths required for Editor mode`);
    }
    const rpcParams = pickEditorAssetParameters(name, parameters);
    return withUnrealClient({ session, ...(signal === undefined ? {} : { signal }) },
      (client) => client.invoke(name, rpcParams, invokeOptions(name, rpcParams, signal)));
  }

  if (canRunEditorAssetTool(name, parameters)) {
    const sessions = await discoverSessions(session, {}, signal);
    if (sessions.length === 1) {
      const rpcParams = pickEditorAssetParameters(name, parameters);
      return withUnrealClient({ session: { editorSessionId: sessions[0]!.editorSessionId }, ...(signal === undefined ? {} : { signal }) },
        (client) => client.invoke(name, rpcParams, invokeOptions(name, rpcParams, signal)));
    }
    if (sessions.length > 1 || session.editorSessionId !== undefined) {
      await selectSession(session, {}, signal);
    }
  }
  if (canRunOfflineAssetTool(name, parameters)) return invokeOfflineAssetTool(name, parameters);
  throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT,
    `${name} auto mode found no matching Editor and does not have the file paths required for offline fallback`);
}

export async function invokeTool(name: ToolName, parameters: unknown, signal?: AbortSignal): Promise<JsonValue> {
  const parsed = toolSchemas[name].parse(parameters);
  const { session, rpcParams } = splitParameters(parsed);
  if (ASSET_TOOL_NAMES.has(name)) return invokeLayeredAssetTool(name, rpcParams, session, signal);
  if (name === "unreal_status" && session.editorSessionId === undefined) {
    const sessions = await discoverSessions(session, {}, signal);
    if (sessions.length !== 1) {
      return {
        connected: false,
        ambiguous: sessions.length > 1,
        sessions: sessions.map((candidate) => ({
          editorSessionId: candidate.editorSessionId,
          pid: candidate.pid,
          uproject: candidate.uproject,
          engineVersion: candidate.engineVersion,
          pluginVersion: candidate.pluginVersion,
          protocolVersion: candidate.protocolVersion
        }))
      };
    }
    const selected = sessions[0] as EditorSession;
    const status = await withUnrealClient(
      { session: { editorSessionId: selected.editorSessionId }, ...(signal === undefined ? {} : { signal }) },
      (client) => client.invoke(name, rpcParams, invokeOptions(name, rpcParams, signal))
    );
    if (!isJsonObject(status)) {
      throw new UnrealBlueprintError(ERROR_CODES.INVALID_RESPONSE, "Editor returned a non-object status response");
    }
    return {
      ...status,
      connected: true,
      session: {
        editorSessionId: selected.editorSessionId,
        pid: selected.pid,
        uproject: selected.uproject,
        engineVersion: selected.engineVersion,
        pluginVersion: selected.pluginVersion,
        protocolVersion: selected.protocolVersion
      }
    };
  }
  if (name === "blueprint_apply") {
    const selected = await selectSession(session, {}, signal);
    return invokeWriteWithRecovery(
      { session: { editorSessionId: selected.editorSessionId }, ...(signal === undefined ? {} : { signal }) },
      name,
      rpcParams,
      signal === undefined ? {} : { signal }
    );
  }
  return withUnrealClient(
    { session, ...(signal === undefined ? {} : { signal }) },
    (client) => client.invoke(name, rpcParams, invokeOptions(name, rpcParams, signal))
  );
}

export function failedToolResult(error: unknown): { isError: true; content: [{ type: "text"; text: string }]; structuredContent: { error: ReturnType<ReturnType<typeof asUnrealBlueprintError>["toJSON"]> } } {
  const normalized = asUnrealBlueprintError(error).toJSON();
  return {
    isError: true,
    content: [{ type: "text", text: JSON.stringify({ error: normalized }, null, 2) }],
    structuredContent: { error: normalized }
  };
}
