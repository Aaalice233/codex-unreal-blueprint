[CmdletBinding()]
param(
    [string]$Config,
    [ValidateSet("project", "engine")][string]$Scope,
    [string]$UProject,
    [string]$EngineRoot,
    [string]$PluginTarget,
    [string]$CodexExecutable,
    [string]$CodexPluginTarget,
    [string]$MarketplacePath,
    [switch]$DryRun,
    [switch]$SkipUnrealBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)

$script:Repo = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..")).Replace("\", "/").TrimEnd("/")
$script:ManagedManifest = ".codex-unreal-blueprint.manifest.json"

function Normalize-Path([string]$Path) { [System.IO.Path]::GetFullPath($Path).Replace("\", "/").TrimEnd("/") }
function Write-Step([string]$Text) { Write-Host "[codex-unreal-blueprint setup] $Text" }
function Invoke-Checked([string]$FilePath, [string[]]$Arguments, [string]$WorkingDirectory = $script:Repo) {
    if ($DryRun) { Write-Step "DRY-RUN: $FilePath $($Arguments -join ' ')"; return }
    Push-Location $WorkingDirectory
    try { & $FilePath @Arguments; if ($LASTEXITCODE -ne 0) { throw "命令失败（exit=$LASTEXITCODE）：$FilePath $($Arguments -join ' ')" } }
    finally { Pop-Location }
}

function Get-Settings {
    $values = [ordered]@{
        repo = $script:Repo
        uproject = "E:/Master/LuaSocial.uproject"
        engineRoot = "E:/UE_4.27"
        installScope = "project"
        uePluginTarget = $null
        codexExecutable = $null
        codexPluginTarget = (Join-Path $env:USERPROFILE "plugins/codex-unreal-blueprint")
        marketplacePath = (Join-Path $env:USERPROFILE ".agents/plugins/marketplace.json")
    }
    $configPath = if ($Config) { Normalize-Path $Config } else { "$script:Repo/dev.local.json" }
    if (Test-Path -LiteralPath $configPath -PathType Leaf) {
        $local = Get-Content -LiteralPath $configPath -Raw -Encoding UTF8 | ConvertFrom-Json
        foreach ($property in $local.PSObject.Properties) { $values[$property.Name] = $property.Value }
    }
    if ($Scope) { $values.installScope = $Scope }
    if ($UProject) { $values.uproject = $UProject }
    if ($EngineRoot) { $values.engineRoot = $EngineRoot }
    if ($PluginTarget) { $values.uePluginTarget = $PluginTarget }
    if ($CodexExecutable) { $values.codexExecutable = $CodexExecutable }
    if ($CodexPluginTarget) { $values.codexPluginTarget = $CodexPluginTarget }
    if ($MarketplacePath) { $values.marketplacePath = $MarketplacePath }
    foreach ($name in @("repo", "uproject", "engineRoot", "codexPluginTarget", "marketplacePath")) {
        if (-not ($values[$name] -is [string]) -or [string]::IsNullOrWhiteSpace($values[$name])) { throw "缺少配置项 $name。" }
        $values[$name] = Normalize-Path $values[$name]
    }
    if (-not [string]::Equals($values.repo, $script:Repo, [System.StringComparison]::OrdinalIgnoreCase)) { throw "配置 repo 与脚本仓库不一致。" }
    if ($values.installScope -notin @("project", "engine")) { throw "installScope 必须是 project 或 engine。" }
    if ($values.uePluginTarget) { $values.uePluginTarget = Normalize-Path ([string]$values.uePluginTarget) }
    elseif ($values.installScope -eq "engine") { $values.uePluginTarget = "$($values.engineRoot)/Engine/Plugins/Developer/CodexUnrealBlueprint" }
    else { $values.uePluginTarget = Normalize-Path (Join-Path (Split-Path -Parent $values.uproject) "Plugins/CodexUnrealBlueprint") }
    if ((Split-Path -Leaf $values.uePluginTarget) -ne "CodexUnrealBlueprint") { throw "uePluginTarget 必须以 CodexUnrealBlueprint 结尾。" }
    return [pscustomobject]$values
}

function Resolve-CodexExecutable($Settings) {
    $candidates = @()
    if ($Settings.codexExecutable) { $candidates += [string]$Settings.codexExecutable }
    $candidates += @(Get-Command codex -All -ErrorAction SilentlyContinue | ForEach-Object Source)
    $candidates += @(Get-ChildItem -Path "$env:LOCALAPPDATA/OpenAI/Codex/bin/*/codex.exe" -File -ErrorAction SilentlyContinue | Sort-Object LastWriteTimeUtc -Descending | ForEach-Object FullName)
    foreach ($candidate in @($candidates | Select-Object -Unique)) {
        try {
            if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) { continue }
            if ($DryRun) { return Normalize-Path $candidate }
            $output = & $candidate --version 2>&1
            if ($LASTEXITCODE -eq 0 -and ($output -join "") -match "codex") { return Normalize-Path $candidate }
        } catch { continue }
    }
    throw "未找到可实际执行的 Codex CLI。可通过 -CodexExecutable 指定桌面版 codex.exe。"
}

function Assert-Prerequisites($Settings) {
    foreach ($command in @("node", "npm")) { if (-not (Get-Command $command -ErrorAction SilentlyContinue)) { throw "缺少命令：$command" } }
    if (-not $DryRun) {
        $nodeVersion = (& node --version).Trim().TrimStart("v")
        if ($LASTEXITCODE -ne 0 -or [version]$nodeVersion -lt [version]"22.19.0") { throw "Node.js 必须 >= 22.19.0。" }
    }
    $Settings.codexExecutable = Resolve-CodexExecutable $Settings
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio/Installer/vswhere.exe"
    if (-not $DryRun) {
        if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) { throw "未找到 vswhere.exe。" }
        $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace(($vs -join ""))) { throw "未找到 Visual Studio C++ 工具链。" }
    }
    foreach ($path in @($Settings.uproject, "$($Settings.engineRoot)/Engine/Build/BatchFiles/RunUAT.bat", "$($Settings.engineRoot)/Engine/Binaries/Win64/UE4Editor.exe")) {
        if (-not $DryRun -and -not (Test-Path -LiteralPath $path)) { throw "缺少前置路径：$path" }
    }
}

function Get-SourceFiles([string]$Source, [string[]]$Includes) {
    $sourceRoot = (Normalize-Path $Source) + "/"
    $result = @()
    foreach ($include in $Includes) {
        $path = Join-Path $Source $include
        if (-not (Test-Path -LiteralPath $path)) { throw "安装源缺少：$include" }
        $items = if (Test-Path -LiteralPath $path -PathType Leaf) { @(Get-Item -LiteralPath $path) } else { @(Get-ChildItem -LiteralPath $path -File -Recurse | Where-Object { $_.FullName -notmatch "[\\/](Binaries|Intermediate|Saved|DerivedDataCache)[\\/]" }) }
        foreach ($item in $items) {
            $full = Normalize-Path $item.FullName
            $result += [pscustomobject]@{ path = $full.Substring($sourceRoot.Length); source = $full; sha256 = (Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash.ToLowerInvariant() }
        }
    }
    return @($result | Sort-Object path -Unique)
}

function Resolve-ManagedPath([string]$Target, [string]$Relative) {
    if ([string]::IsNullOrWhiteSpace($Relative) -or [System.IO.Path]::IsPathRooted($Relative) -or $Relative -match "(^|/|\\)\.\.($|/|\\)") { throw "不安全的受管路径：$Relative" }
    $root = (Normalize-Path $Target) + "/"
    $resolved = Normalize-Path (Join-Path $Target $Relative)
    if (-not $resolved.StartsWith($root, [System.StringComparison]::OrdinalIgnoreCase)) { throw "受管路径越过目标目录：$Relative" }
    return $resolved
}

function Sync-ManagedDirectory([string]$Target, [object[]]$Files) {
    $manifestPath = Join-Path $Target $script:ManagedManifest
    $oldFiles = @()
    if (Test-Path -LiteralPath $manifestPath -PathType Leaf) {
        try { $oldFiles = @((Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json).files) }
        catch { throw "受管 manifest 损坏，拒绝覆盖：$manifestPath" }
    }
    $newByPath = @{}; foreach ($file in $Files) { $newByPath[$file.path] = $file }
    $oldByPath = @{}; foreach ($file in $oldFiles) { [void](Resolve-ManagedPath $Target ([string]$file.path)); $oldByPath[[string]$file.path] = $file }
    foreach ($file in $Files) {
        $destination = Resolve-ManagedPath $Target $file.path
        if ((Test-Path -LiteralPath $destination -PathType Leaf) -and -not $oldByPath.ContainsKey($file.path)) {
            $currentHash = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($currentHash -ne $file.sha256) { throw "目标存在非受管同名文件，拒绝覆盖：$destination" }
        }
    }
    if ($DryRun) { Write-Step "DRY-RUN: 同步 $($Files.Count) 个受管文件到 $Target，并保留非受管文件"; return }
    [void](New-Item -ItemType Directory -Path $Target -Force)
    foreach ($old in $oldFiles) {
        if (-not $newByPath.ContainsKey([string]$old.path)) { $path = Resolve-ManagedPath $Target ([string]$old.path); if (Test-Path -LiteralPath $path -PathType Leaf) { Remove-Item -LiteralPath $path -Force } }
    }
    foreach ($file in $Files) {
        $destination = Resolve-ManagedPath $Target $file.path
        [void](New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force)
        Copy-Item -LiteralPath $file.source -Destination $destination -Force
        if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant() -ne $file.sha256) { throw "复制后哈希校验失败：$($file.path)" }
    }
    $manifest = [ordered]@{ version = 1; generatedAtUtc = [DateTime]::UtcNow.ToString("o"); files = @($Files | ForEach-Object { [ordered]@{ path = $_.path; sha256 = $_.sha256 } }) }
    [System.IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 5) + "`n", [System.Text.UTF8Encoding]::new($false))
}

function Assert-EditorClosed($Settings) {
    if ($DryRun) { return }
    $projectName = [System.IO.Path]::GetFileName($Settings.uproject)
    $running = @(Get-CimInstance Win32_Process -Filter "Name = 'UE4Editor.exe'" | Where-Object { $_.CommandLine -and $_.CommandLine.Replace("\", "/") -like "*$projectName*" })
    if ($running.Count -gt 0) { throw "检测到 $projectName 的 Editor 正在加载插件。未覆盖任何 UE 文件；请关闭 Editor 后重试。" }
}

function Update-PersonalMarketplace($Settings) {
    $path = $Settings.marketplacePath
    if ($DryRun) { Write-Step "DRY-RUN: 保留现有条目并更新个人 Marketplace：$path"; return "personal" }
    if (Test-Path -LiteralPath $path -PathType Leaf) { $marketplace = Get-Content -LiteralPath $path -Raw -Encoding UTF8 | ConvertFrom-Json }
    else { $marketplace = [pscustomobject]@{ name = "personal"; interface = [pscustomobject]@{ displayName = "Personal" }; plugins = @() } }
    if (-not ($marketplace.name -is [string]) -or [string]::IsNullOrWhiteSpace($marketplace.name)) { throw "Marketplace 缺少有效 name。" }
    $entries = @($marketplace.plugins | Where-Object { $_.name -ne "codex-unreal-blueprint" })
    $entries += [pscustomobject]@{ name = "codex-unreal-blueprint"; source = [pscustomobject]@{ source = "local"; path = "./plugins/codex-unreal-blueprint" }; policy = [pscustomobject]@{ installation = "AVAILABLE"; authentication = "ON_INSTALL" }; category = "Developer Tools" }
    $marketplace.plugins = $entries
    [void](New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force)
    [System.IO.File]::WriteAllText($path, ($marketplace | ConvertTo-Json -Depth 8) + "`n", [System.Text.UTF8Encoding]::new($false))
    return [string]$marketplace.name
}

function Test-CodexCacheCurrent([object[]]$Files, [string]$MarketplaceName) {
    if ($DryRun) { return $false }
    $version = [string](Get-Content -LiteralPath "$script:Repo/package.json" -Raw -Encoding UTF8 | ConvertFrom-Json).version
    $cacheRoot = Normalize-Path (Join-Path $env:USERPROFILE ".codex/plugins/cache/$MarketplaceName/codex-unreal-blueprint/$version")
    if (-not (Test-Path -LiteralPath $cacheRoot -PathType Container)) { return $false }
    foreach ($file in $Files) {
        $cached = Resolve-ManagedPath $cacheRoot $file.path
        if (-not (Test-Path -LiteralPath $cached -PathType Leaf)) { return $false }
        if ((Get-FileHash -LiteralPath $cached -Algorithm SHA256).Hash.ToLowerInvariant() -ne $file.sha256) { return $false }
    }
    return $true
}

$settings = Get-Settings
Assert-Prerequisites $settings
Invoke-Checked "npm" @("run", "check")
if (-not $SkipUnrealBuild) {
    $packageRoot = "$script:Repo/artifacts/plugin-build"
    if (-not $DryRun -and (Test-Path -LiteralPath $packageRoot)) { Remove-Item -LiteralPath $packageRoot -Recurse -Force }
    Invoke-Checked "$($settings.engineRoot)/Engine/Build/BatchFiles/RunUAT.bat" @("BuildPlugin", "-Plugin=$script:Repo/unreal/CodexUnrealBlueprint/CodexUnrealBlueprint.uplugin", "-Package=$packageRoot", "-TargetPlatforms=Win64", "-Rocket")
}
Assert-EditorClosed $settings
$ueFiles = Get-SourceFiles "$script:Repo/unreal/CodexUnrealBlueprint" @("Config", "Source", "CodexUnrealBlueprint.uplugin", "README.md")
Sync-ManagedDirectory $settings.uePluginTarget $ueFiles
$codexFiles = Get-SourceFiles $script:Repo @(".codex-plugin", ".mcp.json", "dist/mcp/index.js", "skills", "LICENSE", "README.md", "README.zh-CN.md")
Sync-ManagedDirectory $settings.codexPluginTarget $codexFiles
$marketplaceName = Update-PersonalMarketplace $settings
if (Test-CodexCacheCurrent $codexFiles $marketplaceName) {
    Write-Step "Codex plugin cache 已是当前版本，无需在运行中的 Codex 内重复安装。"
}
else {
    Invoke-Checked $settings.codexExecutable @("plugin", "add", "codex-unreal-blueprint@$marketplaceName")
}
Write-Step "安装完成。请重启 Unreal Editor，并新建 Codex task 以加载 Skill 和九个 MCP tools。"
