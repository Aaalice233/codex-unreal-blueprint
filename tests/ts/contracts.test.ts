import { describe, expect, it } from "vitest";
import { TOOL_NAMES, rpcMethodForTool } from "../../src/shared/contracts.js";

 describe("Pi tool contract", () => {
  it("exposes exactly the ten PLAN.md tools", () => {
    expect(TOOL_NAMES).toEqual([
      "unreal_status",
      "unreal_doctor",
      "unreal_search",
      "blueprint_capabilities",
      "blueprint_inspect",
      "blueprint_validate",
      "blueprint_apply",
      "blueprint_job",
      "blueprint_verify",
      "blueprint_history"
    ]);
    expect(new Set(TOOL_NAMES).size).toBe(10);
  });

  it("maps tools onto stable namespaced RPC methods", () => {
    expect(rpcMethodForTool("blueprint_apply")).toBe("blueprint.apply");
    expect(rpcMethodForTool("unreal_status")).toBe("unreal.status");
  });
});
