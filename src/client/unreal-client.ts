import { assertWriteRequestId, rpcMethodForTool, type ToolName } from "../shared/contracts.js";
import { ERROR_CODES, UnrealBlueprintError } from "../shared/errors.js";
import type { JsonObject, JsonValue } from "../shared/json.js";
import { RpcClient, type RpcClientOptions } from "./rpc-client.js";
import { selectSession, type DiscoveryOptions, type EditorSession, type SessionQuery } from "./session-discovery.js";

export interface UnrealClientOptions {
  readonly session?: SessionQuery;
  readonly discovery?: DiscoveryOptions;
  readonly rpc?: RpcClientOptions;
}

export class UnrealClient {
  readonly session: EditorSession;
  readonly #rpc: RpcClient;

  private constructor(session: EditorSession, rpc: RpcClient) {
    this.session = session;
    this.#rpc = rpc;
  }

  static async connect(options: UnrealClientOptions = {}): Promise<UnrealClient> {
    const session = await selectSession(options.session, options.discovery);
    const rpc = await RpcClient.connect(session, options.rpc);
    return new UnrealClient(session, rpc);
  }

  async invoke(toolName: ToolName, params: JsonObject, options: { signal?: AbortSignal; timeoutMs?: number } = {}): Promise<JsonValue> {
    if (toolName === "blueprint_apply") assertWriteRequestId(params, toolName);
    if (toolName === "blueprint_history" && params.action === "restore") assertWriteRequestId(params, "blueprint_history restore");
    if (toolName === "blueprint_job" && params.action === "cancel" && typeof params.jobId !== "string") {
      throw new UnrealBlueprintError(ERROR_CODES.INVALID_ARGUMENT, "blueprint_job cancel requires jobId");
    }
    return this.#rpc.request(rpcMethodForTool(toolName), params, options);
  }

  close(): void {
    this.#rpc.close();
  }
}

export async function withUnrealClient<T>(options: UnrealClientOptions, run: (client: UnrealClient) => Promise<T>): Promise<T> {
  const client = await UnrealClient.connect(options);
  try {
    return await run(client);
  } finally {
    client.close();
  }
}
