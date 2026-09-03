[CmdletBinding()]
param(
    [string]$Config,
    [ValidateSet("project", "engine")]
    [string]$Scope,
    [string]$UProject,
    [string]$EngineRoot,
    [string]$PluginTarget,
    [string]$PiAgentDir,
    [string]$PiSource,
    [switch]$SkipPiInstall,
    [switch]$DryRun,
    [switch]$SkipUnrealBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)

$repo = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..")).Replace("\", "/").TrimEnd("/")
$isSourceCheckout = Test-Path -LiteralPath "$repo/.git"
$packageVersion = [string](Get-Content -LiteralPath "$repo/package.json" -Raw -Encoding UTF8 | ConvertFrom-Json).version
$defaultPiSource = if ($isSourceCheckout) { $repo } else { "npm:pi-unreal-blueprint@$packageVersion" }
$configPath = if ($Config) {
    [System.IO.Path]::GetFullPath($Config).Replace("\", "/")
}
else {
    "$repo/dev.local.json"
}

function Write-SetupStep {
    param([Parameter(Mandatory = $true)][string]$Text)
    Write-Host "[pi-unreal-blueprint setup] $Text"
}

function Invoke-SetupCommand {
    param([Parameter(Mandatory = $true)][string]$FilePath, [Parameter(Mandatory = $true)][string[]]$Arguments)
    $display = (@($FilePath) + $Arguments) -join " "
    if ($DryRun) {
        Write-SetupStep "DRY-RUN: $display"
        return
    }
    Push-Location $repo
    try {
        & $FilePath @Arguments
        if ($LASTEXITCODE -ne 0) { throw "命令失败（exit=$LASTEXITCODE）：$display" }
    }
    finally {
        Pop-Location
    }
}

function Get-SetupConfig {
    $values = [ordered]@{
        repo = $repo
        piAgentDir = Join-Path $HOME ".pi/agent"
        uproject = $null
        engineRoot = $null
        installScope = "project"
        uePluginTarget = $null
        piSource = $defaultPiSource
        skipPiInstall = $false
    }
    if (Test-Path -LiteralPath $configPath -PathType Leaf) {
        $local = Get-Content -LiteralPath $configPath -Raw -Encoding UTF8 | ConvertFrom-Json
        foreach ($property in $local.PSObject.Properties) { $values[$property.Name] = $property.Value }
    }

    if ($Scope) { $values.installScope = $Scope }
    if ($UProject) { $values.uproject = $UProject }
    if ($EngineRoot) { $values.engineRoot = $EngineRoot }
    if ($PluginTarget) { $values.uePluginTarget = $PluginTarget }
    if ($PiAgentDir) { $values.piAgentDir = $PiAgentDir }
    if ($PiSource) { $values.piSource = $PiSource }

    if ($values.installScope -notin @("project", "engine")) {
        throw "installScope 必须是 project 或 engine。"
    }
    if ($values.skipPiInstall -isnot [bool]) {
        throw "skipPiInstall 必须是 boolean。"
    }
    if ($SkipPiInstall) { $values.skipPiInstall = $true }
    foreach ($name in @("repo", "piAgentDir", "uproject", "engineRoot")) {
        if (-not ($values[$name] -is [string]) -or [string]::IsNullOrWhiteSpace($values[$name])) {
            throw "缺少配置项 $name。请通过参数或 dev.local.json 提供。"
        }
        $values[$name] = [System.IO.Path]::GetFullPath($values[$name]).Replace("\", "/").TrimEnd("/")
    }
    if (-not [string]::Equals($values.repo, $repo, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "配置 repo 与 setup.ps1 所在仓库不一致：配置=$($values.repo)，实际=$repo"
    }

    if ($values.uePluginTarget) {
        $values.uePluginTarget = [System.IO.Path]::GetFullPath([string]$values.uePluginTarget).Replace("\", "/").TrimEnd("/")
    }
    elseif ($values.installScope -eq "engine") {
        $values.uePluginTarget = "$($values.engineRoot)/Engine/Plugins/Developer/PiUnrealBlueprint"
    }
    else {
        $projectRoot = Split-Path -Parent $values.uproject
        $values.uePluginTarget = ([System.IO.Path]::GetFullPath((Join-Path $projectRoot "Plugins/PiUnrealBlueprint"))).Replace("\", "/")
    }
    return [pscustomobject]$values
}

function Assert-SetupPrerequisites {
    param([Parameter(Mandatory = $true)]$Settings)
    if ($DryRun) {
        Write-SetupStep "DRY-RUN: 检查 Node.js >= 22.19.0、npm、Pi、Visual Studio C++、UE4.27 和 .uproject"
        return
    }

    foreach ($command in @("node", "npm", "pi")) {
        if (-not (Get-Command $command -ErrorAction SilentlyContinue)) { throw "缺少命令：$command" }
    }
    $nodeVersion = (& node --version).Trim().TrimStart("v")
    if ($LASTEXITCODE -ne 0 -or ([version]$nodeVersion -lt [version]"22.19.0")) {
        throw "Node.js 版本必须 >= 22.19.0，当前为 $nodeVersion"
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio/Installer/vswhere.exe"
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) { throw "未找到 vswhere.exe。" }
    $visualStudio = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace(($visualStudio -join ""))) {
        throw "未找到带 C++ 工具链的 Visual Studio。"
    }

    foreach ($path in @(
        $Settings.piAgentDir,
        $Settings.uproject,
        "$($Settings.engineRoot)/Engine/Build/BatchFiles/RunUAT.bat",
        "$($Settings.engineRoot)/Engine/Binaries/Win64/UE4Editor.exe"
    )) {
        if (-not (Test-Path -LiteralPath $path)) { throw "缺少前置路径：$path" }
    }
    if ((Split-Path -Leaf $Settings.uePluginTarget) -ne "PiUnrealBlueprint") {
        throw "uePluginTarget 必须是名为 PiUnrealBlueprint 的独立插件目录。"
    }
}

function Test-InstalledFiles {
    param([Parameter(Mandatory = $true)]$Settings)
    if ($DryRun) {
        Write-SetupStep "DRY-RUN: doctor 校验 CLI 构建、插件 manifest 与受管文件哈希"
        return
    }

    $cli = "$repo/src/cli/dist/src/cli/index.js"
    $manifestPath = "$($Settings.uePluginTarget)/.pi-unreal-blueprint.manifest.json"
    if (-not (Test-Path -LiteralPath $cli -PathType Leaf)) { throw "doctor 失败：CLI 未构建：$cli" }
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { throw "doctor 失败：插件 manifest 不存在：$manifestPath" }

    $manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($null -eq $manifest.files -or @($manifest.files).Count -eq 0) { throw "doctor 失败：插件 manifest 没有受管文件。" }
    foreach ($file in @($manifest.files)) {
        $relative = [string]$file.path
        if ([string]::IsNullOrWhiteSpace($relative) -or [System.IO.Path]::IsPathRooted($relative) -or $relative -match "(^|/|\\)\.\.($|/|\\)") {
            throw "doctor 失败：manifest 含不安全路径：$relative"
        }
        $targetFile = [System.IO.Path]::GetFullPath((Join-Path $Settings.uePluginTarget $relative))
        if (-not (Test-Path -LiteralPath $targetFile -PathType Leaf)) { throw "doctor 失败：缺少插件文件：$relative" }
        $hash = (Get-FileHash -LiteralPath $targetFile -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($hash -ne [string]$file.sha256) { throw "doctor 失败：插件文件哈希不一致：$relative" }
    }
    Write-SetupStep "doctor 通过：CLI 已构建，插件受管文件清单一致。"
}

$settings = Get-SetupConfig
Assert-SetupPrerequisites $settings

# 源码 checkout 使用本地路径；npm 包执行时安装同版本的正式 package。
if (-not [bool]$settings.skipPiInstall) {
    Invoke-SetupCommand "pi" @("install", [string]$settings.piSource)
}
else {
    Write-SetupStep "已跳过 Pi package 安装。"
}

$devParameters = @{
    Action = "sync"
    Config = $configPath
    Scope = [string]$settings.installScope
    UProject = [string]$settings.uproject
    EngineRoot = [string]$settings.engineRoot
    PluginTarget = [string]$settings.uePluginTarget
    PiAgentDir = [string]$settings.piAgentDir
    DryRun = $DryRun
    SkipUnrealBuild = $SkipUnrealBuild
    SkipPackageCheck = (-not $isSourceCheckout)
}
Write-SetupStep "运行构建与安全插件同步。"
& "$PSScriptRoot/dev.ps1" @devParameters
if (-not $?) { throw "dev.ps1 sync 执行失败。" }

Test-InstalledFiles $settings
Write-SetupStep "setup 完成。请在 Pi 执行 /reload 或重启 Pi。"
