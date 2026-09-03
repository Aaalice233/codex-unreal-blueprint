#!/usr/bin/env node
import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { pathToFileURL } from "node:url";
import { TOOL_NAMES, type ToolName } from "../shared/contracts.js";
import { failedToolResult, invokeTool, toolAnnotations, toolDescriptions, toolSchemas } from "./tools.js";

export function createMcpServer(): McpServer {
  const server = new McpServer({ name: "codex-unreal-blueprint", version: "1.0.0" }, {
    instructions: "Connects Codex to a local UE4.27 Editor. Select an exact Editor session when discovery is ambiguous. Every write requires a unique requestId and uncertain writes must be queried, never replayed."
  });
  for (const name of TOOL_NAMES) {
    server.registerTool(name, {
      description: toolDescriptions[name],
      inputSchema: toolSchemas[name],
      annotations: toolAnnotations[name]
    }, async (parameters: unknown, extra: { signal: AbortSignal }) => {
      try {
        const result = await invokeTool(name as ToolName, parameters, extra.signal);
        return {
          content: [{ type: "text" as const, text: JSON.stringify(result, null, 2) }],
          structuredContent: { result }
        };
      } catch (error) {
        return failedToolResult(error);
      }
    });
  }
  return server;
}

async function main(): Promise<void> {
  await createMcpServer().connect(new StdioServerTransport());
}

if (process.argv[1] !== undefined && import.meta.url === pathToFileURL(process.argv[1]).href) {
  main().catch((error: unknown) => {
    console.error(error);
    process.exitCode = 1;
  });
}
