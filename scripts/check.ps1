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
. "$PSScriptRoot/unreal-version.ps1"

function Invoke-Checked([string]$FilePath, [string[]]$Arguments) {
    if ($DryRun) { Write-Host "[codex-unreal-blueprint] DRY-RUN: $FilePath $($Arguments -join ' ')"; return }
    Push-Location $repo
    try { & $FilePath @Arguments; if ($LASTEXITCODE -ne 0) { throw "命令失败（exit=$LASTEXITCODE）：$FilePath" } }
    finally { Pop-Location }
}

function Get-Value([string]$Name, $Fallback) {
    if ($Config -and (Test-Path -LiteralPath $Config -PathType Leaf)) {
        $local = Get-Content -LiteralPath $Config -Raw -Encoding UTF8 | ConvertFrom-Json
        $property = $local.PSObject.Properties[$Name]
        if ($null -ne $property -and $null -ne $property.Value) { return $property.Value }
    }
    return $Fallback
}

function Invoke-UnrealBuildAndTests {
    $engine = if ($EngineRoot) { $EngineRoot } else { Get-Value "engineRoot" "E:/UE_4.27" }
    $project = if ($UProject) { $UProject } else { Get-Value "uproject" "E:/Master/LuaSocial.uproject" }
    $unrealVersion = Get-SupportedUnrealVersion $engine $project -AllowMissing:$DryRun
    if (-not $SkipUnrealBuild) {
        $artifactsRoot = [System.IO.Path]::GetFullPath((Join-Path $repo "artifacts"))
        $target = [System.IO.Path]::GetFullPath((Join-Path $artifactsRoot "plugin-build-$unrealVersion"))
        if ([System.IO.Path]::GetDirectoryName($target) -ne $artifactsRoot) { throw "构建输出超出 artifacts 目录：$target" }
        if (-not $DryRun -and (Test-Path -LiteralPath $target)) { Remove-Item -LiteralPath $target -Recurse -Force }
        Invoke-Checked "$engine/Engine/Binaries/DotNET/AutomationTool.exe" @("BuildPlugin", "-Plugin=$repo/unreal/CodexUnrealBlueprint/CodexUnrealBlueprint.uplugin", "-Package=$target", "-TargetPlatforms=Win64", "-Rocket")
    }
    if ($RunUnrealTests -or [bool](Get-Value "runUeTests" $false)) {
        $filter = Get-Value "ueTestFilter" "CodexUnrealBlueprint"
        # Native modal-loop regression tests require Slate in a normal Editor process.
        Invoke-Checked "$engine/Engine/Binaries/Win64/UE4Editor.exe" @($project, "/Engine/Maps/Entry", "-nop4", "-nosplash", "-ExecCmds=Automation RunTests $filter", "-TestExit=Automation Test Queue Empty")
    }
}

Invoke-Checked "npm" @("run", "check")
Invoke-UnrealBuildAndTests
