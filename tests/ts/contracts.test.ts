import { describe, expect, it } from "vitest";
import { TOOL_NAMES, rpcMethodForTool } from "../../src/shared/contracts.js";

describe("Codex MCP tool contract", () => {
  it("exposes layered Unreal asset tools alongside Blueprint automation", () => {
    expect(TOOL_NAMES).toEqual([
      "unreal_status",
      "unreal_doctor",
      "unreal_search",
      "unreal_asset_inspect",
      "unreal_asset_compare",
      "unreal_asset_referencers",
      "blueprint_capabilities",
      "blueprint_inspect",
      "blueprint_validate",
      "blueprint_apply",
      "blueprint_job",
      "blueprint_verify"
    ]);
    expect(new Set(TOOL_NAMES).size).toBe(12);
  });

  it("maps every tool onto the current C++ namespaced RPC method", () => {
    expect(TOOL_NAMES.map((name) => rpcMethodForTool(name))).toEqual([
      "unreal.status",
      "unreal.doctor",
      "unreal.search",
      "unreal.asset.inspect",
      "unreal.asset.compare",
      "unreal.asset.referencers",
      "blueprint.capabilities",
      "blueprint.inspect",
      "blueprint.validate",
      "blueprint.apply",
      "blueprint.job",
      "blueprint.verify"
    ]);
  });
});
