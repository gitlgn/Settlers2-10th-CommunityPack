<#
  Settlers II - 10th Anniversary: Community Pack uninstaller
  Restores all original game files from <game>\mods\_backup and the previous d3d9.dll.
  -Purge also deletes the generated packs (<game>\mods).
#>
param([string]$GameDir, [switch]$Purge)
$ErrorActionPreference = "Stop"

if (-not $GameDir) { $GameDir = Split-Path $PSScriptRoot -Parent }
if (-not (Test-Path (Join-Path $GameDir "bin\S2DNG.exe"))) { $GameDir = Read-Host "Spielordner / game folder (contains bin\S2DNG.exe)" }
$GameDir = (Resolve-Path $GameDir).Path
$Bin = Join-Path $GameDir "bin"; $Data = Join-Path $GameDir "data"; $Mods = Join-Path $GameDir "mods"
if (Get-Process S2DNG -ErrorAction SilentlyContinue) { throw "Please close the game first / Bitte zuerst das Spiel beenden." }

$failed = 0
foreach ($state in Get-ChildItem (Join-Path $Mods "_state") -Filter *.applied -ErrorAction SilentlyContinue) {
    $pack = $state.BaseName
    foreach ($line in Get-Content $state.FullName) {
        if ($line.Length -lt 3 -or $line.StartsWith("#")) { continue }
        $rel = $line.Substring(2)
        $dest = Join-Path $Data $rel
        if ($line[0] -eq '1') {
            $bk = Join-Path (Join-Path (Join-Path $Mods "_backup") $pack) $rel
            if (Test-Path $bk) { Copy-Item $bk $dest -Force } else { Write-Host "missing backup: $bk" -ForegroundColor Yellow; $failed++ }
        } else {
            Remove-Item $dest -ErrorAction SilentlyContinue
        }
    }
    Remove-Item $state.FullName
    Write-Host "restored: $pack"
}

if (Test-Path "$Bin\d3d9.dll.communitypatch") {
    Copy-Item "$Bin\d3d9.dll.communitypatch" "$Bin\d3d9.dll" -Force
    Remove-Item "$Bin\d3d9.dll.communitypatch"
    Write-Host "previous d3d9.dll restored"
}

if (Test-Path "$Bin\S2DNG.exe.laa") {
    # clear the large-address-aware flag again (restores the original S2DNG.exe byte for byte)
    $exe = [IO.File]::ReadAllBytes("$Bin\S2DNG.exe")
    $o = [BitConverter]::ToInt32($exe, 0x3C) + 4 + 18
    $exe[$o] = $exe[$o] -band 0xDF
    [IO.File]::WriteAllBytes("$Bin\S2DNG.exe", $exe)
    Remove-Item "$Bin\S2DNG.exe.laa"
    Write-Host "S2DNG.exe: 4 GB flag removed"
}

if ($Purge -and $failed -eq 0 -and (Test-Path $Mods)) { Remove-Item $Mods -Recurse -Force; Write-Host "mods folder removed" }
if ($failed) { Write-Host "$failed file(s) could not be restored - see above" -ForegroundColor Yellow }
else { Write-Host "Done / Fertig." -ForegroundColor Cyan }
