[CmdletBinding()]
param(
    [Parameter(Position = 0)][ValidateSet("setup", "check", "sync", "publish")][string]$Action = "check",
    [string]$Message,
    [string]$Config,
    [ValidateSet("project", "engine")][string]$Scope,
    [string]$UProject,
    [string]$EngineRoot,
    [string]$PluginTarget,
    [string]$CodexExecutable,
    [switch]$DryRun,
    [switch]$SkipUnrealBuild,
    [switch]$RunUnrealTests
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$repo = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))

function Invoke-Checked([string]$FilePath, [string[]]$Arguments) {
    if ($DryRun) { Write-Host "[codex-unreal-blueprint] DRY-RUN: $FilePath $($Arguments -join ' ')"; return }
    Push-Location $repo
    try { & $FilePath @Arguments; if ($LASTEXITCODE -ne 0) { throw "命令失败（exit=$LASTEXITCODE）：$FilePath" } }
    finally { Pop-Location }
}

function Get-Value([string]$Name, $Fallback) {
    if ($Config -and (Test-Path -LiteralPath $Config -PathType Leaf)) {
        $local = Get-Content -LiteralPath $Config -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($null -ne $local.$Name) { return $local.$Name }
    }
    return $Fallback
}

function Invoke-UnrealBuildAndTests {
    $engine = Get-Value "engineRoot" $(if ($EngineRoot) { $EngineRoot } else { "E:/UE_4.27" })
    $project = Get-Value "uproject" $(if ($UProject) { $UProject } else { "E:/Master/LuaSocial.uproject" })
    if (-not $SkipUnrealBuild) {
        $target = Join-Path $repo "artifacts/plugin-build"
        if (-not $DryRun -and (Test-Path -LiteralPath $target)) { Remove-Item -LiteralPath $target -Recurse -Force }
        Invoke-Checked "$engine/Engine/Build/BatchFiles/RunUAT.bat" @("BuildPlugin", "-Plugin=$repo/unreal/CodexUnrealBlueprint/CodexUnrealBlueprint.uplugin", "-Package=$target", "-TargetPlatforms=Win64", "-Rocket")
    }
    if ($RunUnrealTests -or [bool](Get-Value "runUeTests" $false)) {
        $filter = Get-Value "ueTestFilter" "CodexUnrealBlueprint"
        Invoke-Checked "$engine/Engine/Binaries/Win64/UE4Editor-Cmd.exe" @($project, "-unattended", "-nop4", "-NullRHI", "-ExecCmds=Automation RunTests $filter;Quit", "-TestExit=Automation Test Queue Empty")
    }
}

function Invoke-Setup {
    $parameters = @{ DryRun = $DryRun; SkipUnrealBuild = $SkipUnrealBuild }
    if ($Config) { $parameters.Config = $Config }
    if ($Scope) { $parameters.Scope = $Scope }
    if ($UProject) { $parameters.UProject = $UProject }
    if ($EngineRoot) { $parameters.EngineRoot = $EngineRoot }
    if ($PluginTarget) { $parameters.PluginTarget = $PluginTarget }
    if ($CodexExecutable) { $parameters.CodexExecutable = $CodexExecutable }
    & "$PSScriptRoot/setup.ps1" @parameters
    if (-not $?) { throw "setup.ps1 执行失败。" }
}

switch ($Action) {
    "setup" { Invoke-Setup }
    "sync" { Invoke-Setup }
    "check" { Invoke-Checked "npm" @("run", "check"); Invoke-UnrealBuildAndTests }
    "publish" {
        if ([string]::IsNullOrWhiteSpace($Message) -or $Message.Length -gt 72 -or $Message -notmatch "^(feat|fix|refactor|perf|style|docs|test|chore)(\([A-Za-z0-9._/-]+\))?: .*[\u4e00-\u9fff].*$") { throw "publish 需要不超过 72 字的 type(scope): 中文描述。" }
        Invoke-Checked "npm" @("run", "check")
        Invoke-UnrealBuildAndTests
        Invoke-Checked "git" @("add", "--all")
        Invoke-Checked "git" @("-c", "core.editor=true", "commit", "-m", $Message)
        $branch = if ($DryRun) { "main" } else { (& git -C $repo branch --show-current).Trim() }
        Invoke-Checked "git" @("push", "origin", $branch)
    }
}
