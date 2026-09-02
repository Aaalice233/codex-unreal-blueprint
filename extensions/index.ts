import { StringEnum } from "@earendil-works/pi-ai";
import { defineTool, type ExtensionAPI } from "@earendil-works/pi-coding-agent";
import { Type, type TSchema } from "typebox";
import { discoverSessions, withUnrealClient, type SessionQuery } from "../src/client/index.js";
import { TOOL_NAMES, type ToolName } from "../src/shared/contracts.js";
import { asUnrealBlueprintError } from "../src/shared/errors.js";
import { assertJsonValue, type JsonObject, type JsonValue } from "../src/shared/json.js";

const sessionSchema = Type.Optional(Type.Object({
  editorSessionId: Type.Optional(Type.String({ minLength: 1 })),
  uproject: Type.Optional(Type.String({ minLength: 1 }))
}, { additionalProperties: false }));

const jsonObjectSchema = Type.Record(Type.String(), Type.Unknown());
const operationSchema = Type.Object({
  operation: Type.String({ minLength: 1 })
}, { additionalProperties: true });

const toolSchemas: Record<ToolName, TSchema> = {
  unreal_status: Type.Object({ session: sessionSchema }, { additionalProperties: false }),
  unreal_doctor: Type.Object({ session: sessionSchema }, { additionalProperties: false }),
  unreal_search: Type.Object({
    session: sessionSchema,
    query: Type.String({ minLength: 1 }),
    domain: Type.Optional(Type.String()),
    cursor: Type.Optional(Type.String()),
    limit: Type.Optional(Type.Integer({ minimum: 1, maximum: 200 }))
  }, { additionalProperties: false }),
  blueprint_capabilities: Type.Object({
    session: sessionSchema,
    domain: Type.Optional(Type.String()),
    operationNames: Type.Optional(Type.Array(Type.String({ minLength: 1 }), { maxItems: 200 }))
  }, { additionalProperties: false }),
  blueprint_inspect: Type.Object({
    session: sessionSchema,
    assetPath: Type.String({ minLength: 1 }),
    facets: Type.Optional(Type.Array(Type.String({ minLength: 1 }), { maxItems: 32 })),
    cursor: Type.Optional(Type.String()),
    limit: Type.Optional(Type.Integer({ minimum: 1, maximum: 500 }))
  }, { additionalProperties: false }),
  blueprint_validate: Type.Object({
    session: sessionSchema,
    operations: Type.Array(operationSchema, { minItems: 1, maxItems: 500 }),
    expectedStructureHashes: Type.Optional(jsonObjectSchema)
  }, { additionalProperties: false }),
  blueprint_apply: Type.Object({
    session: sessionSchema,
    requestId: Type.String({ minLength: 1, maxLength: 128 }),
    operations: Type.Array(operationSchema, { minItems: 1, maxItems: 500 }),
    expectedStructureHashes: Type.Optional(jsonObjectSchema)
  }, { additionalProperties: false }),
  blueprint_job: Type.Object({
    session: sessionSchema,
    action: StringEnum(["query", "wait", "cancel", "queryRequest"] as const),
    jobId: Type.Optional(Type.String({ minLength: 1 })),
    requestId: Type.Optional(Type.String({ minLength: 1 })),
    timeoutMs: Type.Optional(Type.Integer({ minimum: 1, maximum: 600_000 }))
  }, { additionalProperties: false }),
  blueprint_verify: Type.Object({
    session: sessionSchema,
    assetPaths: Type.Array(Type.String({ minLength: 1 }), { minItems: 1, maxItems: 200 }),
    expectations: Type.Optional(Type.Array(jsonObjectSchema, { maxItems: 500 })),
    compile: Type.Optional(Type.Boolean()),
    reload: Type.Optional(Type.Boolean())
  }, { additionalProperties: false }),
  blueprint_history: Type.Object({
    session: sessionSchema,
    action: StringEnum(["list", "get", "restore"] as const),
    jobId: Type.Optional(Type.String({ minLength: 1 })),
    requestId: Type.Optional(Type.String({ minLength: 1, maxLength: 128 })),
    cursor: Type.Optional(Type.String()),
    limit: Type.Optional(Type.Integer({ minimum: 1, maximum: 200 }))
  }, { additionalProperties: false })
};

const descriptions: Record<ToolName, string> = {
  unreal_status: "Discover and query the exact UE4.27 Editor session, PIE, source-control, dirty-package, and queue status.",
  unreal_doctor: "Check the Pi package, UE plugin, protocol, project configuration, port, permissions, and build environment.",
  unreal_search: "Search Blueprint assets, classes, members, properties, graph actions, and operation names with pagination.",
  blueprint_capabilities: "Read operation names, strict parameter JSON Schemas, and examples from the UE Operation Registry.",
  blueprint_inspect: "Read paged Blueprint facets, stable IDs, compilation state, and structure hashes without modifying assets.",
  blueprint_validate: "Validate a one-shot operation list in memory without creating a persistent plan or modifying assets.",
  blueprint_apply: "Start one automatic transactional Blueprint write job. Requires a unique requestId and returns a jobId.",
  blueprint_job: "Query, wait for, safely cancel, or recover the status of a Blueprint job or requestId.",
  blueprint_verify: "Independently compile, reload, and assert Blueprint disk structure after an operation.",
  blueprint_history: "List audit/backup records, inspect one job, or restore it under a new requestId and write lease."
};

function splitParameters(value: unknown, selectedEditorSessionId: string | undefined): {
  readonly session: SessionQuery;
  readonly rpcParams: JsonObject;
} {
  assertJsonValue(value);
  const object = value as JsonObject;
  const rawSession = object.session;
  const sessionObject = rawSession !== undefined && typeof rawSession === "object" && rawSession !== null && !Array.isArray(rawSession)
    ? rawSession as JsonObject
    : {};
  const session: SessionQuery = {
    ...(typeof sessionObject.editorSessionId === "string"
      ? { editorSessionId: sessionObject.editorSessionId }
      : selectedEditorSessionId === undefined ? {} : { editorSessionId: selectedEditorSessionId }),
    ...(typeof sessionObject.uproject === "string"
      ? { uproject: sessionObject.uproject }
      : process.env.PI_UNREAL_UPROJECT === undefined ? {} : { uproject: process.env.PI_UNREAL_UPROJECT })
  };
  const { session: _session, ...rpcParams } = object;
  return { session, rpcParams };
}

function displayResult(value: JsonValue): string {
  return JSON.stringify(value, null, 2);
}

export default function registerUnrealBlueprintExtension(pi: ExtensionAPI): void {
  let selectedEditorSessionId: string | undefined;

  for (const name of TOOL_NAMES) {
    pi.registerTool(defineTool({
      name,
      label: name,
      description: descriptions[name],
      parameters: toolSchemas[name],
      async execute(_toolCallId, params, signal) {
        const { session, rpcParams } = splitParameters(params, selectedEditorSessionId);
        try {
          const result = await withUnrealClient({ session }, async (client) => {
            selectedEditorSessionId = client.session.editorSessionId;
            return client.invoke(name, rpcParams, signal === undefined ? {} : { signal });
          });
          return {
            content: [{ type: "text" as const, text: displayResult(result) }],
            details: { tool: name, session: selectedEditorSessionId, result }
          };
        } catch (error) {
          throw asUnrealBlueprintError(error);
        }
      }
    }));
  }

  pi.on("session_start", async (_event, ctx) => {
    try {
      const sessions = await discoverSessions(
        process.env.PI_UNREAL_UPROJECT === undefined ? {} : { uproject: process.env.PI_UNREAL_UPROJECT }
      );
      const state = sessions.length === 0 ? "Disconnected / 未连接" : `${sessions.length} Editor session(s)`;
      ctx.ui.setStatus("pi-unreal-blueprint", `UE: ${state}`);
    } catch {
      ctx.ui.setStatus("pi-unreal-blueprint", "UE: Fault / 故障");
    }
  });

  pi.registerCommand("unreal-blueprint", {
    description: "List or select UE Editor sessions / 列出或选择 UE Editor 会话",
    handler: async (args, ctx) => {
      const requestedId = args.trim();
      const sessions = await discoverSessions(
        process.env.PI_UNREAL_UPROJECT === undefined ? {} : { uproject: process.env.PI_UNREAL_UPROJECT }
      );
      if (requestedId.length > 0) {
        const selected = sessions.find((session) => session.editorSessionId === requestedId);
        if (selected === undefined) {
          ctx.ui.notify(`Editor session not found / 未找到会话: ${requestedId}`, "error");
          return;
        }
        selectedEditorSessionId = selected.editorSessionId;
        ctx.ui.setStatus("pi-unreal-blueprint", `UE: ${selected.uproject} • Connected`);
        ctx.ui.notify(`Selected / 已选择: ${selected.editorSessionId}`, "info");
        return;
      }
      if (sessions.length === 0) {
        ctx.ui.notify("No running UE Editor session / 没有运行中的 UE Editor 会话", "error");
        return;
      }
      const summary = sessions.map((session) => `${session.editorSessionId} | PID ${session.pid} | ${session.uproject}`).join("\n");
      ctx.ui.notify(summary, "info");
    }
  });
}
