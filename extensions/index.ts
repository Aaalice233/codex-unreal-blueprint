import { StringEnum } from "@earendil-works/pi-ai";
import { defineTool, type ExtensionAPI } from "@earendil-works/pi-coding-agent";
import { Type, type TSchema } from "typebox";
import { discoverSessions, invokeWriteWithRecovery, selectSession, withUnrealClient, type EditorSession, type SessionQuery } from "../src/client/index.js";
import { TOOL_NAMES, type ToolName } from "../src/shared/contracts.js";
import { asUnrealBlueprintError, type SerializedError } from "../src/shared/errors.js";
import { assertJsonValue, isJsonObject, type JsonObject, type JsonValue } from "../src/shared/json.js";

const sessionSchema = Type.Optional(Type.Object({
  editorSessionId: Type.Optional(Type.String({ minLength: 1 })),
  uproject: Type.Optional(Type.String({ minLength: 1 }))
}, { additionalProperties: false }));
const jsonObjectSchema = Type.Record(Type.String(), Type.Unknown());
// Operation Registry owns each operation's strict schema; this boundary only keeps the always-present discriminator resident.
const operationSchema = Type.Object({ operation: Type.String({ minLength: 1 }) }, { additionalProperties: true });

const toolSchemas: Record<ToolName, TSchema> = {
  unreal_status: Type.Object({ session: sessionSchema }, { additionalProperties: false }),
  unreal_doctor: Type.Object({ session: sessionSchema }, { additionalProperties: false }),
  unreal_search: Type.Object({
    session: sessionSchema,
    query: Type.String({ minLength: 1 }),
    domain: Type.Optional(StringEnum(["asset", "class", "member", "property", "action", "operation"] as const)),
    context: Type.Optional(jsonObjectSchema),
    cursor: Type.Optional(Type.String()),
    limit: Type.Optional(Type.Integer({ minimum: 1, maximum: 200 }))
  }, { additionalProperties: false }),
  blueprint_capabilities: Type.Object({
    session: sessionSchema,
    domain: Type.Optional(Type.String({ minLength: 1 })),
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
  blueprint_job: Type.Union([
    Type.Object({
      session: sessionSchema,
      action: Type.Literal("query"),
      jobId: Type.String({ minLength: 1 }),
      requestId: Type.Optional(Type.String({ minLength: 1, maxLength: 128 }))
    }, { additionalProperties: false }),
    Type.Object({
      session: sessionSchema,
      action: Type.Literal("query"),
      jobId: Type.Optional(Type.String({ minLength: 1 })),
      requestId: Type.String({ minLength: 1, maxLength: 128 })
    }, { additionalProperties: false }),
    Type.Object({
      session: sessionSchema,
      action: Type.Literal("wait"),
      jobId: Type.String({ minLength: 1 }),
      requestId: Type.Optional(Type.String({ minLength: 1, maxLength: 128 })),
      timeoutMs: Type.Optional(Type.Integer({ minimum: 0, maximum: 600_000 }))
    }, { additionalProperties: false }),
    Type.Object({
      session: sessionSchema,
      action: Type.Literal("wait"),
      jobId: Type.Optional(Type.String({ minLength: 1 })),
      requestId: Type.String({ minLength: 1, maxLength: 128 }),
      timeoutMs: Type.Optional(Type.Integer({ minimum: 0, maximum: 600_000 }))
    }, { additionalProperties: false }),
    Type.Object({
      session: sessionSchema,
      action: Type.Literal("cancel"),
      jobId: Type.String({ minLength: 1 })
    }, { additionalProperties: false })
  ]),
  blueprint_verify: Type.Object({
    session: sessionSchema,
    assetPaths: Type.Array(Type.String({ minLength: 1 }), { minItems: 1, maxItems: 200 }),
    expectations: Type.Optional(Type.Array(jsonObjectSchema, { maxItems: 500 })),
    compile: Type.Optional(Type.Boolean()),
    reload: Type.Optional(Type.Boolean())
  }, { additionalProperties: false })
};

const descriptions: Record<ToolName, string> = {
  unreal_status: "Discover/select the exact UE4.27 Editor and return session, PIE, source-control, dirty-package, and queue status.",
  unreal_doctor: "Check UE plugin, protocol, project configuration, port, permissions, and build environment.",
  unreal_search: "Search Blueprint assets, classes, members, properties, graph actions, or operations with pagination.",
  blueprint_capabilities: "Fetch strict operation parameter JSON Schemas and examples from the UE Operation Registry on demand.",
  blueprint_inspect: "Read paged Blueprint facets, stable IDs, compile state, and structure hashes without modifying assets.",
  blueprint_validate: "Validate a one-shot operation list in memory without persisting a plan or changing assets.",
  blueprint_apply: "Start an automatic transactional Blueprint write job using one unique requestId; no confirmation dialog.",
  blueprint_job: "Query, wait for, or safely cancel a job; requestId may be used to resolve an uncertain write.",
  blueprint_verify: "Independently compile, reload, and assert Blueprint disk structure."
};

function splitParameters(value: unknown, selectedEditorSessionId: string | undefined): { session: SessionQuery; rpcParams: JsonObject } {
  assertJsonValue(value);
  const object = value as JsonObject;
  const rawSession = object.session;
  const sessionObject = isJsonObject(rawSession) ? rawSession : {};
  const session: SessionQuery = {
    ...(typeof sessionObject.editorSessionId === "string" ? { editorSessionId: sessionObject.editorSessionId } : selectedEditorSessionId === undefined ? {} : { editorSessionId: selectedEditorSessionId }),
    ...(typeof sessionObject.uproject === "string" ? { uproject: sessionObject.uproject } : process.env.PI_UNREAL_UPROJECT === undefined ? {} : { uproject: process.env.PI_UNREAL_UPROJECT })
  };
  const { session: _session, ...rpcParams } = object;
  return { session, rpcParams };
}

function projectName(session: EditorSession): string {
  return session.uproject.replaceAll("\\", "/").split("/").at(-1)?.replace(/\.uproject$/i, "") ?? "Editor";
}

function collectPartialAssets(value: JsonValue): string[] {
  if (!isJsonObject(value)) return [];
  const report = isJsonObject(value.failureReport) ? value.failureReport : value;
  const assets: string[] = [];
  const records = report.assets;
  if (Array.isArray(records)) {
    for (const record of records) {
      if (isJsonObject(record) && typeof record.assetPath === "string") {
        assets.push(`${typeof record.state === "string" ? record.state : "unknown"}: ${record.assetPath}`);
      }
    }
  }
  for (const key of ["modified", "saved", "notSaved", "unknown", "modifiedAssets", "savedAssets", "notSavedAssets", "unknownAssets"]) {
    const list = report[key];
    if (Array.isArray(list)) for (const item of list) if (typeof item === "string") assets.push(`${key}: ${item}`);
  }
  for (const key of ["result", "error", "details"]) {
    const nested = report[key];
    if (isJsonObject(nested)) assets.push(...collectPartialAssets(nested));
  }
  return [...new Set(assets)];
}

function uncertainState(value: JsonValue): "Partial" | "State unknown" | undefined {
  if (!isJsonObject(value)) return undefined;
  if (value.stateUnknown === true) return "State unknown";
  if (value.partial === true) return "Partial";
  for (const key of ["result", "error", "details", "failureReport"]) {
    const nested = value[key];
    if (isJsonObject(nested)) {
      const state = uncertainState(nested);
      if (state !== undefined) return state;
    }
  }
  return undefined;
}

function resultJobId(value: JsonValue): string | undefined {
  return isJsonObject(value) && typeof value.jobId === "string" ? value.jobId : undefined;
}

function isTerminalFailure(value: JsonValue): boolean {
  return isJsonObject(value) && (value.phase === "Failed" || value.phase === "Cancelled"
    || value.terminalPhase === "Failed" || value.terminalPhase === "Cancelled");
}

function isTerminalResult(value: JsonValue): boolean {
  return isJsonObject(value) && (value.terminal === true || value.state === "terminal");
}

export default function registerUnrealBlueprintExtension(pi: ExtensionAPI): void {
  let selectedEditorSessionId: string | undefined;
  let selectedProject = "Disconnected";
  let lastJobId: string | undefined;
  let lastJobSummary: JsonValue | undefined;
  let lastError: SerializedError | undefined;
  let partialAssets: string[] = [];
  let setFooter: ((text: string) => void) | undefined;

  const footer = (state: string): void => setFooter?.(`UE: ${selectedProject} • ${state}`);

  for (const name of TOOL_NAMES) {
    pi.registerTool(defineTool({
      name,
      label: name,
      description: descriptions[name],
      parameters: toolSchemas[name],
      async execute(_toolCallId, params, signal) {
        const { session, rpcParams } = splitParameters(params, selectedEditorSessionId);
        try {
          let result: JsonValue;
          if (name === "unreal_status" && session.editorSessionId === undefined) {
            const sessions = await discoverSessions(session);
            if (sessions.length !== 1) {
              result = {
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
            } else {
              const onlySession = sessions[0] as EditorSession;
              selectedEditorSessionId = onlySession.editorSessionId;
              selectedProject = projectName(onlySession);
              result = await withUnrealClient({ session: { editorSessionId: onlySession.editorSessionId } }, (client) => client.invoke(name, rpcParams, signal === undefined ? {} : { signal }));
            }
          } else if (name === "blueprint_apply") {
            const selected = await selectSession(session);
            selectedEditorSessionId = selected.editorSessionId;
            selectedProject = projectName(selected);
            result = await invokeWriteWithRecovery(
              { session: { editorSessionId: selected.editorSessionId } },
              name,
              rpcParams,
              signal === undefined ? {} : { signal }
            );
          } else {
            result = await withUnrealClient({ session }, async (client) => {
              selectedEditorSessionId = client.session.editorSessionId;
              selectedProject = projectName(client.session);
              return client.invoke(name, rpcParams, signal === undefined ? {} : { signal });
            });
          }
          lastError = undefined;
          const jobId = resultJobId(result);
          if (jobId !== undefined) lastJobId = jobId;
          if (name === "blueprint_job" || name === "blueprint_apply") lastJobSummary = result;
          partialAssets = collectPartialAssets(result);
          const uncertain = uncertainState(result);
          const footerState = uncertain ?? (isTerminalFailure(result)
            ? "Failed"
            : name === "unreal_status" && isJsonObject(result) && result.connected === false
              ? result.ambiguous === true ? "Multiple sessions" : "Disconnected"
              : (name === "blueprint_apply" || name === "blueprint_job") && !isTerminalResult(result)
                ? `Job ${lastJobId ?? "running"}`
                : "Connected");
          footer(footerState);
          return { content: [{ type: "text" as const, text: JSON.stringify(result, null, 2) }], details: { tool: name, session: selectedEditorSessionId, result } };
        } catch (error) {
          const normalized = asUnrealBlueprintError(error);
          lastError = normalized.toJSON();
          const details = normalized.context?.details;
          partialAssets = details === undefined ? [] : collectPartialAssets(details);
          footer("Failed");
          throw normalized;
        }
      }
    }));
  }

  pi.on("session_start", async (_event, ctx) => {
    setFooter = (text) => ctx.ui.setStatus("pi-unreal-blueprint", text);
    try {
      const sessions = await discoverSessions(process.env.PI_UNREAL_UPROJECT === undefined ? {} : { uproject: process.env.PI_UNREAL_UPROJECT });
      if (sessions.length === 1) {
        selectedEditorSessionId = sessions[0]?.editorSessionId;
        selectedProject = sessions[0] === undefined ? "Disconnected" : projectName(sessions[0]);
        footer("Connected");
      } else footer(sessions.length === 0 ? "Disconnected" : `${sessions.length} sessions`);
    } catch {
      footer("Failed");
    }
  });

  pi.registerCommand("unreal-blueprint", {
    description: "UE Blueprint sessions, jobs, errors, partial assets, and doctor / UE 蓝图管理",
    handler: async (args, ctx) => {
      const query = process.env.PI_UNREAL_UPROJECT === undefined ? {} : { uproject: process.env.PI_UNREAL_UPROJECT };
      let sessions: EditorSession[];
      try {
        sessions = await discoverSessions(query);
      } catch (error) {
        const normalized = asUnrealBlueprintError(error);
        lastError = normalized.toJSON();
        footer("Failed");
        ctx.ui.notify(`[${normalized.code}] ${normalized.message}`, "error");
        return;
      }
      const directId = args.trim();
      if (directId.length > 0) {
        const direct = sessions.find((session) => session.editorSessionId === directId);
        if (direct === undefined) ctx.ui.notify(`Editor session not found / 未找到会话: ${directId}`, "error");
        else {
          selectedEditorSessionId = direct.editorSessionId;
          selectedProject = projectName(direct);
          footer("Connected");
          ctx.ui.notify(`Selected / 已选择: ${direct.editorSessionId}`, "info");
        }
        return;
      }
      if (!ctx.hasUI) {
        ctx.ui.notify(JSON.stringify({ selectedEditorSessionId, sessions, lastJobId, lastJobSummary, lastError, partialAssets }, null, 2), "info");
        return;
      }
      const action = await ctx.ui.select("Unreal Blueprint / UE 蓝图", [
        "Sessions / 会话",
        "Status / 状态",
        "Jobs / Job",
        "Errors & partial assets / 错误与部分资产",
        "Doctor / 诊断"
      ]);
      if (action === "Sessions / 会话") {
        if (sessions.length === 0) return ctx.ui.notify("No running UE Editor / 没有运行中的 UE Editor", "error");
        const choices = sessions.map((session) => `${session.editorSessionId} | PID ${session.pid} | ${session.uproject}`);
        const choice = await ctx.ui.select("Select exact Editor / 选择准确 Editor", choices);
        const index = choice === undefined ? -1 : choices.indexOf(choice);
        const selected = sessions[index];
        if (selected !== undefined) {
          selectedEditorSessionId = selected.editorSessionId;
          selectedProject = projectName(selected);
          footer("Connected");
        }
        return;
      }
      if (action === "Errors & partial assets / 错误与部分资产") {
        return ctx.ui.notify(JSON.stringify({ error: lastError ?? null, partialAssets }, null, 2), lastError === undefined ? "info" : "error");
      }
      if (action === "Jobs / Job") {
        if (lastJobId === undefined) return ctx.ui.notify("No job in this Pi session / 当前 Pi 会话没有 Job", "info");
        try {
          const jobId = lastJobId;
          const selectedSession = selectedEditorSessionId === undefined ? {} : { editorSessionId: selectedEditorSessionId };
          const result = await withUnrealClient({ session: selectedSession }, (client) => client.invoke("blueprint_job", { action: "query", jobId }));
          lastJobSummary = result;
          return ctx.ui.notify(JSON.stringify(result, null, 2), "info");
        } catch (error) {
          const normalized = asUnrealBlueprintError(error);
          lastError = normalized.toJSON();
          footer("Failed");
          return ctx.ui.notify(`[${normalized.code}] ${normalized.message}`, "error");
        }
      }
      const tool = action === "Doctor / 诊断" ? "unreal_doctor" : "unreal_status";
      try {
        const selectedSession = selectedEditorSessionId === undefined ? {} : { editorSessionId: selectedEditorSessionId };
        const result = await withUnrealClient({ session: selectedSession }, (client) => client.invoke(tool, {}));
        ctx.ui.notify(JSON.stringify(result, null, 2), "info");
      } catch (error) {
        const normalized = asUnrealBlueprintError(error);
        lastError = normalized.toJSON();
        footer("Failed");
        ctx.ui.notify(`[${normalized.code}] ${normalized.message}`, "error");
      }
    }
  });
}
