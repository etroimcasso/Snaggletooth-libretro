# Package a built core as the archive a person downloads, and prove the archive before it goes
# anywhere. The Windows form of package.sh: the same archive, the same checks.
#
#     scripts\package.ps1 -Module <core DLL> -WorkDir <work dir> -Output <output.zip> -Asset <asset name>
#
# The archive holds ONE folder, named for the asset, with four files inside it: the core module,
# its info file, the core's LICENSE and Snaggletooth's as LICENSE.Snaggletooth (the library is
# linked into the module). Unzipping it never scatters files into the download directory, and
# everything a frontend installs, and everything the module's licenses require, arrives together.
#
# The info file is read from beside the module, where every build puts it; the licenses from this
# repository, found from this script's own location.
#
# After packing, the archive is expanded again and the expanded files, not the ones packed, are
# checked: exactly the four, the module and the info file byte for byte what went in. The module
# is also checked to carry its own C runtime, which the build links statically: a DLL that
# imported the Visual C++ runtime DLLs would fail to load on a machine without the redistributable.

param(
    [Parameter(Mandatory = $true)] [string] $Module,
    [Parameter(Mandatory = $true)] [string] $WorkDir,
    [Parameter(Mandatory = $true)] [string] $Output,
    [Parameter(Mandatory = $true)] [string] $Asset
)

$ErrorActionPreference = "Stop"

# The asset is named for the module and the platform, one of the five the core is built on.
$assets = @("snaggletooth_libretro-linux-x64", "snaggletooth_libretro-linux-arm64", "snaggletooth_libretro-macos-arm64",
            "snaggletooth_libretro-windows-x64", "snaggletooth_libretro-windows-arm64")
if ($assets -notcontains $Asset) {
    Write-Error "$Asset is not a release asset name (snaggletooth_libretro-<os>-<arch>)"
    exit 1
}

$root = Resolve-Path (Join-Path $PSScriptRoot "..")
$moduleName = Split-Path -Leaf $Module
$info = Join-Path (Split-Path -Parent $Module) "snaggletooth_libretro.info"
$license = Join-Path $root "LICENSE"
$libraryLicense = Join-Path $root "third_party\snaggletooth\LICENSE"

foreach ($file in @($Module, $info, $license, $libraryLicense)) {
    if (-not (Test-Path -PathType Leaf $file)) {
        Write-Error "no file at $file"
        exit 1
    }
}

# The module carries its own C runtime. A DLL built against the shared runtime names the runtime
# DLLs in its import table; a module that names any of them was not linked statically.
$bytes = [System.IO.File]::ReadAllText($Module, [System.Text.Encoding]::ASCII)
foreach ($runtime in @("VCRUNTIME140", "MSVCP140", "api-ms-win-crt")) {
    if ($bytes.Contains($runtime)) {
        Write-Error "$moduleName needs the machine's C runtime ($runtime); the build links the runtime into the module, and this one did not."
        exit 1
    }
}

if (Test-Path $WorkDir) { Remove-Item -Recurse -Force $WorkDir }
$stage = Join-Path $WorkDir $Asset
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item $Module (Join-Path $stage $moduleName)
Copy-Item $info (Join-Path $stage "snaggletooth_libretro.info")
Copy-Item $license (Join-Path $stage "LICENSE")
Copy-Item $libraryLicense (Join-Path $stage "LICENSE.Snaggletooth")

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Output) | Out-Null
if (Test-Path $Output) { Remove-Item -Force $Output }
Compress-Archive -Path $stage -DestinationPath $Output
if (-not (Test-Path $Output)) {
    Write-Error "no archive produced at $Output"
    exit 1
}

# Expand what a downloader would get, and check those files. Expanding, rather than reading the
# entry names, keeps this off the question of which slash Compress-Archive writes into them.
$verify = Join-Path $WorkDir "verify"
Expand-Archive -Path $Output -DestinationPath $verify -Force

$status = 0

$top = @(Get-ChildItem $verify | ForEach-Object { $_.Name })
if ($top.Count -ne 1 -or $top[0] -ne $Asset) {
    Write-Host "error: the archive's top level is not the one folder $Asset; it holds: $($top -join ', ')"
    $status = 1
}

$expanded = Join-Path $verify $Asset
foreach ($name in @($moduleName, "snaggletooth_libretro.info", "LICENSE", "LICENSE.Snaggletooth")) {
    if (-not (Test-Path -PathType Leaf (Join-Path $expanded $name))) {
        Write-Host "error: the archive does not contain $Asset\$name"
        $status = 1
    }
}

$count = @(Get-ChildItem -Recurse -File $expanded).Count
if ($count -ne 4) {
    $found = (Get-ChildItem -Recurse -File $verify | ForEach-Object { $_.FullName }) -join ", "
    Write-Host "error: the archive holds $count files, not the four it ships: $found"
    $status = 1
}

function Same-Bytes($a, $b) {
    return (Get-FileHash -Algorithm SHA256 $a).Hash -eq (Get-FileHash -Algorithm SHA256 $b).Hash
}
if ((Test-Path (Join-Path $expanded $moduleName)) -and -not (Same-Bytes $Module (Join-Path $expanded $moduleName))) {
    Write-Host "error: the expanded $moduleName differs from the built module"
    $status = 1
}
if ((Test-Path (Join-Path $expanded "snaggletooth_libretro.info")) -and -not (Same-Bytes $info (Join-Path $expanded "snaggletooth_libretro.info"))) {
    Write-Host "error: the expanded info file differs from the build's"
    $status = 1
}

if ($status -ne 0) {
    Write-Host ""
    Write-Host "Refusing to call $Output a release archive."
    exit $status
}

Remove-Item -Recurse -Force $verify
$size = (Get-Item $Module).Length
Write-Host "packaged $Output ($Asset`: $moduleName $size bytes, snaggletooth_libretro.info, LICENSE, LICENSE.Snaggletooth)"
