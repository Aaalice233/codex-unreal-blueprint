[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9]+\.[0-9]+\.[0-9]+(?:[-+][0-9A-Za-z.-]+)?$')]
    [string]$Version,
    [switch]$AllowOpenGate
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$repo = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))

$package = Get-Content -LiteralPath "$repo/package.json" -Raw -Encoding UTF8 | ConvertFrom-Json
$lock = Get-Content -LiteralPath "$repo/package-lock.json" -Raw -Encoding UTF8 | ConvertFrom-Json -AsHashtable
$manifest = Get-Content -LiteralPath "$repo/.codex-plugin/plugin.json" -Raw -Encoding UTF8 | ConvertFrom-Json
$uplugin = Get-Content -LiteralPath "$repo/unreal/CodexUnrealBlueprint/CodexUnrealBlueprint.uplugin" -Raw -Encoding UTF8 | ConvertFrom-Json
$header = Get-Content -LiteralPath "$repo/unreal/CodexUnrealBlueprint/Source/CodexUnrealBlueprintCore/Public/CodexUnrealBlueprintProtocol.h" -Raw -Encoding UTF8
$protocolTs = Get-Content -LiteralPath "$repo/src/shared/protocol.ts" -Raw -Encoding UTF8

$versions = [ordered]@{
    "package.json" = [string]$package.version
    "package-lock.json" = [string]$lock["version"]
    "package-lock root" = [string]$lock["packages"][""]["version"]
    "plugin.json" = [string]$manifest.version
    ".uplugin" = [string]$uplugin.VersionName
}
foreach ($entry in $versions.GetEnumerator()) { if ($entry.Value -ne $Version) { throw "$($entry.Key) version $($entry.Value) does not match $Version." } }

$pluginVersion = [regex]::Match($header, 'PluginVersion\s*=\s*TEXT\("([^"]+)"\)')
$cppProtocol = [regex]::Match($header, 'ProtocolVersion\s*=\s*TEXT\("([^"]+)"\)')
$tsProtocol = [regex]::Match($protocolTs, 'CLIENT_PROTOCOL_VERSION\s*=\s*"([^"]+)"')
if (-not $pluginVersion.Success -or $pluginVersion.Groups[1].Value -ne $Version) { throw "C++ PluginVersion mismatch." }
if (-not $cppProtocol.Success -or -not $tsProtocol.Success -or $cppProtocol.Groups[1].Value -ne $tsProtocol.Groups[1].Value) { throw "TypeScript/C++ protocol mismatch." }

$required = @(
    ".codex-plugin/plugin.json", ".mcp.json", "dist/mcp/index.js", "skills/unreal-blueprint/SKILL.md",
    "offline/Inspect-UAsset.ps1", "offline/uasset-inspector/UnrealUAssetInspector.csproj", "THIRD_PARTY_NOTICES.md",
    "src/mcp/index.ts", "tests/ts/mcp.test.ts", "scripts/setup.ps1",
    "unreal/CodexUnrealBlueprint/CodexUnrealBlueprint.uplugin", "Docs/mcp-reference.md", "Docs/mcp-reference.zh-CN.md"
)
$missing = @($required | Where-Object { -not (Test-Path -LiteralPath "$repo/$_" -PathType Leaf) })
if ($missing.Count -gt 0) { throw "Release surface is incomplete: $($missing -join ', ')" }

$scanRoots = @(".codex-plugin", "skills", "offline", "src", "scripts", "unreal", "Docs", "README.md", "README.zh-TW.md", "README.en.md", "package.json")
$legacyPatterns = @(("Pi" + "UnrealBlueprint"), ("pi-unreal" + "-blueprint"), ("PI_UNREAL" + "_"), ("@earendil" + "-works"), ("Pi Ext" + "ension"))
$legacyHits = @()
foreach ($root in $scanRoots) {
    $path = Join-Path $repo $root
    $files = if (Test-Path -LiteralPath $path -PathType Leaf) { @(Get-Item -LiteralPath $path) } else { @(Get-ChildItem -LiteralPath $path -File -Recurse | Where-Object { $_.FullName -notmatch "[\\/](node_modules|dist|Binaries|Intermediate|artifacts)[\\/]" }) }
    foreach ($file in $files) {
        if (Select-String -LiteralPath $file.FullName -Pattern $legacyPatterns -SimpleMatch -Quiet -ErrorAction SilentlyContinue) { $legacyHits += [System.IO.Path]::GetRelativePath($repo, $file.FullName).Replace("\", "/") }
    }
}
if ($legacyHits.Count -gt 0) { throw "Legacy identity remains: $(($legacyHits | Sort-Object -Unique) -join ', ')" }

if (-not $AllowOpenGate) {
    $open = @(Select-String -LiteralPath "$repo/Docs/v1-release-gate.md" -Pattern "^- \[ \]")
    if ($open.Count -gt 0) { throw "v1 release gate has $($open.Count) open item(s)." }
}
Write-Host "Release metadata is consistent: product=$Version protocol=$($cppProtocol.Groups[1].Value)"
