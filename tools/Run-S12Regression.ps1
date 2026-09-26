[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",

    [ValidateRange(0, 1000000)]
    [int]$Iterations = 0,

    [ValidateRange(0.0, 168.0)]
    [double]$DurationHours = 0.0,

    [switch]$SyncOnly,

    [string]$ResultPath = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ($Iterations -gt 0 -and $DurationHours -gt 0.0) {
    throw "Use either -Iterations or -DurationHours, not both."
}
if ($Iterations -eq 0 -and $DurationHours -eq 0.0) {
    $Iterations = 1
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$buildTestExecutable = Join-Path $repoRoot "build\vs2022-x64\tests\$Configuration\airplaywin_tests.exe"
$packagedTestExecutable = Join-Path $PSScriptRoot "airplaywin_tests.exe"
$testExecutable = if (Test-Path -LiteralPath $buildTestExecutable -PathType Leaf) {
    $buildTestExecutable
} else {
    $packagedTestExecutable
}
if (-not (Test-Path -LiteralPath $testExecutable -PathType Leaf)) {
    throw "Test executable not found. Build the $Configuration preset or use the packaged regression tools."
}

$started = [DateTimeOffset]::UtcNow
$deadline = if ($DurationHours -gt 0.0) {
    $started.AddHours($DurationHours)
} else {
    [DateTimeOffset]::MaxValue
}
$arguments = if ($SyncOnly) { @("--only", "GroupSoakRegression") } else { @() }
$completed = 0
$failed = 0
$failureDetails = [System.Collections.Generic.List[object]]::new()

while (($Iterations -eq 0 -or $completed -lt $Iterations) -and
       [DateTimeOffset]::UtcNow -lt $deadline) {
    $runStarted = [DateTimeOffset]::UtcNow
    $output = (& $testExecutable @arguments 2>&1 | Out-String).TrimEnd()
    $exitCode = $LASTEXITCODE
    ++$completed
    if ($exitCode -ne 0) {
        ++$failed
        $failureDetails.Add([ordered]@{
            iteration = $completed
            started_utc = $runStarted.ToString("o")
            exit_code = $exitCode
            output = $output
        })
    }
    $status = if ($exitCode -eq 0) { "PASS" } else { "FAIL" }
    Write-Host "[$status] iteration=$completed elapsed=$([DateTimeOffset]::UtcNow - $started)"
}

$finished = [DateTimeOffset]::UtcNow
if ([string]::IsNullOrWhiteSpace($ResultPath)) {
    $artifactDirectory = Join-Path $repoRoot "artifacts"
    [void](New-Item -ItemType Directory -Path $artifactDirectory -Force)
    $stamp = $started.ToString("yyyyMMdd-HHmmss")
    $ResultPath = Join-Path $artifactDirectory "s12-regression-$stamp.json"
} else {
    $ResultPath = [System.IO.Path]::GetFullPath($ResultPath)
    $parent = Split-Path -Parent $ResultPath
    if (-not [string]::IsNullOrWhiteSpace($parent)) {
        [void](New-Item -ItemType Directory -Path $parent -Force)
    }
}

$summary = [ordered]@{
    schema_version = 1
    phase = 12
    configuration = $Configuration
    suite = if ($SyncOnly) { "GroupSoakRegression" } else { "all" }
    requested_iterations = $Iterations
    requested_duration_hours = $DurationHours
    started_utc = $started.ToString("o")
    finished_utc = $finished.ToString("o")
    elapsed_seconds = [Math]::Round(($finished - $started).TotalSeconds, 3)
    completed_runs = $completed
    passed_runs = $completed - $failed
    failed_runs = $failed
    physical_hardware_acceptance = $false
    note = "This is a wall-clock software regression runner; it does not prove acoustic multi-endpoint skew or hardware interoperability."
    failures = $failureDetails
}
$summary | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $ResultPath -Encoding utf8
Write-Host "result=$ResultPath completed=$completed passed=$($completed - $failed) failed=$failed"

if ($failed -ne 0) {
    exit 1
}
