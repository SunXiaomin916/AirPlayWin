[CmdletBinding()]
param(
    [switch]$Hidden,
    [string]$Name = "AirPlayWin"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$installRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$receiver = Join-Path $installRoot "AirPlayWin.exe"
if (-not (Test-Path -LiteralPath $receiver -PathType Leaf)) {
    throw "AirPlayWin.exe was not found at '$receiver'."
}

$running = Get-Process -Name "AirPlayWin" -ErrorAction SilentlyContinue | Where-Object {
    try {
        [IO.Path]::GetFullPath($_.Path) -eq $receiver
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
Get-ChildItem -LiteralPath $logRoot -Filter "receiver-*.log" -File -ErrorAction SilentlyContinue |
    Where-Object { $_.LastWriteTime -lt $pruneDeadline } |
    Remove-Item -Force -ErrorAction SilentlyContinue
$timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
$stdoutLog = Join-Path $logRoot "receiver-$timestamp.log"
$safeName = $Name.Replace('"', '')

$start = @{
    FilePath = $receiver
    ArgumentList = @(
        "--serve",
        "--classic-raop",
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
Write-Output "AirPlayWin started with process id $($process.Id)."
Write-Output "Log: '$stdoutLog'."
