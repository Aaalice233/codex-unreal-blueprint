import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";
import { mkdir, rm } from "node:fs/promises";
import { resolve } from "node:path";
import { afterEach, describe, expect, it } from "vitest";
import { TOOL_NAMES } from "../../src/shared/contracts.js";
import { failedToolResult, invokeTool, toolAnnotations, toolSchemas } from "../../src/mcp/tools.js";
import { UnrealBlueprintError } from "../../src/shared/errors.js";

const testState = resolve(".codex-unreal-blueprint/mcp-test-state");

afterEach(async () => {
  await rm(testState, { recursive: true, force: true });
});

describe("Codex stdio MCP server", () => {
  it("publishes the nine tools with the required annotations over a real stdio round trip", async () => {
    await mkdir(testState, { recursive: true });
    const childEnvironment = Object.fromEntries(Object.entries(process.env).filter((entry): entry is [string, string] => entry[1] !== undefined));
    childEnvironment.LOCALAPPDATA = testState;
    const transport = new StdioClientTransport({
      command: process.execPath,
      args: [resolve("dist/mcp/index.js")],
      cwd: process.cwd(),
      env: childEnvironment,
      stderr: "pipe"
    });
    const client = new Client({ name: "codex-unreal-blueprint-test", version: "1.0.0" });
    try {
      await client.connect(transport);
      const listed = await client.listTools();
      expect(listed.tools.map((tool) => tool.name)).toEqual(TOOL_NAMES);
      for (const tool of listed.tools) {
        expect(tool.annotations).toMatchObject(toolAnnotations[tool.name as keyof typeof toolAnnotations]);
      }
      const status = await client.callTool({ name: "unreal_status", arguments: {} });
      expect(status.structuredContent).toEqual({ result: { connected: false, ambiguous: false, sessions: [] } });
    } finally {
      await client.close();
    }
  });

  it("keeps operation schemas dynamic while rejecting unknown envelope fields", () => {
    expect(toolSchemas.blueprint_validate.safeParse({ operations: [{ operation: "future.operation", futureField: true }] }).success).toBe(true);
    expect(toolSchemas.blueprint_validate.safeParse({ operations: [{ operation: "future.operation" }], typo: true }).success).toBe(false);
  });

  it("returns a structured disconnected status when no Editor session exists", async () => {
    await mkdir(testState, { recursive: true });
    const previous = process.env.LOCALAPPDATA;
    process.env.LOCALAPPDATA = testState;
    try {
      await expect(invokeTool("unreal_status", {})).resolves.toEqual({ connected: false, ambiguous: false, sessions: [] });
    } finally {
      if (previous === undefined) delete process.env.LOCALAPPDATA;
      else process.env.LOCALAPPDATA = previous;
    }
  });

  it("preserves stable UE failure context in structured and readable output", () => {
    const result = failedToolResult(new UnrealBlueprintError("COMPILE_FAILED", "Blueprint compile failed", {
      context: { assetPath: "/Game/Test/BP_Test.BP_Test", operationIndex: 2, ueCallsite: "CompileBlueprint", compilerMessages: ["Error: invalid pin"] }
    }));
    expect(result.structuredContent.error).toMatchObject({
      code: "COMPILE_FAILED",
      context: { assetPath: "/Game/Test/BP_Test.BP_Test", operationIndex: 2, ueCallsite: "CompileBlueprint" }
    });
    expect(result.content[0].text).toContain("Error: invalid pin");
  });
});
