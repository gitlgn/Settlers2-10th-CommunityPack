<#
  Settlers II - 10th Anniversary (Die Siedler II - Die naechste Generation): Community Pack installer

  Installs the community d3d9.dll and builds the data packs from YOUR OWN copy of the game.
  No game files are shipped with this pack: shaders, settings and interface textures are decrypted,
  changed and written to <game>\mods\<pack>\ on your computer. The patch DLL switches the packs
  on and off at game start ([Mods] in bin\d3d9.ini).

  Requirements: GOG version 11757 (other builds: packs work, HUD scaling / shadow map size switch off),
  Community Patch 2.3 (https://github.com/S2-modders/Settlers-DNG-Patches/releases) installed first.

  Usage:  install.cmd                    (asks for the optional downloads)
          install.cmd -Yes               (all optional parts, no questions)
          install.cmd -NoHDTextures -NoUIUpscale
#>
param(
    [string]$GameDir,
    [switch]$Yes,
    [switch]$NoHDTextures,
    [switch]$NoUIUpscale
)
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$PackDir  = $PSScriptRoot
$ToolsDir = Join-Path $PackDir "tools"
$GOG_11757_SHA256 = "484A8AFC396DF4AE0E9429B604993DCB5E238F2C0F0FE6D5085AB4620AF548B7"

function Say($msg, $color = "Gray") { Write-Host $msg -ForegroundColor $color }
function Ask($question) {
    if ($Yes) { return $true }
    $a = Read-Host "$question [j/N] (y/N)"
    return $a -match '^(j|y|ja|yes)$'
}

# ------------------------------------------------------------------ checks
if (-not $GameDir) {
    $candidate = Split-Path $PackDir -Parent
    if (Test-Path (Join-Path $candidate "bin\S2DNG.exe")) { $GameDir = $candidate }
}
if (-not $GameDir -or -not (Test-Path (Join-Path $GameDir "bin\S2DNG.exe"))) {
    $GameDir = Read-Host "Spielordner / game folder (contains bin\S2DNG.exe)"
}
$GameDir = (Resolve-Path $GameDir).Path
$Bin = Join-Path $GameDir "bin"
$Data = Join-Path $GameDir "data"
$Mods = Join-Path $GameDir "mods"
if (-not (Test-Path "$Bin\S2DNG.exe")) { throw "S2DNG.exe not found in $Bin" }
if (Get-Process S2DNG -ErrorAction SilentlyContinue) { throw "Please close the game first / Bitte zuerst das Spiel beenden." }

Say "Settlers II 10th Anniversary - Community Pack" Cyan
Say "Game folder: $GameDir"

# PE header flag IMAGE_FILE_LARGE_ADDRESS_AWARE (0x20): lets the 32-bit game use 4 GB instead of 2 GB
function Get-LAAOffset([byte[]]$b) { return [BitConverter]::ToInt32($b, 0x3C) + 4 + 18 }
function Get-ExeHashWithoutLAA([string]$path) {
    $b = [IO.File]::ReadAllBytes($path)
    $o = Get-LAAOffset $b
    $b[$o] = $b[$o] -band 0xDF
    $sha = [Security.Cryptography.SHA256]::Create()
    return ([BitConverter]::ToString($sha.ComputeHash($b)) -replace '-', '')
}

$hash = Get-ExeHashWithoutLAA "$Bin\S2DNG.exe"
if ($hash -ne $GOG_11757_SHA256) {
    Say "Note: S2DNG.exe is not the GOG build 11757. Data packs will work, HUD scaling and the shadow/reflection" Yellow
    Say "      map size switches only support the GOG build and stay off." Yellow
}
if (-not (Test-Path "$Bin\d3d9vk.dll") -or -not (Test-Path "$Bin\d3d9.ini")) {
    throw "Community Patch 2.3 is missing. Install it first: https://github.com/S2-modders/Settlers-DNG-Patches/releases"
}

# ------------------------------------------------------------------ patch DLL + ini
$dllBackup = "$Bin\d3d9.dll.communitypatch"
if ((Test-Path "$Bin\d3d9.dll") -and -not (Test-Path $dllBackup)) { Copy-Item "$Bin\d3d9.dll" $dllBackup }
Copy-Item (Join-Path $PackDir "d3d9.dll") "$Bin\d3d9.dll" -Force
Say "d3d9.dll installed (previous one kept as d3d9.dll.communitypatch)" Green

# 4 GB address space (HD textures, large shadow maps and the Vulkan renderer need more than 2 GB)
$exe = [IO.File]::ReadAllBytes("$Bin\S2DNG.exe")
$o = Get-LAAOffset $exe
if (($exe[$o] -band 0x20) -eq 0) {
    $exe[$o] = $exe[$o] -bor 0x20
    [IO.File]::WriteAllBytes("$Bin\S2DNG.exe", $exe)
    Set-Content "$Bin\S2DNG.exe.laa" "Large-address-aware flag set by the Community Pack; uninstall.cmd clears it again."
    Say "S2DNG.exe: 4 GB memory (large address aware) enabled" Green
} else {
    Say "S2DNG.exe: 4 GB memory already enabled" Green
}

$ini = Get-Content "$Bin\d3d9.ini" -Raw
$template = Get-Content (Join-Path $PackDir "d3d9.ini.sections") -Raw
foreach ($section in ($template -split '(?m)^(?=\[)') | Where-Object { $_.Trim() }) {
    $name = ($section -split "`n")[0].Trim()
    if ($ini -notmatch [regex]::Escape($name)) { $ini = $ini.TrimEnd() + "`r`n`r`n" + $section.Trim() + "`r`n" }
}
Set-Content "$Bin\d3d9.ini" $ini -NoNewline
Say "d3d9.ini: [Mods], [UI] and [Graphics] sections present" Green

# ------------------------------------------------------------------ helpers
$Work = Join-Path $Mods "_build"
if (Test-Path $Work) { Remove-Item $Work -Recurse -Force }
New-Item -ItemType Directory -Force $Work, $Mods | Out-Null

# run an external tool; its console/stderr output is not an error (Windows PowerShell 5.1 treats stderr as one)
function Invoke-Tool([string]$exe, [string[]]$arguments) {
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { & $exe @arguments 2>&1 | Out-Null } finally { $ErrorActionPreference = $old }
}
# original version of a data file (packs that are already applied keep the original in mods\_backup)
function Get-Original([string]$rel) {
    foreach ($b in Get-ChildItem (Join-Path $Mods "_backup") -Directory -ErrorAction SilentlyContinue) {
        $p = Join-Path $b.FullName $rel
        if (Test-Path $p) { return $p }
    }
    return (Join-Path $Data $rel)
}

function Test-Encrypted([string]$path) {
    $fs = [IO.File]::OpenRead($path)
    try { $b = New-Object byte[] 8; [void]$fs.Read($b, 0, 8) } finally { $fs.Close() }
    return [Text.Encoding]::ASCII.GetString($b, 4, 4) -eq "rc00"
}

# decrypted copy of a data file in the work folder
function Get-Plain([string]$rel) {
    $src = Get-Original $rel
    if (-not (Test-Path $src)) { throw "missing game file: data\$rel" }
    # the decryption key is derived from the file name -> keep name and folder structure
    $dst = Join-Path (Join-Path $Work "src") $rel
    New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
    Copy-Item $src $dst -Force
    if (-not (Test-Encrypted $dst)) { return $dst }
    $out = Join-Path (Split-Path $dst) (([IO.Path]::GetFileNameWithoutExtension($dst)) + ".dng" + [IO.Path]::GetExtension($dst))
    if (Test-Path $out) { Remove-Item $out -Force }
    Invoke-Tool (Join-Path $ToolsDir "decryptor_s2.exe") @($dst)
    if (-not (Test-Path $out)) { throw "decryption failed: data\$rel" }
    return $out
}

function Write-PackFile([string]$pack, [string]$rel, [string]$text) {
    $dst = Join-Path (Join-Path $Mods $pack) $rel
    New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
    [IO.File]::WriteAllText($dst, $text)
}

function Replace-Checked([string]$text, [string]$old, [string]$new, [string]$what) {
    if (-not $text.Contains($old)) { throw "unexpected file content ($what) - unsupported game version?" }
    return $text.Replace($old, $new)
}

function Reset-Pack([string]$pack) {
    $p = Join-Path $Mods $pack
    if (Test-Path $p) { Remove-Item $p -Recurse -Force }
}

# ------------------------------------------------------------------ settings packs
Say "Building data packs from your game files ..." Cyan

# sharp_shadows: full precision (R32F) shadow/depth map
Reset-Pack "sharp_shadows"
$t = [IO.File]::ReadAllText((Get-Plain "settings\engine.xml"))
$t = Replace-Checked $t 'lowresDepthMap="true"' 'lowresDepthMap="false"' "engine.xml"
Write-PackFile "sharp_shadows" "settings\engine.xml" $t

# lod_fix: view distance / LOD values from the S2-modders 4k texture project
Reset-Pack "lod_fix"
$t = [IO.File]::ReadAllText((Get-Plain "settings\graphics.xml"))
$t = Replace-Checked $t 'farPlane="500.0"' 'farPlane="10000.0"' "graphics.xml farPlane"
$t = Replace-Checked $t 'lodDistance="50.0"' 'lodDistance="1000.0"' "graphics.xml lodDistance"
$t = Replace-Checked $t 'maxIndices="20000"' 'maxIndices="100000"' "graphics.xml maxIndices"
Write-PackFile "lod_fix" "settings\graphics.xml" $t

# postfx: gaussian bloom/blur weights, softer depth of field
Reset-Pack "postfx"
$t = [IO.File]::ReadAllText((Get-Plain "gfx\fx_20\postprocess.fx"))
$t = Replace-Checked $t "#define RCP_BLUR_SAMPLES`t1 / NUM_BLUR_SAMPLES" ("#define RCP_BLUR_SAMPLES`t1 / NUM_BLUR_SAMPLES`r`n`r`n" +
    "// community pack: gaussian weights (sum = 1) instead of a box filter`r`n" +
    "static const float g_BlurWeights[9] = { 0.0276, 0.0663, 0.1238, 0.1801, 0.2044, 0.1801, 0.1238, 0.0663, 0.0276 };`r`n`r`n" +
    "// community pack: depth of field strength (1.0 = original)`r`n#define DOF_STRENGTH`t0.6") "postprocess.fx"
$t = Replace-Checked $t "`t`tresult += tex2D( Texture0, blurCoord );" "`t`tresult += tex2D( Texture0, blurCoord ) * g_BlurWeights[i];" "postprocess.fx blur"
$t = Replace-Checked $t "`treturn result * RCP_BLUR_SAMPLES;" "`treturn result;" "postprocess.fx blur return"
$t = Replace-Checked $t "(depth.r  - g_CamFocus.x) * g_CamFocus.y)" "(depth.r  - g_CamFocus.x) * g_CamFocus.y * DOF_STRENGTH)" "postprocess.fx dof"
Write-PackFile "postfx" "gfx\fx_20\postprocess.fx" $t

# smooth_shadows: 3x3 PCF (16 taps) in every shader that receives shadows; needs ps_2_b (any Shader Model 3 GPU)
Reset-Pack "smooth_shadows"
$shared = [IO.File]::ReadAllText((Get-Plain "gfx\fx_20\shared.inc"))
$pcf = @"

// ---- community pack (smooth_shadows) ----
// 3x3 box PCF evaluated with bilinear weights (4x4 taps): soft edges about 3 shadow-map texels wide
float calculateShadow8(const float4 shadow)
{
	float	testDepth	= shadow.z / shadow.w + g_ShadowScale.y;
	float2	shadowXY	= shadow.xy / shadow.w;
	float2	f			= frac(shadowXY * ShadowParameter.xy);

	float4	wx = float4( 1.0 - f.x, 1.0, 1.0, f.x );
	float4	wy = float4( 1.0 - f.y, 1.0, 1.0, f.y );

	float	sum = 0;

	for(int j=0; j < 4; ++j)
	{
		float row = 0;
		for(int i=0; i < 4; ++i)
		{
			float2	texTap	= shadowXY + float2( i - 1, j - 1 ) * ShadowParameter.zw;
			float	depth	= tex2D( ShadowTex, texTap ).r;
			row += wx[i] * ( (testDepth < depth) ? 1 : g_ShadowIntensity );
		}
		sum += wy[j] * row;
	}

	return sum / 9.0;
}

#ifdef SMOOTH_SHADOW8
#define calculateShadow calculateShadow8
#endif
// ---- end smooth_shadows ----

"@
$shared = Replace-Checked $shared "#endif // HW_SHADOW_MAP" ($pcf + "#endif // HW_SHADOW_MAP") "shared.inc"
Write-PackFile "smooth_shadows" "gfx\fx_20\shared.inc" $shared
$n = 0
foreach ($f in Get-ChildItem (Join-Path $Data "gfx\fx_20") -Filter *.fx) {
    if ($f.Name -eq "postprocess.fx") { continue }
    $t = [IO.File]::ReadAllText((Get-Plain ("gfx\fx_20\" + $f.Name)))
    if ($t -notmatch '#include\s+"shared.inc"') { continue }
    $t = [regex]::Replace($t, '(#include\s+"shared.inc")', "#define SMOOTH_SHADOW8`r`n`$1")
    $t = $t.Replace("compile ps_2_0", "compile ps_2_b")
    Write-PackFile "smooth_shadows" ("gfx\fx_20\" + $f.Name) $t
    $n++
}
Say "  settings and shader packs built ($n shaders)" Green

# ------------------------------------------------------------------ optional: HD textures (S2-modders)
function Get-Download([string]$url, [string]$file) {
    $dl = Join-Path $ToolsDir "downloads"
    New-Item -ItemType Directory -Force $dl | Out-Null
    $p = Join-Path $dl $file
    if (-not (Test-Path $p)) { Say "  downloading $url"; Invoke-WebRequest $url -OutFile $p }
    return $p
}

if (-not $NoHDTextures -and (Ask "HD textures from S2-modders/Settlers-II-remakes-4k-textures-dds laden? (~800 MB download)")) {
    Reset-Pack "hd_textures"
    $zip = Get-Download "https://github.com/S2-modders/Settlers-II-remakes-4k-textures-dds/archive/refs/heads/main.zip" "4k-textures-dds.zip"
    $ex = Join-Path $Work "4k"
    Expand-Archive $zip $ex -Force
    $root = Get-ChildItem $ex -Recurse -Directory -Filter "10th Anniversary Vikings" | Select-Object -First 1
    $count = 0
    foreach ($f in Get-ChildItem $root.FullName -Recurse -File -Filter *.dds) {
        $rel = $f.FullName.Substring($root.FullName.Length + 1)
        if (-not (Test-Path (Join-Path $Data $rel))) { continue }   # only textures the game actually has
        $dst = Join-Path (Join-Path $Mods "hd_textures") $rel
        New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
        Copy-Item $f.FullName $dst
        $count++
    }
    Say "  hd_textures: $count textures" Green
}

# ------------------------------------------------------------------ optional: 2x interface (Real-ESRGAN)
if (-not $NoUIUpscale -and (Ask "Interface 2x hochskalieren mit Real-ESRGAN + texconv (~45 MB download, GPU, einige Minuten)?")) {
    Reset-Pack "ui_hd"
    $esrZip = Get-Download "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-windows.zip" "realesrgan.zip"
    $esrDir = Join-Path $ToolsDir "downloads\realesrgan"
    if (-not (Test-Path "$esrDir\realesrgan-ncnn-vulkan.exe")) { Expand-Archive $esrZip $esrDir -Force }
    $texconv = Get-Download "https://github.com/microsoft/DirectXTex/releases/download/may2026/texconv.exe" "texconv.exe"
    $esrgan = "$esrDir\realesrgan-ncnn-vulkan.exe"

    $manifest = @()
    # interface atlases + the localised main-menu logo
    $ui = @(Get-ChildItem (Join-Path $Data "gfx") -Filter "ui*.dds" | ForEach-Object { "gfx\" + $_.Name })
    $ui += @(Get-ChildItem (Join-Path $Data "language") -Directory -ErrorAction SilentlyContinue |
        Where-Object { Test-Path (Join-Path $_.FullName "ui_s2logo.dds") } |
        ForEach-Object { "language\" + $_.Name + "\ui_s2logo.dds" })
    # bitmap fonts: glyph coordinates come from scaleW/scaleH in the .fnt, so a 2x page needs no .fnt change
    # and must NOT be listed in the manifest (no texture-coordinate correction)
    $ui += @(Get-ChildItem (Join-Path $Data "language") -Recurse -File -Filter "*.tga" -ErrorAction SilentlyContinue |
        Where-Object { $_.Directory.Name -eq "fonts" } |
        ForEach-Object { $_.FullName.Substring($Data.TrimEnd('\').Length + 1) })
    $i = 0; $fonts = 0
    foreach ($rel in $ui) {
        $i++
        $f = Get-Item (Join-Path $Data $rel)
        Write-Host ("  [{0}/{1}] {2}" -f $i, $ui.Count, $rel)
        $plain = Get-Plain $rel
        $b = [IO.File]::ReadAllBytes($plain)
        $isFont = $f.Extension -eq ".tga"
        if ($isFont) {
            # uncompressed true-colour (2) or greyscale (3) only; keep the pixel format
            if ($b[2] -eq 2 -and $b[16] -eq 32) { $fmt = "B8G8R8A8_UNORM" }
            elseif ($b[2] -eq 3 -and $b[16] -eq 8) { $fmt = "R8_UNORM" }
            else { Say "    skipped (unsupported TGA format)" Yellow; continue }
            $mips = 1; $ext = "tga"
        } else {
            $h = [BitConverter]::ToInt32($b, 12); $w = [BitConverter]::ToInt32($b, 16); $mips = [BitConverter]::ToInt32($b, 28)
            $fourcc = [Text.Encoding]::ASCII.GetString($b, 84, 4).Trim([char]0)
            switch ($fourcc) { "DXT1" { $fmt = "BC1_UNORM" } "DXT3" { $fmt = "BC2_UNORM" } "DXT5" { $fmt = "BC3_UNORM" } default { $fmt = "B8G8R8A8_UNORM" } }
            $ext = "dds"
        }
        $base = [IO.Path]::GetFileNameWithoutExtension($f.Name)
        $tmp = Join-Path $Work "ui"
        New-Item -ItemType Directory -Force $tmp | Out-Null
        Copy-Item $plain "$tmp\$base.$ext" -Force
        Invoke-Tool $texconv @("-nologo", "-y", "-ft", "png", "-o", $tmp, "$tmp\$base.$ext")
        Invoke-Tool $esrgan @("-i", "$tmp\$base.png", "-o", "$tmp\$base.up.png", "-n", "realesr-animevideov3", "-s", "2")
        if (-not (Test-Path "$tmp\$base.up.png")) { Say "    skipped (upscale failed)" Yellow; continue }
        if ($mips -gt 1) { $mipArg = "0" } else { $mipArg = "1" }
        Invoke-Tool $texconv @("-nologo", "-y", "-ft", $ext, "-f", $fmt, "-m", $mipArg, "-o", $tmp, "$tmp\$base.up.png")
        if (-not (Test-Path "$tmp\$base.up.$ext")) { Say "    skipped (conversion failed)" Yellow; continue }
        $dst = Join-Path $Mods ("ui_hd\" + (Split-Path $rel -Parent))
        New-Item -ItemType Directory -Force $dst | Out-Null
        Move-Item -Force "$tmp\$base.up.$ext" "$dst\$base.$ext"
        if ($isFont) { $fonts++ } else { $manifest += "$rel $w $h" }
    }
    Set-Content (Join-Path $Mods "ui_hd.sizes.txt") $manifest
    Say "  ui_hd: $($manifest.Count) interface textures, $fonts font pages" Green
}

Remove-Item $Work -Recurse -Force
Say ""
Say "Done. Start the game - the packs are applied on the first start (this takes a few seconds once)." Cyan
Say "Fertig. Spiel starten - die Pakete werden beim ersten Start eingespielt (einmalig ein paar Sekunden)." Cyan
Say "Switches / Schalter: bin\d3d9.ini  ->  [Mods], [UI], [Graphics]"
