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
$sourceApp = Join-Path $sourceRoot "bin\AirPlayWinApp.exe"
$sourceCli = Join-Path $sourceRoot "bin\AirPlayWin.exe"
$sourceUi = Join-Path $sourceRoot "ui"
if (-not (Test-Path -LiteralPath $sourceApp -PathType Leaf)) {
    throw "AirPlayWinApp.exe was not found at '$sourceApp'. Extract the complete package first."
}
if (-not (Test-Path -LiteralPath (Join-Path $sourceUi "index.html") -PathType Leaf)) {
    throw "The UI resources were not found at '$sourceUi'. Extract the complete package first."
}
if (-not $SkipFirewall -and -not (Test-IsAdministrator)) {
    throw "Run this installer as Administrator so it can create Private-profile firewall rules, or pass -SkipFirewall for a diagnostic-only install."
}

$targetRoot = [IO.Path]::GetFullPath($InstallRoot)
$expectedRoot = [IO.Path]::GetFullPath((Join-Path $env:LOCALAPPDATA "Programs\AirPlayWin"))
if ($targetRoot -ne $expectedRoot -or [IO.Path]::GetFileName($targetRoot) -ne "AirPlayWin") {
    throw "InstallRoot must be the exact per-user AirPlayWin install directory."
}
$targetApp = Join-Path $targetRoot "AirPlayWinApp.exe"
$targetCli = Join-Path $targetRoot "AirPlayWin.exe"
$targetUi = Join-Path $targetRoot "ui"
$targetInstaller = Join-Path $targetRoot "installer"
$runningInstalled = Get-Process -Name "AirPlayWinApp", "AirPlayWin" -ErrorAction SilentlyContinue |
    Where-Object {
        try {
            $path = [IO.Path]::GetFullPath($_.Path)
            $path -eq $targetApp -or $path -eq $targetCli
        } catch {
            $false
        }
    }
if ($runningInstalled) {
    throw "Close the installed AirPlayWin application before installing or upgrading."
}
New-Item -ItemType Directory -Path $targetRoot -Force | Out-Null
New-Item -ItemType Directory -Path $targetInstaller -Force | Out-Null
if (Test-Path -LiteralPath $targetUi) {
    Remove-Item -LiteralPath $targetUi -Recurse -Force
}
Copy-Item -LiteralPath $sourceApp -Destination $targetApp -Force
if (Test-Path -LiteralPath $sourceCli -PathType Leaf) {
    Copy-Item -LiteralPath $sourceCli -Destination $targetCli -Force
}
Copy-Item -LiteralPath $sourceUi -Destination $targetUi -Recurse -Force
Copy-Item -LiteralPath (Join-Path $sourceRoot "README.md") -Destination $targetRoot -Force
Copy-Item -LiteralPath (Join-Path $sourceRoot "RELEASE_NOTES.md") -Destination $targetRoot -Force
Copy-Item -LiteralPath (Join-Path $sourceRoot "SECURITY.md") -Destination $targetRoot -Force
Copy-Item -Path (Join-Path $sourceRoot "installer\*.ps1") -Destination $targetInstaller -Force

if (-not $SkipFirewall) {
    & $targetApp --install-firewall-rules
    if ($LASTEXITCODE -ne 0) {
        throw "AirPlayWin firewall rule installation failed with exit code $LASTEXITCODE."
    }
}

$programs = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs"
$shell = New-Object -ComObject WScript.Shell

$shortcutPath = Join-Path $programs "AirPlayWin.lnk"
$shortcut = $shell.CreateShortcut($shortcutPath)
$shortcut.TargetPath = $targetApp
$shortcut.Arguments = ""
$shortcut.WorkingDirectory = $targetRoot
$shortcut.IconLocation = "$targetApp,0"
$shortcut.Description = "AirPlayWin - Windows AirPlay 音频接收器"
$shortcut.Save()

if (Test-Path -LiteralPath $targetCli -PathType Leaf) {
    $consoleShortcutPath = Join-Path $programs "AirPlayWin 控制台.lnk"
    $consoleShortcut = $shell.CreateShortcut($consoleShortcutPath)
    $consoleShortcut.TargetPath = $targetCli
    $consoleShortcut.Arguments = "--serve --classic-raop --run-until-stopped"
    $consoleShortcut.WorkingDirectory = $targetRoot
    $consoleShortcut.IconLocation = "$targetCli,0"
    $consoleShortcut.Description = "AirPlayWin 命令行接收器（诊断用）"
    $consoleShortcut.Save()
}

$runKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
if ($EnableStartup) {
    $startupCommand = '"{0}" --minimized' -f $targetApp
    New-ItemProperty -Path $runKey -Name "AirPlayWin" -Value $startupCommand -PropertyType String -Force | Out-Null
} else {
    Remove-ItemProperty -Path $runKey -Name "AirPlayWin" -ErrorAction SilentlyContinue
}

Write-Output "AirPlayWin installed to '$targetRoot'."
Write-Output "Start-menu shortcuts created. Startup enabled: $([bool]$EnableStartup)."
if ($SkipFirewall) {
    Write-Warning "Firewall rules were skipped. Remote discovery/control may be blocked."
}
