[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet("setup", "check", "sync", "publish")]
    [string]$Action = "check",

    [string]$Message,
    [string]$Config,
    [ValidateSet("project", "engine")]
    [string]$Scope,
    [string]$UProject,
    [string]$EngineRoot,
    [string]$PluginTarget,
    [string]$PiAgentDir,
    [switch]$DryRun,
    [switch]$SkipUnrealBuild,
    [switch]$SkipPackageCheck,
    [switch]$RunUnrealTests
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)

$script:ScriptDir = $PSScriptRoot
$script:ActualRepo = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$script:ManifestName = ".pi-unreal-blueprint.manifest.json"
$script:PluginSource = Join-Path $script:ActualRepo "unreal/PiUnrealBlueprint"

function ConvertTo-NormalizedPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    return [System.IO.Path]::GetFullPath($Path).Replace("\", "/").TrimEnd("/")
}

function Write-Step {
    param([Parameter(Mandatory = $true)][string]$Text)
    Write-Host "[pi-unreal-blueprint] $Text"
}

function Format-CommandArgument {
    param([Parameter(Mandatory = $true)][string]$Value)
    if ($Value -match '[\s"]') {
        return '"' + $Value.Replace('"', '\"') + '"'
    }
    return $Value
}

function Invoke-CheckedCommand {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [string]$WorkingDirectory = $script:ActualRepo
    )

    $display = (@($FilePath) + ($Arguments | ForEach-Object { Format-CommandArgument $_ })) -join " "
    if ($DryRun) {
        Write-Step "DRY-RUN: $display"
        return
    }

    Push-Location $WorkingDirectory
    try {
        & $FilePath @Arguments
        if ($LASTEXITCODE -ne 0) {
            throw "命令失败（exit=$LASTEXITCODE）：$display"
        }
    }
    finally {
        Pop-Location
    }
}

function Get-DevelopmentConfig {
    $defaults = [ordered]@{
        repo = $script:ActualRepo
        piAgentDir = "C:/Users/Admin/.pi/agent"
        uproject = "E:/Master/LuaSocial.uproject"
        installScope = "project"
        uePluginTarget = $null
        engineRoot = "E:/UE_4.27"
        editorTarget = "LuaSocialEditor"
        runUeTests = $false
        ueTestFilter = "PiUnrealBlueprint"
    }

    $configPath = if ($Config) {
        ConvertTo-NormalizedPath $Config
    }
    else {
        Join-Path $script:ActualRepo "dev.local.json"
    }

    if (Test-Path -LiteralPath $configPath -PathType Leaf) {
        $raw = Get-Content -LiteralPath $configPath -Raw -Encoding UTF8 | ConvertFrom-Json
        foreach ($property in $raw.PSObject.Properties) {
            $defaults[$property.Name] = $property.Value
        }
        Write-Step "配置：$($configPath.Replace('\', '/'))"
    }
    else {
        Write-Step "配置文件不存在，使用本机默认值：$($configPath.Replace('\', '/'))"
    }

    if ($Scope) { $defaults.installScope = $Scope }
    if ($UProject) { $defaults.uproject = $UProject }
    if ($EngineRoot) { $defaults.engineRoot = $EngineRoot }
    if ($PluginTarget) { $defaults.uePluginTarget = $PluginTarget }
    if ($PiAgentDir) { $defaults.piAgentDir = $PiAgentDir }

    foreach ($name in @("repo", "piAgentDir", "uproject", "engineRoot")) {
        if (-not ($defaults[$name] -is [string]) -or [string]::IsNullOrWhiteSpace($defaults[$name])) {
            throw "配置项 $name 必须是非空路径。"
        }
        $defaults[$name] = ConvertTo-NormalizedPath $defaults[$name]
    }
    if ($defaults.installScope -notin @("project", "engine")) {
        throw "installScope 必须是 project 或 engine。"
    }
    if ($defaults.uePluginTarget) {
        $defaults.uePluginTarget = ConvertTo-NormalizedPath ([string]$defaults.uePluginTarget)
    }
    elseif ($defaults.installScope -eq "engine") {
        $defaults.uePluginTarget = ConvertTo-NormalizedPath (Join-Path $defaults.engineRoot "Engine/Plugins/Developer/PiUnrealBlueprint")
    }
    else {
        $defaults.uePluginTarget = ConvertTo-NormalizedPath (Join-Path (Split-Path -Parent $defaults.uproject) "Plugins/PiUnrealBlueprint")
    }

    if ((Split-Path -Leaf $defaults.uePluginTarget) -ne "PiUnrealBlueprint") {
        throw "uePluginTarget 必须是名为 PiUnrealBlueprint 的独立插件目录：$($defaults.uePluginTarget)"
    }
    if ([System.IO.Path]::GetExtension($defaults.uproject) -ne ".uproject") {
        throw "uproject 必须指向 .uproject 文件：$($defaults.uproject)"
    }

    $configuredRepo = ConvertTo-NormalizedPath $defaults.repo
    $actualRepo = ConvertTo-NormalizedPath $script:ActualRepo
    if (-not [string]::Equals($configuredRepo, $actualRepo, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "配置 repo 与脚本所在仓库不一致：配置=$configuredRepo，实际=$actualRepo"
    }

    return [pscustomobject]$defaults
}

function Assert-CommandAvailable {
    param([Parameter(Mandatory = $true)][string]$Name)
    if ($DryRun) {
        Write-Step "DRY-RUN: 检查命令 $Name"
        return
    }
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "缺少命令：$Name"
    }
}

function Assert-NodeVersion {
    if ($DryRun) {
        Write-Step "DRY-RUN: 检查 Node.js >= 22.19.0"
        return
    }
    Assert-CommandAvailable "node"
    $rawVersion = (& node --version).Trim().TrimStart("v")
    if ($LASTEXITCODE -ne 0 -or ([version]$rawVersion -lt [version]"22.19.0")) {
        throw "Node.js 版本必须 >= 22.19.0，当前为 $rawVersion"
    }
}

function Assert-DevelopmentPrerequisites {
    param([Parameter(Mandatory = $true)]$Settings, [switch]$IncludePiAndVisualStudio)

    Assert-NodeVersion
    Assert-CommandAvailable "npm"

    if ($IncludePiAndVisualStudio) {
        Assert-CommandAvailable "pi"
        if ($DryRun) {
            Write-Step "DRY-RUN: 检查 Visual Studio C++ 工具链"
        }
        else {
            $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio/Installer/vswhere.exe"
            if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
                throw "未找到 vswhere.exe，无法确认 Visual Studio C++ 工具链。"
            }
            $installation = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
            if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace(($installation -join ""))) {
                throw "未找到带 C++ 工具链的 Visual Studio。"
            }
        }
    }

    $requiredPaths = @(
        $Settings.uproject,
        (Join-Path $Settings.engineRoot "Engine/Build/BatchFiles/RunUAT.bat"),
        (Join-Path $Settings.engineRoot "Engine/Binaries/Win64/UE4Editor.exe"),
        $script:PluginSource
    )
    foreach ($path in $requiredPaths) {
        $normalized = $path.Replace("\", "/")
        if ($DryRun) {
            Write-Step "DRY-RUN: 检查路径 $normalized"
        }
        elseif (-not (Test-Path -LiteralPath $path)) {
            throw "缺少前置路径：$normalized"
        }
    }
}

function Get-PluginFiles {
    $excludedRoots = @("Binaries", "Intermediate", "Saved", "DerivedDataCache")
    $sourceRoot = ConvertTo-NormalizedPath $script:PluginSource
    return @(Get-ChildItem -LiteralPath $script:PluginSource -File -Recurse | Where-Object {
        $relative = (ConvertTo-NormalizedPath $_.FullName).Substring($sourceRoot.Length).TrimStart("/")
        $first = ($relative -split "/", 2)[0]
        $excludedRoots -notcontains $first
    } | ForEach-Object {
        $full = ConvertTo-NormalizedPath $_.FullName
        [pscustomobject]@{
            path = $full.Substring($sourceRoot.Length).TrimStart("/")
            source = $full
            sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    } | Sort-Object path)
}

function Read-ManagedManifest {
    param([Parameter(Mandatory = $true)][string]$Target)
    $manifestPath = Join-Path $Target $script:ManifestName
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        return @()
    }

    try {
        $manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    }
    catch {
        throw "受管 manifest 无法解析，拒绝覆盖目标：$($manifestPath.Replace('\', '/'))。$($_.Exception.Message)"
    }
    if ($null -eq $manifest.files) {
        throw "受管 manifest 缺少 files，拒绝覆盖目标：$($manifestPath.Replace('\', '/'))"
    }
    return @($manifest.files)
}

function Resolve-SafeManagedPath {
    param([Parameter(Mandatory = $true)][string]$Target, [Parameter(Mandatory = $true)][string]$RelativePath)
    if ([string]::IsNullOrWhiteSpace($RelativePath) -or [System.IO.Path]::IsPathRooted($RelativePath) -or $RelativePath -match "(^|/|\\)\.\.($|/|\\)") {
        throw "manifest 含不安全路径：$RelativePath"
    }
    $targetRoot = (ConvertTo-NormalizedPath $Target) + "/"
    $resolved = ConvertTo-NormalizedPath (Join-Path $Target $RelativePath)
    if (-not $resolved.StartsWith($targetRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "manifest 路径越过插件目录：$RelativePath"
    }
    return $resolved
}

function Get-SyncPlan {
    param([Parameter(Mandatory = $true)]$Settings)
    $files = Get-PluginFiles
    $currentByPath = @{}
    foreach ($file in $files) { $currentByPath[$file.path] = $file }
    $oldFiles = Read-ManagedManifest $Settings.uePluginTarget
    $oldByPath = @{}

    $remove = @()
    foreach ($old in $oldFiles) {
        $relative = [string]$old.path
        [void](Resolve-SafeManagedPath $Settings.uePluginTarget $relative)
        $oldByPath[$relative] = $old
        if (-not $currentByPath.ContainsKey($relative)) { $remove += $relative }
    }

    $copy = @()
    $conflicts = @()
    foreach ($file in $files) {
        $destination = Resolve-SafeManagedPath $Settings.uePluginTarget $file.path
        $same = $false
        if (Test-Path -LiteralPath $destination -PathType Leaf) {
            $same = ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant() -eq $file.sha256)
            if (-not $same -and -not $oldByPath.ContainsKey($file.path)) {
                $conflicts += $file.path
                continue
            }
        }
        if (-not $same) { $copy += $file }
    }

    return [pscustomobject]@{
        files = $files
        copy = $copy
        remove = $remove
        conflicts = $conflicts
        changed = (($copy.Count + $remove.Count) -gt 0)
    }
}

function Assert-NoUnmanagedConflicts {
    param([Parameter(Mandatory = $true)]$SyncPlan)
    if ($SyncPlan.conflicts.Count -gt 0) {
        throw "目标中存在未由上次 manifest 管理的同名文件，拒绝覆盖：$($SyncPlan.conflicts -join ', ')"
    }
}

function Test-TargetEditorRunning {
    param([Parameter(Mandatory = $true)]$Settings)
    if ($DryRun) { return $false }

    $projectName = [System.IO.Path]::GetFileName($Settings.uproject)
    $running = @(Get-CimInstance Win32_Process -Filter "Name = 'UE4Editor.exe'" | Where-Object {
        $_.CommandLine -and ($_.CommandLine.Replace("\", "/") -like "*$projectName*")
    })
    return $running.Count -gt 0
}

function Assert-EditorSafeForPluginUpdate {
    param([Parameter(Mandatory = $true)]$Settings, [Parameter(Mandatory = $true)]$SyncPlan)
    if (-not $SyncPlan.changed) { return }
    if ($DryRun) {
        Write-Step "DRY-RUN: 检查目标 UE Editor 未运行（仅在覆盖插件前停止）"
        return
    }

    if (Test-TargetEditorRunning $Settings) {
        $projectName = [System.IO.Path]::GetFileName($Settings.uproject)
        throw "检测到 $projectName 的 UE Editor 正在运行，且插件文件需要更新。已在覆盖前停止；请关闭 Editor 后重跑 sync。"
    }
}

function Invoke-UnrealBuild {
    param([Parameter(Mandatory = $true)]$Settings)
    if ($SkipUnrealBuild) {
        Write-Step "已按参数跳过 UE C++ 构建。"
        return
    }

    # 在独立 staging 目录构建插件，不写入正在运行的项目或其已加载 DLL。
    $runUat = Join-Path $Settings.engineRoot "Engine/Build/BatchFiles/RunUAT.bat"
    $packageRoot = Join-Path $script:ActualRepo "artifacts/dev-build"
    if (-not $DryRun -and (Test-Path -LiteralPath $packageRoot)) {
        Remove-Item -LiteralPath $packageRoot -Recurse -Force
    }
    Invoke-CheckedCommand $runUat @(
        "BuildPlugin",
        "-Plugin=$($script:PluginSource)/PiUnrealBlueprint.uplugin",
        "-Package=$($packageRoot.Replace('\', '/'))",
        "-TargetPlatforms=Win64",
        "-Rocket"
    )
}

function Invoke-UnrealTests {
    param([Parameter(Mandatory = $true)]$Settings)
    $shouldRun = $RunUnrealTests -or [bool]$Settings.runUeTests
    if (-not $shouldRun) {
        Write-Step "UE Automation 测试未启用（可用 -RunUnrealTests 开启）。"
        return
    }
    $editor = Join-Path $Settings.engineRoot "Engine/Binaries/Win64/UE4Editor-Cmd.exe"
    if (-not $DryRun -and -not (Test-Path -LiteralPath $editor -PathType Leaf)) {
        throw "未找到 UE4Editor-Cmd.exe：$($editor.Replace('\', '/'))"
    }
    Invoke-CheckedCommand $editor @(
        $Settings.uproject,
        "-unattended",
        "-nop4",
        "-NullRHI",
        "-ExecCmds=Automation RunTests $($Settings.ueTestFilter);Quit",
        "-TestExit=Automation Test Queue Empty"
    )
}

function Invoke-Check {
    param([Parameter(Mandatory = $true)]$Settings)
    Assert-DevelopmentPrerequisites $Settings
    if ($SkipPackageCheck) {
        Write-Step "使用已发布 package 的预构建 CLI，跳过仅源码 checkout 可用的 npm 开发检查。"
    }
    else {
        Invoke-CheckedCommand "npm" @("run", "check")
    }
    Invoke-UnrealBuild $Settings
    Invoke-UnrealTests $Settings
}

function Write-Manifest {
    param([Parameter(Mandatory = $true)]$Settings, [Parameter(Mandatory = $true)]$SyncPlan)
    $manifest = [ordered]@{
        version = 1
        source = (ConvertTo-NormalizedPath $script:PluginSource)
        generatedAtUtc = [DateTime]::UtcNow.ToString("o")
        files = @($SyncPlan.files | ForEach-Object { [ordered]@{ path = $_.path; sha256 = $_.sha256 } })
    }
    $json = $manifest | ConvertTo-Json -Depth 5
    $path = Join-Path $Settings.uePluginTarget $script:ManifestName
    [System.IO.File]::WriteAllText($path, $json + "`n", [System.Text.UTF8Encoding]::new($false))
}

function Invoke-SafePluginMirror {
    param([Parameter(Mandatory = $true)]$Settings, [Parameter(Mandatory = $true)]$SyncPlan)
    Write-Step "插件镜像计划：复制 $($SyncPlan.copy.Count)，删除 $($SyncPlan.remove.Count)，保留所有非受管文件。"
    if ($DryRun) {
        foreach ($path in $SyncPlan.remove) { Write-Step "DRY-RUN: 删除上次 manifest 记录的文件 $path" }
        foreach ($file in $SyncPlan.copy) { Write-Step "DRY-RUN: 复制 $($file.path)" }
        Write-Step "DRY-RUN: 写入 UTF-8 manifest $($Settings.uePluginTarget)/$script:ManifestName"
        return
    }

    [void](New-Item -ItemType Directory -Path $Settings.uePluginTarget -Force)
    foreach ($relative in $SyncPlan.remove) {
        $path = Resolve-SafeManagedPath $Settings.uePluginTarget $relative
        if (Test-Path -LiteralPath $path -PathType Leaf) { Remove-Item -LiteralPath $path -Force }
    }
    foreach ($file in $SyncPlan.copy) {
        $destination = Resolve-SafeManagedPath $Settings.uePluginTarget $file.path
        [void](New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force)
        Copy-Item -LiteralPath $file.source -Destination $destination -Force
        $copiedHash = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($copiedHash -ne $file.sha256) {
            throw "插件文件复制后校验失败：$($file.path)"
        }
    }
    Write-Manifest $Settings $SyncPlan

    $verification = Get-SyncPlan $Settings
    if ($verification.changed) {
        throw "插件镜像完成后清单校验仍有差异。"
    }
}

function Assert-PublishMessage {
    param([string]$Value)
    if ([string]::IsNullOrWhiteSpace($Value)) {
        throw "publish 必须提供 -Message。"
    }
    if ($Value.Length -gt 72) {
        throw "提交消息不能超过 72 个字符，当前为 $($Value.Length)。"
    }
    if ($Value -notmatch "^(feat|fix|refactor|perf|style|docs|test|chore)(\([A-Za-z0-9._/-]+\))?: .*[\u4e00-\u9fff].*$") {
        throw "提交消息必须为 type(scope): 中文描述；scope 可省略。"
    }
}

function Assert-PublishGitPreflight {
    if ($DryRun) {
        Write-Step "DRY-RUN: 检查当前分支、origin 与远端连通性"
        return "<current-branch>"
    }

    $inside = (& git -C $script:ActualRepo rev-parse --is-inside-work-tree).Trim()
    if ($LASTEXITCODE -ne 0 -or $inside -ne "true") { throw "repo 不是有效 Git 工作树。" }

    $branch = (& git -C $script:ActualRepo branch --show-current).Trim()
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($branch)) {
        throw "publish 只能在已命名的当前分支执行。"
    }

    $origin = (& git -C $script:ActualRepo remote get-url origin).Trim()
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($origin)) { throw "publish 需要已配置的 origin。" }
    Invoke-CheckedCommand "git" @("ls-remote", "--exit-code", "origin", "HEAD") | Out-Null
    return $branch
}

function Invoke-Publish {
    param([Parameter(Mandatory = $true)]$Settings)
    Assert-PublishMessage $Message
    Assert-CommandAvailable "git"

    # 云端发布只验证仓库源码；UE 是否运行不能阻止 Git 提交和推送。
    $branch = Assert-PublishGitPreflight
    Invoke-Check $Settings

    if ($DryRun) {
        Write-Step "DRY-RUN: 展示 git status --short"
    }
    else {
        $summary = & git -C $script:ActualRepo status --short
        if ($LASTEXITCODE -ne 0) { throw "无法读取 git 变更摘要。" }
        if ([string]::IsNullOrWhiteSpace(($summary -join "`n"))) { throw "没有可发布的变更。" }
        Write-Step "待提交变更："
        $summary | ForEach-Object { Write-Host $_ }
    }

    $oldPrompt = $env:GIT_TERMINAL_PROMPT
    $oldEditor = $env:GIT_EDITOR
    try {
        $env:GIT_TERMINAL_PROMPT = "0"
        $env:GIT_EDITOR = "true"
        Invoke-CheckedCommand "git" @("add", "--all")
        Invoke-CheckedCommand "git" @("-c", "core.editor=true", "commit", "-m", $Message)
        Invoke-CheckedCommand "git" @("push", "origin", $branch)
    }
    finally {
        $env:GIT_TERMINAL_PROMPT = $oldPrompt
        $env:GIT_EDITOR = $oldEditor
    }

    # 推送已经完成；Editor 运行中只延后本机同步，不回滚或掩盖云端结果。
    $syncPlan = Get-SyncPlan $Settings
    Assert-NoUnmanagedConflicts $syncPlan
    if ($syncPlan.changed -and (Test-TargetEditorRunning $Settings)) {
        Write-Step "Git 提交和云端推送已完成；目标 UE Editor 正在运行，本机插件同步已延后。关闭 Editor 后运行 dev.ps1 sync。"
        return
    }
    Assert-EditorSafeForPluginUpdate $Settings $syncPlan
    Invoke-SafePluginMirror $Settings $syncPlan
}

$settings = Get-DevelopmentConfig
switch ($Action) {
    "setup" {
        $setupParameters = @{
            Config = $(if ($Config) { ConvertTo-NormalizedPath $Config } else { Join-Path $script:ActualRepo "dev.local.json" })
            Scope = $Scope
            UProject = $UProject
            EngineRoot = $EngineRoot
            PluginTarget = $PluginTarget
            PiAgentDir = $PiAgentDir
            DryRun = $DryRun
            SkipUnrealBuild = $SkipUnrealBuild
        }
        & (Join-Path $script:ScriptDir "setup.ps1") @setupParameters
        if (-not $?) { throw "setup.ps1 执行失败。" }
    }
    "check" {
        Invoke-Check $settings
    }
    "sync" {
        # 插件有变化时，必须在构建和覆盖之前确认 Editor 已停止。
        $syncPlan = Get-SyncPlan $settings
        Assert-NoUnmanagedConflicts $syncPlan
        Assert-EditorSafeForPluginUpdate $settings $syncPlan
        Invoke-Check $settings

        # 构建可能耗时，覆盖目标前重新读取并再次确认 Editor 状态。
        $syncPlan = Get-SyncPlan $settings
        Assert-NoUnmanagedConflicts $syncPlan
        Assert-EditorSafeForPluginUpdate $settings $syncPlan
        Invoke-SafePluginMirror $settings $syncPlan
    }
    "publish" {
        Invoke-Publish $settings
    }
}
