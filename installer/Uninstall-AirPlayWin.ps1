[CmdletBinding()]
param(
    [string]$InstallRoot = (Join-Path $env:LOCALAPPDATA "Programs\AirPlayWin"),
    [switch]$KeepFirewallRules
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

$targetRoot = [IO.Path]::GetFullPath($InstallRoot)
$expectedRoot = [IO.Path]::GetFullPath((Join-Path $env:LOCALAPPDATA "Programs\AirPlayWin"))
if ($targetRoot -ne $expectedRoot -or [IO.Path]::GetFileName($targetRoot) -ne "AirPlayWin") {
    throw "Refusing to remove a directory other than the exact per-user AirPlayWin install root."
}
$targetApp = Join-Path $targetRoot "AirPlayWinApp.exe"
$targetExe = Join-Path $targetRoot "AirPlayWin.exe"
$runningInstalled = Get-Process -Name "AirPlayWinApp", "AirPlayWin" -ErrorAction SilentlyContinue |
    Where-Object {
        try {
            $path = [IO.Path]::GetFullPath($_.Path)
            $path -eq $targetApp -or $path -eq $targetExe
        } catch {
            $false
        }
    }
if ($runningInstalled) {
    # Request an orderly shutdown: the GUI exits via its close message; the
    # console receiver stops via its stop event.
    if (Test-Path -LiteralPath $targetExe -PathType Leaf) {
        & $targetExe --stop 2>$null
    }
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    do {
        Start-Sleep -Milliseconds 200
        $runningInstalled = Get-Process -Name "AirPlayWinApp", "AirPlayWin" -ErrorAction SilentlyContinue |
            Where-Object {
                try {
                    $path = [IO.Path]::GetFullPath($_.Path)
                    $path -eq $targetApp -or $path -eq $targetExe
                } catch {
                    $false
                }
            }
    } while ($runningInstalled -and [DateTime]::UtcNow -lt $deadline)
    if ($runningInstalled) {
        $runningInstalled | Stop-Process -Force
    }
}
if (-not $KeepFirewallRules) {
    if (-not (Test-IsAdministrator)) {
        throw "Run the uninstaller as Administrator to remove firewall rules, or pass -KeepFirewallRules."
    }
    $firewallOwner = if (Test-Path -LiteralPath $targetApp -PathType Leaf) { $targetApp }
                     elseif (Test-Path -LiteralPath $targetExe -PathType Leaf) { $targetExe }
                     else { $null }
    if ($firewallOwner) {
        & $firewallOwner --remove-firewall-rules
        if ($LASTEXITCODE -ne 0) {
            throw "Firewall rule removal failed with exit code $LASTEXITCODE."
        }
    }
}

Remove-ItemProperty -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Name "AirPlayWin" -ErrorAction SilentlyContinue
$programs = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs"
foreach ($name in @("AirPlayWin.lnk", "AirPlayWin 控制台.lnk", "Stop AirPlayWin.lnk")) {
    Remove-Item -LiteralPath (Join-Path $programs $name) -Force -ErrorAction SilentlyContinue
}

if (Test-Path -LiteralPath $targetRoot -PathType Container) {
    Remove-Item -LiteralPath $targetRoot -Recurse -Force
}
Write-Output "AirPlayWin was removed from '$targetRoot'."
