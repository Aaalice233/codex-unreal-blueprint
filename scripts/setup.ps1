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
        $items = if (Test-Path -LiteralPath $path -PathType Leaf) { @(Get-Item -LiteralPath $path) } else { @(Get-ChildItem -LiteralPath $path -File -Recurse | Where-Object { $_.FullName -notmatch "[\\/](Intermediate|Saved|DerivedDataCache)[\\/]" }) }
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
    $canAdoptGeneratedBinaries = $oldByPath.ContainsKey("CodexUnrealBlueprint.uplugin")
    foreach ($file in $Files) {
        $destination = Resolve-ManagedPath $Target $file.path
        if ((Test-Path -LiteralPath $destination -PathType Leaf) -and -not $oldByPath.ContainsKey($file.path)) {
            $currentHash = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
            $isGeneratedPluginBinary = $canAdoptGeneratedBinaries -and $file.path.StartsWith("Binaries/", [System.StringComparison]::OrdinalIgnoreCase)
            if ($currentHash -ne $file.sha256 -and -not $isGeneratedPluginBinary) { throw "目标存在非受管同名文件，拒绝覆盖：$destination" }
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
        if (-not [string]::Equals((Normalize-Path $file.source), $destination, [System.StringComparison]::OrdinalIgnoreCase)) {
            Copy-Item -LiteralPath $file.source -Destination $destination -Force
        }
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

function New-CodexPluginInstallStage([object[]]$Files) {
    if ($DryRun) { return $script:Repo }
    $stage = Normalize-Path "$script:Repo/artifacts/codex-plugin-install"
    $artifactsRoot = (Normalize-Path "$script:Repo/artifacts") + "/"
    if (-not $stage.StartsWith($artifactsRoot, [System.StringComparison]::OrdinalIgnoreCase)) { throw "Codex plugin 暂存目录不安全：$stage" }
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
    [void](New-Item -ItemType Directory -Path $stage -Force)
    foreach ($file in $Files) {
        $destination = Resolve-ManagedPath $stage $file.path
        [void](New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force)
        Copy-Item -LiteralPath $file.source -Destination $destination -Force
    }

    $manifestPath = "$stage/.codex-plugin/plugin.json"
    $manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if (-not ($manifest.version -is [string]) -or [string]::IsNullOrWhiteSpace($manifest.version)) { throw "Codex plugin manifest 缺少 version。" }
    $baseVersion = ([string]$manifest.version -split "\+", 2)[0]
    $cachebuster = [DateTime]::UtcNow.ToString("yyyyMMddHHmmssfff")
    $manifest.version = "$baseVersion+codex.$cachebuster"
    [System.IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 10) + [Environment]::NewLine, [System.Text.UTF8Encoding]::new($false))
    return $stage
}

function Update-PersonalMarketplace($Settings) {
    $path = $Settings.marketplacePath
    if ($DryRun) { Write-Step "DRY-RUN: 保留现有条目并更新个人 Marketplace：$path"; return "personal" }
    if (Test-Path -LiteralPath $path -PathType Leaf) { $marketplace = Get-Content -LiteralPath $path -Raw -Encoding UTF8 | ConvertFrom-Json }
    else { $marketplace = [pscustomobject]@{ name = "personal"; interface = [pscustomobject]@{ displayName = "Personal" }; plugins = @() } }
    if (-not ($marketplace.name -is [string]) -or [string]::IsNullOrWhiteSpace($marketplace.name)) { throw "Marketplace 缺少有效 name。" }
    $matching = @($marketplace.plugins | Where-Object { $_.name -eq "codex-unreal-blueprint" })
    if ($matching.Count -gt 1) { throw "Marketplace 中存在重复的 codex-unreal-blueprint 条目。" }
    if ($matching.Count -eq 1) {
        $entry = $matching[0]
        if ($entry.source.source -ne "local" -or $entry.source.path -ne "./plugins/codex-unreal-blueprint") {
            throw "Marketplace 中现有 codex-unreal-blueprint 条目没有指向受管的本地插件目录。"
        }
        return [string]$marketplace.name
    }
    $marketplace.plugins = @($marketplace.plugins) + [pscustomobject]@{ name = "codex-unreal-blueprint"; source = [pscustomobject]@{ source = "local"; path = "./plugins/codex-unreal-blueprint" }; policy = [pscustomobject]@{ installation = "AVAILABLE"; authentication = "ON_INSTALL" }; category = "Developer Tools" }
    [void](New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force)
    [System.IO.File]::WriteAllText($path, ($marketplace | ConvertTo-Json -Depth 8) + "`n", [System.Text.UTF8Encoding]::new($false))
    return [string]$marketplace.name
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
if (-not $SkipUnrealBuild) {
    # Install the exact DLLs produced by the validated BuildPlugin run. Copying only source would leave an
    # older binary active until the project happened to rebuild the plugin itself.
    $ueFiles += Get-SourceFiles $packageRoot @("Binaries")
    $ueFiles = @($ueFiles | Sort-Object path -Unique)
}
elseif (Test-Path -LiteralPath "$($settings.uePluginTarget)/Binaries" -PathType Container) {
    # A source-only maintenance run must not make previously installed, managed DLLs disappear.
    $ueFiles += Get-SourceFiles $settings.uePluginTarget @("Binaries")
    $ueFiles = @($ueFiles | Sort-Object path -Unique)
}
Sync-ManagedDirectory $settings.uePluginTarget $ueFiles
$codexIncludes = @(".codex-plugin", ".mcp.json", "dist/mcp/index.js", "skills", "LICENSE", "README.md", "README.zh-CN.md")
$codexSourceFiles = Get-SourceFiles $script:Repo $codexIncludes
$codexStage = New-CodexPluginInstallStage $codexSourceFiles
$codexFiles = Get-SourceFiles $codexStage $codexIncludes
Sync-ManagedDirectory $settings.codexPluginTarget $codexFiles
$marketplaceName = Update-PersonalMarketplace $settings
Invoke-Checked $settings.codexExecutable @("plugin", "add", "codex-unreal-blueprint@$marketplaceName")
Write-Step "安装完成。请重启 Unreal Editor，并新建 Codex task 以加载 Skill 和九个 MCP tools。"
