[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Version,
    [Parameter(Mandatory = $true)]
    [string]$Output
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$changelogPath = Join-Path ([System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))) "CHANGELOG.md"
$lines = Get-Content -LiteralPath $changelogPath -Encoding UTF8
$heading = "## [$Version]"
$start = [Array]::IndexOf($lines, $heading)
if ($start -lt 0) { throw "CHANGELOG.md has no $heading section." }
$end = $lines.Count
for ($index = $start + 1; $index -lt $lines.Count; $index += 1) {
    if ($lines[$index] -match '^## \[') {
        $end = $index
        break
    }
}
if ($end -le $start + 1) { throw "$heading has no release notes." }
$body = ($lines[($start + 1)..($end - 1)] -join "`n").Trim()
if ([string]::IsNullOrWhiteSpace($body)) { throw "$heading has no release notes." }
[System.IO.File]::WriteAllText([System.IO.Path]::GetFullPath($Output), $body + "`n", [System.Text.UTF8Encoding]::new($false))
