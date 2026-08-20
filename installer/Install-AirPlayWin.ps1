[CmdletBinding()]
param(
    [string]$PackageRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$InstallRoot = (Join-Path $env:LOCALAPPDATA "Programs\AirPlayWin"),
    [switch]$EnableStartup,
    [switch]$SkipFirewall
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

$sourceRoot = [IO.Path]::GetFullPath($PackageRoot)
$sourceExe = Join-Path $sourceRoot "bin\AirPlayWin.exe"
if (-not (Test-Path -LiteralPath $sourceExe -PathType Leaf)) {
    throw "AirPlayWin.exe was not found at '$sourceExe'. Extract the complete package first."
}
if (-not $SkipFirewall -and -not (Test-IsAdministrator)) {
    throw "Run this installer as Administrator so it can create Private-profile firewall rules, or pass -SkipFirewall for a diagnostic-only install."
}

$targetRoot = [IO.Path]::GetFullPath($InstallRoot)
$expectedRoot = [IO.Path]::GetFullPath((Join-Path $env:LOCALAPPDATA "Programs\AirPlayWin"))
if ($targetRoot -ne $expectedRoot -or [IO.Path]::GetFileName($targetRoot) -ne "AirPlayWin") {
    throw "InstallRoot must be the exact per-user AirPlayWin install directory."
}
$targetExe = Join-Path $targetRoot "AirPlayWin.exe"
$targetInstaller = Join-Path $targetRoot "installer"
$runningInstalled = Get-Process -Name "AirPlayWin" -ErrorAction SilentlyContinue | Where-Object {
    try {
        [IO.Path]::GetFullPath($_.Path) -eq $targetExe
    } catch {
        $false
    }
}
if ($runningInstalled) {
    throw "Close the installed AirPlayWin receiver before installing or upgrading."
}
New-Item -ItemType Directory -Path $targetRoot -Force | Out-Null
New-Item -ItemType Directory -Path $targetInstaller -Force | Out-Null
Copy-Item -LiteralPath $sourceExe -Destination $targetExe -Force
Copy-Item -LiteralPath (Join-Path $sourceRoot "README.md") -Destination $targetRoot -Force
Copy-Item -Path (Join-Path $sourceRoot "installer\*.ps1") -Destination $targetInstaller -Force

if (-not $SkipFirewall) {
    & $targetExe --install-firewall-rules
    if ($LASTEXITCODE -ne 0) {
        throw "AirPlayWin firewall rule installation failed with exit code $LASTEXITCODE."
    }
}

$programs = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs"
$shortcutPath = Join-Path $programs "AirPlayWin.lnk"
$shell = New-Object -ComObject WScript.Shell
$shortcut = $shell.CreateShortcut($shortcutPath)
$shortcut.TargetPath = $targetExe
$shortcut.Arguments = "--serve --run-until-stopped"
$shortcut.WorkingDirectory = $targetRoot
$shortcut.Description = "AirPlayWin audio receiver"
$shortcut.Save()

$runKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
if ($EnableStartup) {
    New-ItemProperty -Path $runKey -Name "AirPlayWin" -Value ('"{0}" --serve --run-until-stopped' -f $targetExe) -PropertyType String -Force | Out-Null
} else {
    Remove-ItemProperty -Path $runKey -Name "AirPlayWin" -ErrorAction SilentlyContinue
}

Write-Output "AirPlayWin installed to '$targetRoot'."
Write-Output "Start Menu shortcut created. Startup enabled: $([bool]$EnableStartup)."
if ($SkipFirewall) {
    Write-Warning "Firewall rules were skipped. Remote discovery/control may be blocked."
}
