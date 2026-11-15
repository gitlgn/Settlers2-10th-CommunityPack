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

# Game folder below Program Files: the game runs without admin rights, so Windows redirects everything it writes into
# the game folder (pack switching at game start, engine settings, the shader settings file) to the user's VirtualStore
# and prefers those copies on every read. Stale copies would hide the files installed now: they are removed (the game
# writes what it needs again at the next start). bin\ (d3d9.ini settings, logs) stays.
$VirtualStore = Join-Path $env:LOCALAPPDATA ("VirtualStore" + $GameDir.Substring(2))
foreach ($sub in "data", "mods") {
    $vs = Join-Path $VirtualStore $sub
    if (Test-Path $vs) {
        $n = @(Get-ChildItem $vs -Recurse -File -ErrorAction SilentlyContinue).Count
        Remove-Item $vs -Recurse -Force
        Say "VirtualStore: $n stale file(s) in $sub removed"
    }
}

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

# merge the template: missing sections are appended, missing keys (with their comment lines) are added
# to the end of an existing section - values the player already set are never touched
$lines = [System.Collections.Generic.List[string]]((Get-Content "$Bin\d3d9.ini") -as [string[]])
$template = Get-Content (Join-Path $PackDir "d3d9.ini.sections") -Raw
$added = 0
foreach ($section in ($template -split '(?m)^(?=\[)') | Where-Object { $_.Trim() }) {
    $tl = $section.Trim() -split "\r?\n"
    $name = $tl[0].Trim()
    $start = -1
    for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i].Trim() -ieq $name) { $start = $i; break } }
    if ($start -lt 0) {
        $lines.Add(""); foreach ($l in $tl) { $lines.Add($l) }; $added++
        continue
    }
    $end = $lines.Count
    for ($i = $start + 1; $i -lt $lines.Count; $i++) { if ($lines[$i].TrimStart().StartsWith("[")) { $end = $i; break } }
    $present = @{}
    for ($i = $start + 1; $i -lt $end; $i++) { if ($lines[$i] -match '^\s*([^;=\s][^=]*?)\s*=') { $present[$Matches[1].ToLower()] = $true } }
    $comments = @()
    foreach ($l in $tl[1..($tl.Count - 1)]) {
        if ($l -match '^\s*;') { $comments += $l; continue }
        if ($l -match '^\s*([^;=\s][^=]*?)\s*=') {
            if (-not $present.ContainsKey($Matches[1].ToLower())) {
                while ($end -gt $start + 1 -and -not $lines[$end - 1].Trim()) { $end-- }   # before trailing blank lines
                foreach ($x in $comments + $l) { $lines.Insert($end, $x); $end++ }
                $added++
            }
        }
        $comments = @()
    }
}
Set-Content "$Bin\d3d9.ini" $lines
Say "d3d9.ini: [Mods], [UI], [Graphics] and [Misc] settings present ($added added)" Green

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

function Replace-Regex1([string]$text, [string]$pattern, [string]$replacement, [string]$what) {
    $re = [regex]::new($pattern)
    if (-not $re.IsMatch($text)) { throw "unexpected file content ($what) - unsupported game version?" }
    return $re.Replace($text, $replacement, 1)
}

# improved terrain lighting (community_terrain.inc, switches [Graphics] ImprovedTerrain / TerrainDetail); Shader Model 3
function Add-TerrainImprovements([string]$t) {
    $t = Replace-Regex1 $t '(#include "shared\.inc")' ('$1' + "`r`n" + '#include "community_terrain.inc"') "terrain include"
    $t = Replace-Regex1 $t '(D \*= calculateShadow\(IN\.shadow\);\s*\}\s*)D\s*\+= g_Ambient\.rgb;' ('$1' + "D`t+= CP_TerrainAmbient( N );`t// community pack") "terrain ambient"
    $t = Replace-Regex1 $t '(float4\s+Tex\s*=\s*tex2D\( Texture0, IN\.texcoord0\.xy \);)' ('$1' + "`r`n`tTex.rgb`t= CP_TerrainDetail( Tex.rgb, IN.texcoord0.xy );`t// community pack") "terrain detail"
    return $t.Replace("compile vs_2_0 TerrainVertexShader", "compile vs_3_0 TerrainVertexShader").Replace("compile ps_2_b TerrainPixelShader", "compile ps_3_0 TerrainPixelShader")
}

# improved water surface (community_water.inc, switch [Graphics] ImprovedWater); Shader Model 3
function Add-WaterImprovements([string]$t) {
    $t = Replace-Regex1 $t '(#include "shared\.inc")' ('$1' + "`r`n" + '#include "community_water.inc"') "water include"
    $t = Replace-Regex1 $t 'float3\s+N0\s*=\s*2\.0 \* tex2D\( Texture0, IN\.texcoord0 \) - 1\.0;\s*float3\s+N1\s*=\s*2\.0 \* tex2D\( Texture1, IN\.texcoord1 \) - 1\.0;\s*float3\s+N\s*=\s*0\.5 \* \(N0\.xzy \+ N1\.xzy \);' `
        ("float3`tN`t`t`t= CP_WaterNormal( IN.texcoord0, IN.texcoord1 );`t// community pack`r`n`tfloat`tshoreCP`t`t= bUseWorldMap ? tex2D( WorldTex, IN.worldCoord ).b : 0;") "water normal"
    $t = Replace-Regex1 $t '(float\s+s\s*=\s*g_ReflectionFactor \* D\.r;[^\r\n]*)' ('$1' + "`r`n`ts`t= CP_WaterReflection( s, V, N );`t// community pack") "water fresnel"
    $t = Replace-Regex1 $t '(float3\s+C\s*=\s*lerp\( g_WaveColor, g_WaterColor, N\.y \);)' ('$1' + "`r`n`t`tC`t= CP_WaterColor( C, shoreCP );`t// community pack") "water colour"
    $t = Replace-Regex1 $t 'result\.rgb\s*=\s*D \* lerp\( g_WaveColor, g_WaterColor, N\.y \);' "result.rgb`t`t= D * CP_WaterColor( lerp( g_WaveColor, g_WaterColor, N.y ), shoreCP );" "water colour 2"
    $t = Replace-Regex1 $t 'float3 halfVec\s*=\s*normalize\( normalize\(L\) \+ V \);\s*float specIntensity = pow\( dot\( halfVec, N \), 16 \);\s*float\s+S\s*=\s*saturate\( specIntensity \);' `
        "float`tS`t`t`t= CP_WaterSpecular( L, V, N );`t// community pack" "water specular"
    $t = Replace-Regex1 $t '(\s*// fog of war texture\s*\r?\n\s*if \( bUseWorldMap \))' `
        ("`r`n`t// community pack: foam at the shore`r`n`tfloat`tfoamCP`t`t= CP_WaterFoam( shoreCP, N );`r`n" +
         "`tresult.rgb`t= lerp( result.rgb, float3(0.95, 0.97, 1.0) * saturate(D), foamCP );`r`n`tresult.a`t= max( result.a, foamCP );`r`n" + '$1') "water foam"
    return $t.Replace("compile vs_2_0 Water_VS_2_0", "compile vs_3_0 Water_VS_2_0").Replace("compile ps_2_b Water_PS_2_0", "compile ps_3_0 Water_PS_2_0")
}

# sky/ground ambient light for objects (community_objects.inc, switch [Graphics] ImprovedObjects): the vertex shaders
# store how much the surface faces up in the unused alpha of their light colour, CalcLight uses it
function Add-ObjectSharedInc([string]$t) {
    $t = Replace-Regex1 $t '\n(float4 CalcLight\s*\()' ("`n#include `"community_objects.inc`"`r`n`r`n" + '$1') "shared.inc objects include"
    $a = $t.IndexOf("float4 CalcLight"); $b = $t.IndexOf("return float4(diffuse,1);", $a)
    if ($a -lt 0 -or $b -lt 0) { throw "unexpected file content (shared.inc CalcLight) - unsupported game version?" }
    $body = $t.Substring($a, $b - $a)
    if (([regex]::Matches($body, [regex]::Escape("diffuse = g_Ambient.rgb +"))).Count -ne 3) { throw "unexpected file content (shared.inc CalcLight ambient) - unsupported game version?" }
    return $t.Substring(0, $a) + $body.Replace("diffuse = g_Ambient.rgb +", "diffuse = CP_ObjectAmbient(_IN_color.a) +") + $t.Substring($b)
}
function Add-ObjectLighting([string]$t) {
    return [regex]::Replace($t, '(OUT\.color\s*=\s*saturate\(dot\(([\w.]+),-g_MainLight\.xyz\)\) \* g_LightColor(?: \* IN\.color)?;)',
        ('$1' + "`r`n`tCP_SET_UP( OUT.color, " + '$2' + " );`t// community pack"))
}

# rim light for settlers (per pixel) and animals / donkeys (per vertex), [Graphics] UnitRimLight
function Add-UnitRim([string]$name, [string]$t) {
    if ($name -eq "skin_color_mesh.fx") {
        return Replace-Regex1 $t '(Color\.rgb\s*=\s*saturate\( Color\.rgb \+ S \* IdxMap\.b\);)' ('$1' + "`r`n`tColor.rgb`t`t`t= saturate( Color.rgb + CP_UnitRim( N, V ) );`t// community pack") "settler rim light"
    }
    if ($name -eq "animals.fx" -or $name -eq "donkey.fx") {
        return Replace-Regex1 $t '(CP_SET_UP\( OUT\.color, N \);[^\r\n]*)' ('$1' + "`r`n`tOUT.color.rgb`t+= CP_UnitRimVS( N, position.xyz );`t// community pack") "animal rim light"
    }
    return $t
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
    "// community pack: depth of field strength (1.0 = original), [Graphics] DOFStrength via community_settings.inc`r`n" +
    "#ifdef CP_DOF_STRENGTH`r`n#define DOF_STRENGTH`tCP_DOF_STRENGTH`r`n#else`r`n#define DOF_STRENGTH`t0.6`r`n#endif") "postprocess.fx"
$t = Replace-Checked $t "`t`tresult += tex2D( Texture0, blurCoord );" "`t`tresult += tex2D( Texture0, blurCoord ) * g_BlurWeights[i];" "postprocess.fx blur"
$t = Replace-Checked $t "`treturn result * RCP_BLUR_SAMPLES;" "`treturn result;" "postprocess.fx blur return"
$t = Replace-Checked $t "(depth.r  - g_CamFocus.x) * g_CamFocus.y)" "(CP_DOF_DEPTH(depth.r) - g_CamFocus.x) * g_CamFocus.y * DOF_STRENGTH)" "postprocess.fx dof"
# FXAA + SSAO in the combine pass (switches: [Graphics] FXAA / SSAO, written to community_settings.inc by the DLL)
$t = Replace-Checked $t "#define ADDRESS_MODE_V0 `tClamp" ("#define ADDRESS_MODE_V0 `tClamp`r`n#define ADDRESS_MODE_U1 `tClamp`r`n#define ADDRESS_MODE_V1 `tClamp`r`n" +
    "#define ADDRESS_MODE_U2 `tClamp`r`n#define ADDRESS_MODE_V2 `tClamp`r`n#define FILTER_MODE_2 `tPoint") "postprocess.fx samplers"
$t = Replace-Checked $t "#include `"shared.inc`"" "#include `"shared.inc`"`r`n#include `"community_postfx.inc`"" "postprocess.fx include"
$t = Replace-Checked $t "`tfloat4`tcolor`t= tex2D( Texture1, IN.texcoord1 );`t// normal scene" ("`tfloat4`tcolor`t= tex2D( Texture1, IN.texcoord1 );`t// normal scene`r`n" +
    "`tcolor.rgb`t= CP_SceneColor( IN.texcoord1 );`t// community pack: FXAA") "postprocess.fx fxaa"
$t = Replace-Checked $t "`treturn lerp( blur, color, dof.xxxx );" ("`tfloat4`tresult`t= lerp( blur, color, dof.xxxx );`r`n" +
    "`tresult.rgb`t*= CP_AmbientOcclusion( IN.texcoord1 );`t// community pack: SSAO`r`n" +
    "`tresult.rgb`t+= CP_Bloom( blur );`t// community pack: bloom`r`n`treturn result;") "postprocess.fx ssao"
# bloom: the copy pass measured its brightness mask from the depth target (stage 1 there) and nothing used it;
# now it comes from the scene image and the combine pass adds the blurred bright areas ([Graphics] Bloom)
$t = Replace-Checked $t "`t`tfloat4`tlumSample`t= tex2D( Texture1, texTap );" "`t`tfloat4`tlumSample`t= tex2D( Texture0, texTap );`t// community pack: brightness from the scene image" "postprocess.fx bloom source"
$t = Replace-Checked $t "`tcolor.a`t`t= saturate( L - bloomParameter.y );" "`tcolor.a`t`t= CP_BloomMask( L );`t// community pack: bloom mask" "postprocess.fx bloom mask"
$t = Replace-Checked $t "compile ps_2_0 ColorAndLuminance" "compile ps_2_b ColorAndLuminance" "postprocess.fx profile"
Write-PackFile "postfx" "gfx\fx_20\postprocess.fx" $t
Write-PackFile "postfx" "gfx\fx_20\community_postfx.inc" ([IO.File]::ReadAllText((Join-Path $PackDir "shaders\community_postfx.inc")))
Write-PackFile "postfx" "gfx\fx_20\community_live.inc" ([IO.File]::ReadAllText((Join-Path $PackDir "shaders\community_live.inc")))

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
$shared = Add-ObjectSharedInc $shared
Write-PackFile "smooth_shadows" "gfx\fx_20\shared.inc" $shared
$n = 0
foreach ($f in Get-ChildItem (Join-Path $Data "gfx\fx_20") -Filter *.fx) {
    if ($f.Name -eq "postprocess.fx") { continue }
    $t = [IO.File]::ReadAllText((Get-Plain ("gfx\fx_20\" + $f.Name)))
    if ($t -notmatch '#include\s+"shared.inc"') { continue }
    $t = [regex]::Replace($t, '(#include\s+"shared.inc")', "#define SMOOTH_SHADOW8`r`n`$1")
    $t = $t.Replace("compile ps_2_0", "compile ps_2_b")
    $t = Add-ObjectLighting $t
    $t = Add-UnitRim $f.Name $t
    if ($f.Name -eq "terrain_shader.fx") { $t = Add-TerrainImprovements $t }
    if ($f.Name -eq "water_shader.fx") { $t = Add-WaterImprovements $t }
    Write-PackFile "smooth_shadows" ("gfx\fx_20\" + $f.Name) $t
    $n++
}
Write-PackFile "smooth_shadows" "gfx\fx_20\community_live.inc" ([IO.File]::ReadAllText((Join-Path $PackDir "shaders\community_live.inc")))
Write-PackFile "smooth_shadows" "gfx\fx_20\community_objects.inc" ([IO.File]::ReadAllText((Join-Path $PackDir "shaders\community_objects.inc")))
Write-PackFile "smooth_shadows" "gfx\fx_20\community_terrain.inc" ([IO.File]::ReadAllText((Join-Path $PackDir "shaders\community_terrain.inc")))
Write-PackFile "smooth_shadows" "gfx\fx_20\community_water.inc" ([IO.File]::ReadAllText((Join-Path $PackDir "shaders\community_water.inc")))
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
