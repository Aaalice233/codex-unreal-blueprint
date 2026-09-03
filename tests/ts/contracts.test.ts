import { describe, expect, it } from "vitest";
import { TOOL_NAMES, rpcMethodForTool } from "../../src/shared/contracts.js";

describe("Codex MCP tool contract", () => {
  it("exposes exactly the nine PLAN.md tools", () => {
    expect(TOOL_NAMES).toEqual([
      "unreal_status",
      "unreal_doctor",
      "unreal_search",
      "blueprint_capabilities",
      "blueprint_inspect",
      "blueprint_validate",
      "blueprint_apply",
      "blueprint_job",
      "blueprint_verify"
    ]);
    expect(new Set(TOOL_NAMES).size).toBe(9);
  });

  it("maps every tool onto the current C++ namespaced RPC method", () => {
    expect(TOOL_NAMES.map((name) => rpcMethodForTool(name))).toEqual([
      "unreal.status",
      "unreal.doctor",
      "unreal.search",
      "blueprint.capabilities",
      "blueprint.inspect",
      "blueprint.validate",
      "blueprint.apply",
      "blueprint.job",
      "blueprint.verify"
    ]);
  });
});
