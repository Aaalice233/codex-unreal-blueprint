function Get-SupportedUnrealVersion([string]$EngineRoot, [string]$UProject, [switch]$AllowMissing) {
    $versionPath = Join-Path $EngineRoot "Engine/Build/Build.version"
    if (-not (Test-Path -LiteralPath $versionPath -PathType Leaf)) {
        if ($AllowMissing) { return "dry-run" }
        throw "Missing Unreal Build.version: $versionPath"
    }
    $build = Get-Content -LiteralPath $versionPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $version = "$($build.MajorVersion).$($build.MinorVersion)"
    if ($version -notin @("4.26", "4.27")) { throw "Unsupported Unreal Engine $version; supported: 4.26, 4.27." }
    if (Test-Path -LiteralPath $UProject -PathType Leaf) {
        $project = Get-Content -LiteralPath $UProject -Raw -Encoding UTF8 | ConvertFrom-Json
        $association = [string]$project.EngineAssociation
        # GUID associations identify source builds; numeric associations must match the selected engine.
        if ($association -match '^\d+\.\d+(?:\.\d+)?$' -and -not ($association -eq $version -or $association.StartsWith("$version."))) {
            throw "Unreal version mismatch: project EngineAssociation=$association, EngineRoot=$version."
        }
    }
    return $version
}
