[CmdletBinding()]
param(
    [switch]$Hidden,
    [switch]$Console,
    [string]$Name = "AirPlayWin"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$installRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$app = Join-Path $installRoot "AirPlayWinApp.exe"
$cli = Join-Path $installRoot "AirPlayWin.exe"
if (-not (Test-Path -LiteralPath $app -PathType Leaf)) {
    throw "AirPlayWinApp.exe was not found at '$app'."
}

$running = Get-Process -Name "AirPlayWinApp", "AirPlayWin" -ErrorAction SilentlyContinue |
    Where-Object {
        try {
            [IO.Path]::GetFullPath($_.Path) -eq $app -or
            [IO.Path]::GetFullPath($_.Path) -eq $cli
        } catch {
            $false
        }
    }
if ($running) {
    Write-Output "AirPlayWin is already running."
    exit 0
}

$logRoot = Join-Path $env:LOCALAPPDATA "AirPlayWin\Logs"
New-Item -ItemType Directory -Path $logRoot -Force | Out-Null
# The receiver logs every diagnostics interval while it runs; without a cap
# an autostarted receiver fills the disk after a few months. Keep the most
# recent 30 days of logs and prune anything older on every start.
$pruneDeadline = (Get-Date).AddDays(-30)
Get-ChildItem -LiteralPath $logRoot -Filter "*.log" -File -ErrorAction SilentlyContinue |
    Where-Object { $_.LastWriteTime -lt $pruneDeadline } |
    Remove-Item -Force -ErrorAction SilentlyContinue

if ($Console -and (Test-Path -LiteralPath $cli -PathType Leaf)) {
    $timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $stdoutLog = Join-Path $logRoot "receiver-$timestamp.log"
    $safeName = $Name.Replace('"', '')
    $start = @{
        FilePath = $cli
        ArgumentList = @(
            "--serve", "--classic-raop",
            "--name", ('"{0}"' -f $safeName),
            "--run-until-stopped",
            "--diagnostics-interval", "30",
            "--log-file", ('"{0}"' -f $stdoutLog)
        )
        WorkingDirectory = $installRoot
    }
    if ($Hidden) {
        $start.WindowStyle = "Hidden"
    }
    $process = Start-Process @start -PassThru
    Write-Output "AirPlayWin console receiver started with process id $($process.Id)."
    Write-Output "Log: '$stdoutLog'."
    exit 0
}

$start = @{
    FilePath = $app
    ArgumentList = @("--minimized")
    WorkingDirectory = $installRoot
}
if (-not $Hidden) {
    # Launch the window normally; --minimized is only added for autostart.
    $start.ArgumentList = @()
}
$process = Start-Process @start -PassThru
Write-Output "AirPlayWin started with process id $($process.Id)."
