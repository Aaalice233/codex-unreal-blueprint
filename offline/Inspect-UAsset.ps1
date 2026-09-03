[CmdletBinding()]
param(
    [Parameter(Mandatory, Position = 0, ValueFromPipeline, ValueFromPipelineByPropertyName)]
    [Alias("FullName")]
    [string[]]$Path,

    [ValidateSet("markdown", "json", "raw-json")]
    [string]$Format = "markdown",

    [string[]]$Search = @(
        "SpawnProbability",
        "NonUseCountPercentage",
        "FrameRateOptimization"
    ),

    [string]$OutputPath,

    [string]$ContentRoot,

    [switch]$Rebuild
)

begin {
    $ErrorActionPreference = "Stop"
    $allInputPaths = [System.Collections.Generic.List[string]]::new()
}

process {
    foreach ($item in $Path) {
        if (-not [string]::IsNullOrWhiteSpace($item)) {
            $allInputPaths.Add($item)
        }
    }
}

end {
    if (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) {
        throw "未找到 dotnet。该脚本需要 .NET 8 SDK。"
    }

    $sourceDir = Join-Path $PSScriptRoot "uasset-inspector"
    $sourceFiles = @(
        (Join-Path $sourceDir "UnrealUAssetInspector.csproj")
    ) + @(
        Get-ChildItem -LiteralPath $sourceDir -Filter "*.cs" -File |
            ForEach-Object { $_.FullName }
    )
    foreach ($sourceFile in $sourceFiles) {
        if (-not (Test-Path -LiteralPath $sourceFile -PathType Leaf)) {
            throw "缺少解析器源文件：$sourceFile"
        }
    }

    $hashText = ($sourceFiles | ForEach-Object {
        (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash
    }) -join ""
    $hashBytes = [System.Text.Encoding]::UTF8.GetBytes($hashText)
    $hash = [Convert]::ToHexString(
        [System.Security.Cryptography.SHA256]::HashData($hashBytes)
    ).Substring(0, 16).ToLowerInvariant()

    $cacheBase = if ($env:LOCALAPPDATA) {
        $env:LOCALAPPDATA
    } else {
        [System.IO.Path]::GetTempPath()
    }
    $cacheRoot = Join-Path $cacheBase "CodexUnrealBlueprint/offline-uasset-inspector/$hash"
    $buildDir = Join-Path $cacheRoot "build"
    $binDir = Join-Path $cacheRoot "bin"
    $dllPath = Join-Path $binDir "UnrealUAssetInspector.dll"

    if ($Rebuild -or -not (Test-Path -LiteralPath $dllPath -PathType Leaf)) {
        New-Item -ItemType Directory -Force -Path $buildDir, $binDir | Out-Null
        Copy-Item -LiteralPath $sourceFiles -Destination $buildDir -Force
        $buildOutput = & dotnet build (Join-Path $buildDir "UnrealUAssetInspector.csproj") `
            --configuration Release `
            --output $binDir `
            --nologo 2>&1
        if ($LASTEXITCODE -ne 0) {
            throw "UAsset 解析器构建失败，dotnet exit code=$LASTEXITCODE`n$($buildOutput -join [Environment]::NewLine)"
        }
    }

    $assets = [System.Collections.Generic.List[string]]::new()
    foreach ($inputPath in $allInputPaths) {
        $resolved = Resolve-Path -LiteralPath $inputPath
        foreach ($resolvedPath in $resolved) {
            if (Test-Path -LiteralPath $resolvedPath.Path -PathType Container) {
                Get-ChildItem -LiteralPath $resolvedPath.Path -Recurse -File |
                    Where-Object { $_.Extension -in ".uasset", ".umap" } |
                    ForEach-Object { $assets.Add($_.FullName) }
            } elseif ([System.IO.Path]::GetExtension($resolvedPath.Path) -in ".uasset", ".umap") {
                $assets.Add($resolvedPath.Path)
            }
        }
    }

    $assets = @($assets | Sort-Object -Unique)
    if ($assets.Count -eq 0) {
        throw "没有找到 .uasset 或 .umap 文件。"
    }

    if ($OutputPath) {
        if ($assets.Count -gt 1) {
            New-Item -ItemType Directory -Force -Path $OutputPath | Out-Null
        } else {
            $outputParent = Split-Path -Parent $OutputPath
            if ($outputParent) {
                New-Item -ItemType Directory -Force -Path $outputParent | Out-Null
            }
        }
    }

    $normalizedSearch = @(
        $Search |
            ForEach-Object { $_ -split "," } |
            ForEach-Object { $_.Trim() } |
            Where-Object { $_ } |
            Sort-Object -Unique
    )

    foreach ($asset in $assets) {
        $arguments = @(
            $dllPath,
            "--input", $asset,
            "--format", $Format
        )
        if ($ContentRoot) {
            $arguments += @("--content-root", $ContentRoot)
        }
        foreach ($term in $normalizedSearch) {
            if (-not [string]::IsNullOrWhiteSpace($term)) {
                $arguments += @("--search", $term)
            }
        }

        $result = & dotnet @arguments
        if ($LASTEXITCODE -ne 0) {
            throw "解析失败：$asset，dotnet exit code=$LASTEXITCODE"
        }

        if ($OutputPath) {
            $extension = if ($Format -eq "markdown") { ".md" } else { ".json" }
            $target = if ($assets.Count -eq 1) {
                $OutputPath
            } else {
                Join-Path $OutputPath (
                    [System.IO.Path]::GetFileNameWithoutExtension($asset) + $extension
                )
            }
            $result | Set-Content -LiteralPath $target -Encoding utf8
            Write-Host "已生成：$target"
        } else {
            if ($assets.Count -gt 1) {
                Write-Output ""
                Write-Output "===== $asset ====="
            }
            Write-Output $result
        }
    }
}
