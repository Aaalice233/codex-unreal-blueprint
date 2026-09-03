import { mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { resolve } from "node:path";
import { spawnSync } from "node:child_process";
import { afterAll, beforeAll, describe, expect, it } from "vitest";

const repo = resolve(import.meta.dirname, "../..").replaceAll("\\", "/");
const sandbox = `${repo}/.codex-unreal-blueprint/installer-test`;
const configPath = `${sandbox}/dev.local.json`;
const devScript = `${repo}/scripts/dev.ps1`;
const setupScript = `${repo}/scripts/setup.ps1`;

function runPowerShell(script: string, args: readonly string[]) {
  return spawnSync("pwsh", ["-NoLogo", "-NoProfile", "-NonInteractive", "-File", script, ...args], {
    cwd: repo, encoding: "utf8", timeout: 30_000
  });
}

beforeAll(() => {
  mkdirSync(sandbox, { recursive: true });
  writeFileSync(configPath, `${JSON.stringify({
    repo,
    uproject: `${sandbox}/Fixture.uproject`,
    uePluginTarget: `${sandbox}/Plugins/CodexUnrealBlueprint`,
    engineRoot: `${sandbox}/UE_4.27`,
    codexExecutable: process.execPath.replaceAll("\\", "/"),
    codexPluginTarget: `${sandbox}/home/plugins/codex-unreal-blueprint`,
    marketplacePath: `${sandbox}/home/.agents/plugins/marketplace.json`,
    runUeTests: false
  }, null, 2)}\n`, "utf8");
});

afterAll(() => rmSync(sandbox, { recursive: true, force: true }));

describe("PowerShell development workflow", () => {
  it("plans a Codex Marketplace and UE managed install without writes", () => {
    const result = runPowerShell(setupScript, ["-Config", configPath, "-DryRun", "-SkipUnrealBuild"]);
    expect(result.status, result.stderr).toBe(0);
    expect(result.stdout).toContain("npm run check");
    expect(result.stdout).toContain("CodexUnrealBlueprint");
    expect(result.stdout).toContain("codex-unreal-blueprint");
    expect(result.stdout).toContain("Marketplace");
    expect(result.stdout).toContain("plugin add codex-unreal-blueprint@personal");
    expect(() => readFileSync(`${sandbox}/Plugins/CodexUnrealBlueprint/.codex-unreal-blueprint.manifest.json`, "utf8")).toThrow();
  });

  it("routes dev sync through the same safe setup implementation", () => {
    const result = runPowerShell(devScript, ["sync", "-Config", configPath, "-DryRun", "-SkipUnrealBuild"]);
    expect(result.status, result.stderr).toBe(0);
    expect(result.stdout).toContain("保留非受管文件");
    expect(result.stdout).toContain("plugin add codex-unreal-blueprint@personal");
  });

  it("rejects an invalid publish message before any git mutation", () => {
    const result = runPowerShell(devScript, ["publish", "-Config", configPath, "-DryRun", "-SkipUnrealBuild", "-Message", "bad message"]);
    expect(result.status).not.toBe(0);
    expect(`${result.stdout}${result.stderr}`).toContain("type(scope): 中文描述");
    expect(result.stdout).not.toContain("git add");
  });

  it("keeps destructive boundaries explicit in the installer", () => {
    const source = readFileSync(setupScript, "utf8");
    expect(source).toContain("Resolve-ManagedPath");
    expect(source).toContain("目标存在非受管同名文件");
    expect(source.indexOf("Assert-EditorClosed $settings")).toBeLessThan(source.indexOf("Sync-ManagedDirectory $settings.uePluginTarget"));
    for (const forbidden of ["reset --hard", "git clean", "git stash", "push --force"]) expect(source).not.toContain(forbidden);
  });
});
