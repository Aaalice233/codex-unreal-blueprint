[CmdletBinding()]
param(
    [string]$Config,
    [string]$UProject,
    [string]$EngineRoot,
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

Invoke-Checked "npm" @("run", "check")
Invoke-UnrealBuildAndTests
