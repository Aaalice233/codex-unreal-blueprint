[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$EditorCmd,

    [string]$Project = 'E:/Master/LuaSocial.uproject',

    [string]$RunId = ([Guid]::NewGuid().ToString('N')),

    [string]$Filter = 'PiUnrealBlueprint',

    [string]$Plugin,

    [switch]$KeepFixture
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$editorPath = (Resolve-Path -LiteralPath $EditorCmd).Path
$projectPath = (Resolve-Path -LiteralPath $Project).Path
$projectRoot = Split-Path -Parent $projectPath
$artifactRoot = Join-Path $PSScriptRoot "../../../.artifacts/ue-e2e/$RunId"
$artifactRoot = [IO.Path]::GetFullPath($artifactRoot)
$reportRoot = Join-Path $artifactRoot 'automation-report'
$logPath = Join-Path $artifactRoot 'ue-e2e.log'
$parityDirectPath = Join-Path $artifactRoot 'parity-core.json'
$parityRequestPath = Join-Path $artifactRoot 'parity-request.json'
$parityCommandletPath = Join-Path $artifactRoot 'parity-commandlet.json'
$applyRequestPath = Join-Path $artifactRoot 'apply-request.json'
$applyResultPath = Join-Path $artifactRoot 'apply-result.json'
$fixtureParent = Join-Path $projectRoot 'Content/PiAutomation'
$commandletFixtureDirectory = Join-Path $fixtureParent $RunId
$commandletAssetFile = Join-Path $commandletFixtureDirectory 'BP_CommandletApply.uasset'

New-Item -ItemType Directory -Force -Path $artifactRoot | Out-Null
New-Item -ItemType Directory -Force -Path $reportRoot | Out-Null
$env:PI_UE_AUTOMATION_RUN_ID = $RunId
$env:PI_UE_PARITY_DIRECT_RESULT = $parityDirectPath

$pluginArgument = $null
if ($Plugin) {
    $pluginPath = (Resolve-Path -LiteralPath $Plugin).Path
    $pluginArgument = "-PLUGIN=$pluginPath"
}

$arguments = @(
    $projectPath
    '-unattended'
    '-nop4'
    '-nosplash'
    '-nullrhi'
    '-stdout'
    '-FullStdOutLogOutput'
    "-ReportExportPath=$reportRoot"
    "-ExecCmds=`"Automation RunTests $Filter;Quit`""
    "-TestExit=`"Automation Test Queue Empty`""
)
if ($pluginArgument) { $arguments += $pluginArgument }

try {
    $process = Start-Process -FilePath $editorPath -ArgumentList $arguments -Wait -PassThru -NoNewWindow `
        -RedirectStandardOutput $logPath -RedirectStandardError (Join-Path $artifactRoot 'ue-e2e.stderr.log')
    if ($process.ExitCode -ne 0) {
        throw "UE4.27 Automation exited with code $($process.ExitCode). See $logPath"
    }
    $reports = @(Get-ChildItem -LiteralPath $reportRoot -Filter '*.json' -File -Recurse)
    if ($reports.Count -eq 0) {
        throw "UE did not produce an Automation JSON report under: $reportRoot"
    }
    $log = Get-Content -LiteralPath $logPath -Raw -Encoding UTF8
    if ($log -match 'Result=\{Failed\}|Automation Test Failed|Test Completed\. Result=Failed') {
        throw "UE Automation reported a failure. See $logPath"
    }
    if ($log -notmatch 'Automation Test Queue Empty') {
        throw "UE Automation did not reach queue completion. See $logPath"
    }
    if (-not (Test-Path -LiteralPath $parityDirectPath)) {
        throw "Core parity fixture was not exported by the public-entry Automation test: $parityDirectPath"
    }

    @{
        jsonrpc = '2.0'
        id = 'commandlet-parity-capabilities'
        method = 'blueprint.capabilities'
        params = @{ operationNames = @('asset.create', 'component.add', 'graph.add', 'widget.add', 'anim.variable.add') }
    } | ConvertTo-Json -Depth 4 -Compress | Set-Content -LiteralPath $parityRequestPath -Encoding UTF8
    $commandletArguments = @(
        $projectPath
        '-run=PiUnrealBlueprint'
        "-Request=`"$parityRequestPath`""
        "-Result=`"$parityCommandletPath`""
        '-unattended'
        '-nop4'
        '-nosplash'
        '-nullrhi'
    )
    if ($pluginArgument) { $commandletArguments += $pluginArgument }
    $commandlet = Start-Process -FilePath $editorPath -ArgumentList $commandletArguments -Wait -PassThru -NoNewWindow `
        -RedirectStandardOutput (Join-Path $artifactRoot 'commandlet-parity.log') `
        -RedirectStandardError (Join-Path $artifactRoot 'commandlet-parity.stderr.log')
    if ($commandlet.ExitCode -ne 0) {
        throw "Real PiUnrealBlueprint Commandlet exited with code $($commandlet.ExitCode)."
    }
    if (-not (Test-Path -LiteralPath $parityCommandletPath)) {
        throw "Real PiUnrealBlueprint Commandlet did not write: $parityCommandletPath"
    }
    $direct = Get-Content -LiteralPath $parityDirectPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $commandletResult = Get-Content -LiteralPath $parityCommandletPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $directCanonical = $direct.result | ConvertTo-Json -Depth 100 -Compress
    $commandletCanonical = $commandletResult.result | ConvertTo-Json -Depth 100 -Compress
    if ($directCanonical -cne $commandletCanonical) {
        throw "Commandlet OperationRegistry parity mismatch. Compare $parityDirectPath and $parityCommandletPath"
    }

    @{
        jsonrpc = '2.0'
        id = 'commandlet-apply-terminal'
        method = 'blueprint.apply'
        params = @{
            requestId = "driver-$RunId"
            operations = @(@{
                operation = 'asset.create'
                packagePath = "/Game/PiAutomation/$RunId/BP_CommandletApply"
                kind = 'blueprint'
                parentClassPath = '/Script/Engine.Actor'
            })
        }
    } | ConvertTo-Json -Depth 8 -Compress | Set-Content -LiteralPath $applyRequestPath -Encoding UTF8
    $applyArguments = @(
        $projectPath
        '-run=PiUnrealBlueprint'
        "-Request=`"$applyRequestPath`""
        "-Result=`"$applyResultPath`""
        '-unattended'
        '-nop4'
        '-nosplash'
        '-nullrhi'
    )
    if ($pluginArgument) { $applyArguments += $pluginArgument }
    $apply = Start-Process -FilePath $editorPath -ArgumentList $applyArguments -Wait -PassThru -NoNewWindow `
        -RedirectStandardOutput (Join-Path $artifactRoot 'commandlet-apply.log') `
        -RedirectStandardError (Join-Path $artifactRoot 'commandlet-apply.stderr.log')
    if ($apply.ExitCode -ne 0) {
        throw "Headless blueprint.apply exited with code $($apply.ExitCode)."
    }
    if (-not (Test-Path -LiteralPath $applyResultPath)) {
        throw "Headless blueprint.apply did not write: $applyResultPath"
    }
    $applyResult = Get-Content -LiteralPath $applyResultPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($applyResult.result.terminal -ne $true -or $applyResult.result.phase -ne 'Succeeded') {
        throw "Headless blueprint.apply did not return a succeeded terminal Job snapshot: $applyResultPath"
    }
    if ($applyResult.result.result.success -ne $true -or -not (Test-Path -LiteralPath $commandletAssetFile)) {
        throw "Headless blueprint.apply returned before the Blueprint was saved: $applyResultPath"
    }
    Remove-Item -LiteralPath $commandletFixtureDirectory -Recurse -Force

    # 每个测试都应自行清理；这里检查磁盘，避免 Fixture 泄漏到非沙盒目录。
    $leaks = @()
    if (Test-Path -LiteralPath $fixtureParent) {
        $leaks = @(Get-ChildItem -LiteralPath $fixtureParent -Directory -Filter "$RunId*")
    }
    if ($leaks.Count -gt 0) {
        throw "Fixture cleanup failed: $($leaks.FullName -join ', ')"
    }

    [pscustomobject]@{
        runId = $RunId
        filter = $Filter
        report = $reportRoot
        log = $logPath
        succeeded = $true
    } | ConvertTo-Json -Depth 4
}
finally {
    Remove-Item Env:PI_UE_AUTOMATION_RUN_ID -ErrorAction SilentlyContinue
    Remove-Item Env:PI_UE_PARITY_DIRECT_RESULT -ErrorAction SilentlyContinue
    if (-not $KeepFixture -and (Test-Path -LiteralPath $fixtureParent)) {
        Get-ChildItem -LiteralPath $fixtureParent -Directory -Filter "$RunId*" |
            ForEach-Object { Remove-Item -LiteralPath $_.FullName -Recurse -Force }
    }
    elseif (Test-Path -LiteralPath $commandletFixtureDirectory) {
        Remove-Item -LiteralPath $commandletFixtureDirectory -Recurse -Force
    }
}
