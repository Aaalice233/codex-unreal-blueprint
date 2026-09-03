[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9]+\.[0-9]+\.[0-9]+(?:[-+][0-9A-Za-z.-]+)?$')]
    [string]$Version
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repo = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$package = Get-Content -LiteralPath (Join-Path $repo "package.json") -Raw -Encoding UTF8 | ConvertFrom-Json
$packageLock = Get-Content -LiteralPath (Join-Path $repo "package-lock.json") -Raw -Encoding UTF8 | ConvertFrom-Json -AsHashtable
$plugin = Get-Content -LiteralPath (Join-Path $repo "unreal/PiUnrealBlueprint/PiUnrealBlueprint.uplugin") -Raw -Encoding UTF8 | ConvertFrom-Json
$protocolHeader = Get-Content -LiteralPath (Join-Path $repo "unreal/PiUnrealBlueprint/Source/PiUnrealBlueprintCore/Public/PiUnrealBlueprintProtocol.h") -Raw -Encoding UTF8
$protocolTs = Get-Content -LiteralPath (Join-Path $repo "src/shared/protocol.ts") -Raw -Encoding UTF8
$changelog = Get-Content -LiteralPath (Join-Path $repo "CHANGELOG.md") -Raw -Encoding UTF8

$versions = [ordered]@{
    "package.json" = [string]$package.version
    "package-lock.json" = [string]$packageLock["version"]
    "package-lock.json root package" = [string]$packageLock["packages"][""]["version"]
    "PiUnrealBlueprint.uplugin" = [string]$plugin.VersionName
}
foreach ($entry in $versions.GetEnumerator()) {
    if ($entry.Value -ne $Version) {
        throw "$($entry.Key) version $($entry.Value) does not match release version $Version."
    }
}

$pluginVersionMatch = [regex]::Match($protocolHeader, 'PluginVersion\s*=\s*TEXT\("([^"]+)"\)')
$cppProtocolMatch = [regex]::Match($protocolHeader, 'ProtocolVersion\s*=\s*TEXT\("([^"]+)"\)')
$tsProtocolMatch = [regex]::Match($protocolTs, 'CLIENT_PROTOCOL_VERSION\s*=\s*"([^"]+)"')
if (-not $pluginVersionMatch.Success -or $pluginVersionMatch.Groups[1].Value -ne $Version) {
    throw "C++ PluginVersion does not match release version $Version."
}
if (-not $cppProtocolMatch.Success -or -not $tsProtocolMatch.Success -or $cppProtocolMatch.Groups[1].Value -ne $tsProtocolMatch.Groups[1].Value) {
    throw "TypeScript and C++ protocol versions do not match."
}
if ($cppProtocolMatch.Groups[1].Value -ne "1.0.0") {
    throw "Protocol version must remain 1.0.0 for the v1 protocol."
}
if ([bool]$plugin.IsBetaVersion) {
    throw "PiUnrealBlueprint.uplugin must set IsBetaVersion=false for a stable release."
}
$releaseMajor = [int]($Version.Split('.')[0])
if ([int]$plugin.Version -ne $releaseMajor) {
    throw "Plugin numeric Version $($plugin.Version) does not match release major $releaseMajor."
}
if ($changelog -notmatch [regex]::Escape("## [$Version]")) {
    throw "CHANGELOG.md has no ## [$Version] release section."
}

$requiredReleaseFiles = @(
    "extensions/index.ts",
    "src/cli/run.ts",
    "src/cli/commandlet.ts",
    "src/cli/plugin-manager.ts",
    "scripts/setup.ps1",
    "tests/ts/contracts.test.ts",
    "tests/ts/commandlet.test.ts",
    "tests/ts/jobs.test.ts",
    "tests/ts/plugin-manager.test.ts",
    "tests/fixtures/driver/run-ue427-e2e.ps1",
    "Docs/cli-reference.md",
    "Docs/cli-reference.zh-CN.md",
    "Docs/setup.md",
    "Docs/setup.zh-CN.md"
)
$missingReleaseFiles = @($requiredReleaseFiles | Where-Object { -not (Test-Path -LiteralPath (Join-Path $repo $_) -PathType Leaf) })
if ($missingReleaseFiles.Count -gt 0) {
    throw "Release surface is incomplete: $($missingReleaseFiles -join ', ')"
}

$legacyRpcPatterns = @(
    ("blueprint" + "_history"),
    ("blueprint" + ".history"),
    ("EJobPhase::" + "Backup")
)
$legacyRpcFiles = @(Get-ChildItem -LiteralPath $repo -File -Recurse | Where-Object {
    $_.FullName -notmatch "[\\/](node_modules|\.git|\.?artifacts|Binaries|Intermediate)[\\/]" -and
    $_.Extension -in @(".ts", ".cpp", ".h", ".md")
} | Where-Object {
    Select-String -LiteralPath $_.FullName -Pattern $legacyRpcPatterns -SimpleMatch -Quiet
})
if ($legacyRpcFiles.Count -gt 0) {
    $relative = $legacyRpcFiles | ForEach-Object { [System.IO.Path]::GetRelativePath($repo, $_.FullName).Replace("\", "/") }
    throw "Legacy history RPC remains in the release surface: $($relative -join ', ')"
}

if ($releaseMajor -ge 1) {
    $gatePath = Join-Path $repo "Docs/v1-release-gate.md"
    $openItems = @(Select-String -LiteralPath $gatePath -Pattern "^- \[ \]" | ForEach-Object { $_.Line })
    if ($openItems.Count -gt 0) {
        throw "v1 release gate still has $($openItems.Count) open item(s)."
    }
}

Write-Host "Release metadata and safety gates are consistent: product=$Version protocol=$($cppProtocolMatch.Groups[1].Value)"
