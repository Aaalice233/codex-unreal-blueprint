import { mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { resolve } from "node:path";
import { spawnSync } from "node:child_process";
import { afterAll, beforeAll, describe, expect, it } from "vitest";

const repo = resolve(import.meta.dirname, "../..").replaceAll("\\", "/");
const sandbox = `${repo}/.codex-unreal-blueprint/installer-test`;
const configPath = `${sandbox}/dev.local.json`;
const checkScript = `${repo}/scripts/check.ps1`;
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

  it("plans the shared TypeScript check without running a UE build", () => {
    const result = runPowerShell(checkScript, ["-Config", configPath, "-DryRun", "-SkipUnrealBuild"]);
    expect(result.status, result.stderr).toBe(0);
    expect(result.stdout).toContain("DRY-RUN: npm run check");
    expect(result.stdout).not.toContain("RunUAT.bat");
  });

  it("keeps destructive boundaries explicit in the installer", () => {
    const source = readFileSync(setupScript, "utf8");
    expect(source).toContain("Resolve-ManagedPath");
    expect(source).toContain("目标存在非受管同名文件");
    expect(source).toContain('$oldByPath.ContainsKey("CodexUnrealBlueprint.uplugin")');
    expect(source).toContain('$file.path.StartsWith("Binaries/"');
    expect(source).toContain('New-CodexPluginInstallStage');
    expect(source).toContain('$manifest.version = "$baseVersion+codex.$cachebuster"');
    expect(source).toContain('$matching.Count -eq 1');
    expect(source.indexOf("Assert-EditorClosed $settings")).toBeLessThan(source.indexOf("Sync-ManagedDirectory $settings.uePluginTarget"));
    expect(source).toContain('Get-SourceFiles $packageRoot @("Binaries")');
    expect(source).toContain('Get-SourceFiles $settings.uePluginTarget @("Binaries")');
    expect(source.indexOf('Invoke-Checked "$($settings.engineRoot)/Engine/Build/BatchFiles/RunUAT.bat"'))
      .toBeLessThan(source.indexOf('Get-SourceFiles $packageRoot @("Binaries")'));
    for (const forbidden of ["reset --hard", "git clean", "git stash", "push --force"]) expect(source).not.toContain(forbidden);
  });
});
