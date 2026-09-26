# AirPlayWin 1.0 installation

Build and create the x64 package from a Visual Studio Developer PowerShell:

```powershell
cmake --build --preset release --parallel
cpack --config .\build\vs2022-x64\CPackConfig.cmake -C Release
```

Extract `AirPlayWin-1.0.0-windows-x64.zip`, open an Administrator PowerShell in the extracted
folder, and run:

```powershell
.\installer\Install-AirPlayWin.ps1
```

Use `-EnableStartup` to add a hidden current-user startup entry with live timestamped logs under
`%LOCALAPPDATA%\AirPlayWin\Logs`. The installer copies the executable, release/security notes,
README, and maintenance scripts to
`%LOCALAPPDATA%\Programs\AirPlayWin`, creates a Start Menu shortcut, and asks the executable
to install two inbound rules:

- TCP control port 5000, restricted to the exact program and Private profile;
- negotiated UDP media/control/timing, restricted to the exact program and Private profile.

Custom control ports can be installed manually with:

```powershell
.\AirPlayWin.exe --install-firewall-rules --raop-port 5100 --airplay-port 7100
```

Inspect without changing the firewall:

```powershell
.\AirPlayWin.exe --firewall-status
```

Uninstall from an Administrator PowerShell:

```powershell
& "$env:LOCALAPPDATA\Programs\AirPlayWin\installer\Uninstall-AirPlayWin.ps1"
```

The Start Menu contains separate start and orderly-stop shortcuts. The v1.0 ZIP may be unsigned
when built without a publisher certificate; verify the adjacent SHA-256 file before installing.
Public redistribution should use an Authenticode certificate trusted for production code signing.
