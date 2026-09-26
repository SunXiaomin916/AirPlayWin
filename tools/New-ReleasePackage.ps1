[CmdletBinding()]
param(
    [ValidateSet("Release")]
    [string]$Configuration = "Release",
    [string]$CertificateThumbprint,
    [string]$SignToolPath,
    [switch]$AllowDirty
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$root = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$build = Join-Path $root "build\vs2022-x64"
$cmakeProject = Get-Content -Raw -LiteralPath (Join-Path $root "CMakeLists.txt")
$versionMatch = [regex]::Match($cmakeProject, 'project\(AirPlayWin VERSION ([0-9]+\.[0-9]+\.[0-9]+)')
if (-not $versionMatch.Success) {
    throw "Unable to read the AirPlayWin version from CMakeLists.txt."
}
$expectedVersion = $versionMatch.Groups[1].Value
$archive = Join-Path $build "AirPlayWin-$expectedVersion-windows-x64.zip"
$checksum = "$archive.sha256"
$manifest = Join-Path $build "AirPlayWin-$expectedVersion-release.json"
$bundledCMake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"

function Resolve-Tool([string]$Name) {
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }
    $candidate = Join-Path $bundledCMake "$Name.exe"
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
        return $candidate
    }
    throw "Unable to locate $Name."
}

$cmake = Resolve-Tool "cmake"
$ctest = Resolve-Tool "ctest"
$cpack = Resolve-Tool "cpack"

if (-not $AllowDirty) {
    $dirty = & git -C $root status --porcelain
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to inspect Git status."
    }
    if ($dirty) {
        throw "Release packaging requires a clean Git checkout."
    }
}

& $cmake --preset vs2022-x64
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }
& $cmake --build --preset release --parallel
if ($LASTEXITCODE -ne 0) { throw "Release build failed." }
& $ctest --preset release
if ($LASTEXITCODE -ne 0) { throw "Release tests failed." }

$receiver = Join-Path $build "Release\AirPlayWin.exe"
$reportedVersion = (& $receiver --version | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $reportedVersion -ne "AirPlayWin $expectedVersion") {
    throw "Version smoke failed: '$reportedVersion'."
}
$logSmoke = Join-Path $build "release-log-smoke.log"
Remove-Item -LiteralPath $logSmoke -Force -ErrorAction SilentlyContinue
& $receiver --serve --classic-raop --name "AirPlayWin Release Smoke" --duration 1 `
    --diagnostics-interval 1 --log-file $logSmoke
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $logSmoke -PathType Leaf) -or
    (Get-Item -LiteralPath $logSmoke).Length -eq 0) {
    throw "Native diagnostics log smoke failed."
}
Remove-Item -LiteralPath $logSmoke -Force

$signingRequested = -not [string]::IsNullOrWhiteSpace($CertificateThumbprint) -or
                    -not [string]::IsNullOrWhiteSpace($SignToolPath)
if ($signingRequested) {
    if ([string]::IsNullOrWhiteSpace($CertificateThumbprint) -or
        [string]::IsNullOrWhiteSpace($SignToolPath) -or
        -not (Test-Path -LiteralPath $SignToolPath -PathType Leaf)) {
        throw "Provide both CertificateThumbprint and a valid SignToolPath."
    }
    & $SignToolPath sign /sha1 $CertificateThumbprint /fd SHA256 `
        /tr "http://timestamp.digicert.com" /td SHA256 $receiver
    if ($LASTEXITCODE -ne 0) { throw "Authenticode signing failed." }
    & $SignToolPath verify /pa $receiver
    if ($LASTEXITCODE -ne 0) { throw "Authenticode verification failed." }
}

Remove-Item -LiteralPath $archive -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $checksum -Force -ErrorAction SilentlyContinue
& $cpack --config (Join-Path $build "CPackConfig.cmake") -C $Configuration -B $build
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $archive -PathType Leaf)) {
    throw "CPack did not create the expected archive '$archive'."
}

$verifyRoot = [IO.Path]::GetFullPath((Join-Path $build "release-verify-$expectedVersion"))
if (-not $verifyRoot.StartsWith(([IO.Path]::GetFullPath($build) + [IO.Path]::DirectorySeparatorChar),
                                [StringComparison]::OrdinalIgnoreCase)) {
    throw "Unsafe release verification path."
}
Remove-Item -LiteralPath $verifyRoot -Recurse -Force -ErrorAction SilentlyContinue
Expand-Archive -LiteralPath $archive -DestinationPath $verifyRoot

$packagedReceiver = Get-ChildItem -LiteralPath $verifyRoot -Recurse -Filter "AirPlayWin.exe" |
    Where-Object { $_.FullName -match "[\\/]bin[\\/]AirPlayWin\.exe$" } |
    Select-Object -First 1
if (-not $packagedReceiver) {
    throw "The packaged receiver was not found under bin."
}
$requiredNames = @("README.md", "RELEASE_NOTES.md", "SECURITY.md", "Install-AirPlayWin.ps1",
                   "Start-AirPlayWin.ps1", "Uninstall-AirPlayWin.ps1",
                   "release.md", "AirPlayWin-icon-1024.png", "AirPlayWin.ico")
foreach ($required in $requiredNames) {
    if (-not (Get-ChildItem -LiteralPath $verifyRoot -Recurse -File |
        Where-Object Name -eq $required | Select-Object -First 1)) {
        throw "Package is missing '$required'."
    }
}
if (Get-ChildItem -LiteralPath $verifyRoot -Recurse -File |
    Where-Object { $_.Name -match "(^airplaywin_tests\.exe$|\.pdb$|\.obj$|\.lib$)" }) {
    throw "Package contains developer-only build artifacts."
}
$packagedVersion = (& $packagedReceiver.FullName --version | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $packagedVersion -ne "AirPlayWin $expectedVersion") {
    throw "Packaged version smoke failed: '$packagedVersion'."
}

$hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
if (-not (Test-Path -LiteralPath $checksum -PathType Leaf)) {
    Set-Content -LiteralPath $checksum -Value $hash -Encoding ascii
}
$checksumText = (Get-Content -Raw -LiteralPath $checksum).ToLowerInvariant()
if (-not $checksumText.Contains($hash)) {
    throw "The generated SHA-256 file does not match the release archive."
}
$commit = (& git -C $root rev-parse HEAD | Out-String).Trim()
[ordered]@{
    product = "AirPlayWin"
    version = $expectedVersion
    configuration = $Configuration
    commit = $commit
    archive = [IO.Path]::GetFileName($archive)
    sha256 = $hash
    authenticodeSigned = $signingRequested
    testsPassed = $true
    packagedVersion = $packagedVersion
    generatedAtUtc = [DateTime]::UtcNow.ToString("o")
} | ConvertTo-Json | Set-Content -LiteralPath $manifest -Encoding utf8

Write-Output "Release archive: $archive"
Write-Output "SHA-256: $hash"
Write-Output "Manifest: $manifest"
