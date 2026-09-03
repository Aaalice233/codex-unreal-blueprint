import { z } from "zod";
import { discoverSessions, invokeWriteWithRecovery, selectSession, withUnrealClient, type EditorSession, type SessionQuery } from "../client/index.js";
import { TOOL_NAMES, type ToolName } from "../shared/contracts.js";
import { asUnrealBlueprintError } from "../shared/errors.js";
import { assertJsonValue, isJsonObject, type JsonObject, type JsonValue } from "../shared/json.js";

const sessionSchema = z.object({
  editorSessionId: z.string().min(1).optional(),
  uproject: z.string().min(1).optional()
}).strict().optional();
const jsonObjectSchema = z.record(z.unknown());
const operationSchema = z.object({ operation: z.string().min(1) }).passthrough();

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
  blueprint_capabilities: z.object({
    session: sessionSchema,
    domain: z.string().min(1).optional(),
    operationNames: z.array(z.string().min(1)).max(200).optional()
  }).strict(),
  blueprint_inspect: z.object({
    session: sessionSchema,
    assetPath: z.string().min(1),
    facets: z.array(z.string().min(1)).max(32).optional(),
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

export async function invokeTool(name: ToolName, parameters: unknown, signal?: AbortSignal): Promise<JsonValue> {
  const parsed = toolSchemas[name].parse(parameters);
  const { session, rpcParams } = splitParameters(parsed);
  if (name === "unreal_status" && session.editorSessionId === undefined) {
    const sessions = await discoverSessions(session);
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
    return withUnrealClient({ session: { editorSessionId: selected.editorSessionId } }, (client) => client.invoke(name, rpcParams, signal === undefined ? {} : { signal }));
  }
  if (name === "blueprint_apply") {
    const selected = await selectSession(session);
    return invokeWriteWithRecovery({ session: { editorSessionId: selected.editorSessionId } }, name, rpcParams, signal === undefined ? {} : { signal });
  }
  return withUnrealClient({ session }, (client) => client.invoke(name, rpcParams, signal === undefined ? {} : { signal }));
}

export function failedToolResult(error: unknown): { isError: true; content: [{ type: "text"; text: string }]; structuredContent: { error: ReturnType<ReturnType<typeof asUnrealBlueprintError>["toJSON"]> } } {
  const normalized = asUnrealBlueprintError(error).toJSON();
  return {
    isError: true,
    content: [{ type: "text", text: JSON.stringify({ error: normalized }, null, 2) }],
    structuredContent: { error: normalized }
  };
}
