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
$targetExe = Join-Path $targetRoot "AirPlayWin.exe"
if (-not $KeepFirewallRules) {
    if (-not (Test-IsAdministrator)) {
        throw "Run the uninstaller as Administrator to remove firewall rules, or pass -KeepFirewallRules."
    }
    if (Test-Path -LiteralPath $targetExe -PathType Leaf) {
        & $targetExe --remove-firewall-rules
        if ($LASTEXITCODE -ne 0) {
            throw "Firewall rule removal failed with exit code $LASTEXITCODE."
        }
    }
}

Remove-ItemProperty -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Name "AirPlayWin" -ErrorAction SilentlyContinue
$shortcut = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs\AirPlayWin.lnk"
Remove-Item -LiteralPath $shortcut -Force -ErrorAction SilentlyContinue

if (Test-Path -LiteralPath $targetRoot -PathType Container) {
    Remove-Item -LiteralPath $targetRoot -Recurse -Force
}
Write-Output "AirPlayWin was removed from '$targetRoot'."
