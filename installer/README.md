# AirPlayWin beta installation

Build and create the x64 package from a Visual Studio Developer PowerShell:

```powershell
cmake --build --preset release --parallel
cpack --config .\build\vs2022-x64\CPackConfig.cmake -C Release
```

Extract `AirPlayWin-0.9.1-windows-x64.zip`, open an Administrator PowerShell in the extracted
folder, and run:

```powershell
.\installer\Install-AirPlayWin.ps1
```

Use `-EnableStartup` to add the current user's startup entry. The installer copies only the
packaged executable, README, and maintenance scripts to
`%LOCALAPPDATA%\Programs\AirPlayWin`, creates a Start Menu shortcut, and asks the executable
to install two inbound rules:

- TCP control ports 5000 and 7000, restricted to the exact program and Private profile;
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

The beta ZIP is unsigned. Production MSIX/signing and automatic update feeds remain reserved
for the later productization milestone; do not distribute a development certificate as a
trusted production publisher.
