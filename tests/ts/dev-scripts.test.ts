import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { spawnSync } from "node:child_process";
import { afterAll, beforeAll, describe, expect, it } from "vitest";

const repo = resolve(import.meta.dirname, "../..").replaceAll("\\", "/");
const devScript = `${repo}/scripts/dev.ps1`;
const setupScript = `${repo}/scripts/setup.ps1`;
let sandbox = "";
let configPath = "";

function runPowerShell(script: string, args: readonly string[]) {
  return spawnSync(
    "pwsh",
    ["-NoLogo", "-NoProfile", "-NonInteractive", "-File", script, ...args],
    { cwd: repo, encoding: "utf8", timeout: 15_000 }
  );
}

beforeAll(() => {
  sandbox = mkdtempSync(join(tmpdir(), "pi-unreal-blueprint-dev-")).replaceAll("\\", "/");
  configPath = `${sandbox}/dev.local.json`;
  writeFileSync(configPath, `${JSON.stringify({
    repo,
    piAgentDir: `${sandbox}/pi-agent`,
    uproject: `${sandbox}/Fixture.uproject`,
    uePluginTarget: `${sandbox}/Plugins/PiUnrealBlueprint`,
    engineRoot: `${sandbox}/UE_4.27`,
    editorTarget: "FixtureEditor",
    runUeTests: false,
    ueTestFilter: "PiUnrealBlueprint"
  }, null, 2)}\n`, "utf8");
});

afterAll(() => {
  rmSync(sandbox, { recursive: true, force: true });
});

describe("PowerShell development workflow", () => {
  it("plans check and sync without touching the configured plugin target", () => {
    const target = `${sandbox}/Plugins/PiUnrealBlueprint`;
    const result = runPowerShell(devScript, ["sync", "-Config", configPath, "-DryRun", "-SkipUnrealBuild"]);

    expect(result.status, result.stderr).toBe(0);
    expect(result.stdout).toContain("npm run check");
    expect(result.stdout).toContain("UE Editor");
    expect(result.stdout).toContain("manifest");
    expect(result.stdout).toContain("保留所有非受管文件");
    expect(() => readFileSync(`${target}/.pi-unreal-blueprint.manifest.json`, "utf8")).toThrow();
  });

  it("plans setup as local Pi install, build/sync, and doctor without writes", () => {
    const result = runPowerShell(setupScript, ["-Config", configPath, "-DryRun", "-SkipUnrealBuild"]);

    expect(result.status, result.stderr).toBe(0);
    expect(result.stdout).toContain(`pi install ${repo}`);
    expect(result.stdout).toContain("运行构建与安全插件同步");
    expect(result.stdout).toContain("doctor");
  });

  it("rejects a publish message before planning any git mutation", () => {
    const result = runPowerShell(devScript, [
      "publish", "-Config", configPath, "-DryRun", "-SkipUnrealBuild", "-Message", "bad message"
    ]);

    expect(result.status).not.toBe(0);
    expect(`${result.stdout}${result.stderr}`).toContain("type(scope): 中文描述");
    expect(result.stdout).not.toContain("git add");
  });

  it("pushes independently and defers only local plugin sync", () => {
    const result = runPowerShell(devScript, [
      "publish", "-Config", configPath, "-DryRun", "-SkipUnrealBuild", "-Message", "feat(dev): 添加安全发布脚本"
    ]);

    expect(result.status, result.stderr).toBe(0);
    expect(result.stdout).toContain("git add --all");
    expect(result.stdout).toContain("core.editor=true commit -m");
    expect(result.stdout).toContain("git push origin <current-branch>");

    const source = readFileSync(devScript, "utf8");
    const publishBody = source.slice(source.indexOf("function Invoke-Publish"), source.indexOf("$settings = Get-DevelopmentConfig"));
    expect(publishBody.indexOf('Invoke-CheckedCommand "git" @("push"')).toBeLessThan(
      publishBody.indexOf("Test-TargetEditorRunning $Settings")
    );
    expect(publishBody).toContain("本机插件同步已延后");
    expect(source).toContain("BuildPlugin");
    for (const forbidden of ["push --force", "reset --hard", "git clean", "git stash"]) {
      expect(source).not.toContain(forbidden);
    }
  });
});
