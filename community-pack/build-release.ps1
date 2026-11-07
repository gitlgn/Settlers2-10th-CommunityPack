<#
  Builds the Community Pack release zip:
    - d3d9.dll (Win32 Release, static CRT) from ..\source with Visual Studio 2022 (C++ workload) and CMake
    - decryptor_s2.exe from a checkout of S2-modders/S2-10th-Anniversary-decrypter (Rust)
  Usage: .\build-release.ps1 -Decrypter C:\path\to\S2-10th-Anniversary-decrypter [-Version 1.0]
#>
param(
    [Parameter(Mandatory = $true)][string]$Decrypter,
    [string]$Version = "dev"
)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$out  = Join-Path $root "dist"
$pack = Join-Path $out "CommunityPack"

# patch DLL
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
cmd /c "`"$vs\VC\Auxiliary\Build\vcvars32.bat`" >nul && cmake -S `"$root\source`" -B `"$root\build`" -A Win32 && cmake --build `"$root\build`" --config Release"
if ($LASTEXITCODE) { throw "patch build failed" }

# decrypter (unchanged upstream source)
Push-Location $Decrypter
cargo build --release
if ($LASTEXITCODE) { throw "decrypter build failed" }
Pop-Location

Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$pack\tools" | Out-Null
Copy-Item "$root\build\bin\Release\d3d9.dll" $pack
foreach ($f in "install.ps1", "install.cmd", "uninstall.ps1", "uninstall.cmd", "d3d9.ini.sections", "README.md", "THIRD-PARTY.md") {
    Copy-Item (Join-Path $PSScriptRoot $f) $pack
}
Copy-Item "$root\LICENSE" "$pack\LICENSE.txt"
Copy-Item "$Decrypter\target\release\decryptor_s2_bin.exe" "$pack\tools\decryptor_s2.exe"
Copy-Item "$Decrypter\LICENSE" "$pack\tools\decryptor_s2.LICENSE.txt"

$zip = Join-Path $out "S2DNG-CommunityPack-$Version.zip"
Compress-Archive -Path $pack -DestinationPath $zip -Force
Write-Host "release: $zip"
