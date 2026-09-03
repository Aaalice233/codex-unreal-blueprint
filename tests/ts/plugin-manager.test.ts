import { resolve } from "node:path";
import { describe, expect, it } from "vitest";
import { resolvePluginTarget } from "../../src/cli/plugin-manager.js";

describe("project and engine plugin targets", () => {
  it("defaults to the project Plugins directory", () => {
    const project = resolve("Fixture/MyGame.uproject");
    expect(resolvePluginTarget({ uproject: project }))
      .toBe(resolve("Fixture/Plugins/PiUnrealBlueprint"));
  });

  it("resolves the engine Developer plugin directory", () => {
    const engineRoot = resolve("Fixture/UE_4.27");
    expect(resolvePluginTarget({ scope: "engine", engineRoot }))
      .toBe(resolve(engineRoot, "Engine/Plugins/Developer/PiUnrealBlueprint"));
  });

  it("rejects unknown scopes and incomplete targets", () => {
    expect(() => resolvePluginTarget({ scope: "global" })).toThrow(/scope/);
    expect(() => resolvePluginTarget({ scope: "engine" })).toThrow(/engineRoot/);
    expect(() => resolvePluginTarget({ scope: "project" })).toThrow(/uproject/);
  });
});
