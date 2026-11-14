//
// Community Pack: anti-aliasing (FXAA) and ambient occlusion (SSAO) for the post-processing combine pass.
// Live settings (community_live.inc, F10 menu / [Graphics] in bin\d3d9.ini):
//   CP_FXAA_ON, CP_SSAO_ON (0/1), CP_SSAO_STRENGTH, CP_SSAO_RADIUS, CP_DOF_STRENGTH (used by postprocess.fx)
//
// Texture1 = scene colour, Texture2 = linear view depth (R32F, written by the scene shaders, bound by the DLL).
//

#include "community_live.inc"

// Depth of field: the engine never bound the depth target for this pass, so the original game blurred the whole
// picture evenly (depth = 0). The DLL binds it now (for SSAO), but alpha-blended objects (flags, particles, decals)
// blend their depth into it, which gives blurred patches with real depth. The depth of field keeps the even haze.
#define CP_DOF_DEPTH(_d)	0.0

// size of one scene pixel in texture coordinates (falls back to stage 0 if the engine leaves stage 1 empty)
float2 CP_PixelSize()
{
	float2 p = TextureParameter[1].zw;
	return (p.x > 0.0 && p.y > 0.0) ? p : TextureParameter[0].zw;
}

float CP_Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

// FXAA (edge-directed blur along the local gradient, after T. Lottes' FXAA PC)
float3 CP_Fxaa(float2 uv, float2 px)
{
	float3 rgbM  = tex2D(Texture1, uv).rgb;
	float  lumNW = CP_Luma(tex2D(Texture1, uv + float2(-0.5, -0.5) * px).rgb);
	float  lumNE = CP_Luma(tex2D(Texture1, uv + float2( 0.5, -0.5) * px).rgb);
	float  lumSW = CP_Luma(tex2D(Texture1, uv + float2(-0.5,  0.5) * px).rgb);
	float  lumSE = CP_Luma(tex2D(Texture1, uv + float2( 0.5,  0.5) * px).rgb);
	float  lumM  = CP_Luma(rgbM);

	float lumMin = min(lumM, min(min(lumNW, lumNE), min(lumSW, lumSE)));
	float lumMax = max(lumM, max(max(lumNW, lumNE), max(lumSW, lumSE)));

	float2 dir;
	dir.x = -((lumNW + lumNE) - (lumSW + lumSE));
	dir.y =  ((lumNW + lumSW) - (lumNE + lumSE));
	float dirReduce = max((lumNW + lumNE + lumSW + lumSE) * (0.25 / 8.0), 1.0 / 128.0);
	float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
	dir = clamp(dir * rcpDirMin, -8.0, 8.0) * px;

	float3 rgbA = 0.5 * (tex2D(Texture1, uv + dir * (1.0 / 3.0 - 0.5)).rgb +
	                     tex2D(Texture1, uv + dir * (2.0 / 3.0 - 0.5)).rgb);
	float3 rgbB = rgbA * 0.5 + 0.25 * (tex2D(Texture1, uv - dir * 0.5).rgb +
	                                   tex2D(Texture1, uv + dir * 0.5).rgb);
	float lumB = CP_Luma(rgbB);
	return (lumB < lumMin || lumB > lumMax) ? rgbA : rgbB;
}

// depth-only SSAO (Texture2 = linear view depth, bound by the DLL for this pass).
// Samples come in pairs mirrored around the pixel: on a flat or sloped surface the two depths average out to the
// pixel's own depth (no occlusion), in a corner (house wall on the ground, trunk on the terrain) the pair is closer
// on average. Occluders far in front (a tree over distant ground) fade out. The kernel is rotated per pixel in a 2x2
// pattern, which supersampling averages into 4x the directions (without supersampling the pattern stays constant).
static const float2 CP_AOKernel[6] =
{
	{ 0.300, 0.000 }, { 0.736, 0.425 }, { 0.275, 0.476 },
	{ 0.000, 1.000 }, {-0.200, 0.346 }, {-0.606, 0.350 },
};

float CP_Ssao(float2 uv, float2 px)
{
	float d = tex2D(Texture2, uv).r;
	// radius shrinks with distance: about 24 px at view depth 100, 6..48 px
	float  r      = clamp(2400.0 * CP_SSAO_RADIUS / d, 6.0, 48.0);
	float  bias   = d * 0.002;
	float  range  = d * 0.08;
	float2 q      = step(0.5, frac(uv / px));          // 2x2 pattern of the supersampled pixels
	float  sn, cs;
	sincos((q.x * 0.5 + q.y * 0.25) * 3.14159265, sn, cs);
	float  occ    = 0.0;
	for (int i = 0; i < 6; i++)
	{
		float2 k  = CP_AOKernel[i];
		float2 o  = float2(k.x * cs - k.y * sn, k.x * sn + k.y * cs) * px * r;
		float  a  = d - tex2D(Texture2, uv + o).r;
		float  b  = d - tex2D(Texture2, uv - o).r;
		float  c  = saturate((0.5 * (a + b) - bias) / (range * 0.25));   // concave: the pair is closer on average
		occ += c * saturate(2.0 - max(a, b) / range);                    // fade occluders far in front
	}
	float valid = (d > 0.0 && d < 5000.0) ? 1.0 : 0.0;   // sky / nothing rendered: no occlusion
	return 1.0 - valid * CP_SSAO_STRENGTH * occ / 6.0;
}

float3 CP_SceneColor(float2 uv)
{
	float3 c = tex2D(Texture1, uv).rgb;
	return lerp(c, CP_Fxaa(uv, CP_PixelSize()), CP_FXAA_ON);
}

// bloom: the copy pass stores how far the brightest of its 9 scene samples lies above the threshold in alpha (the
// original read the depth target there and nothing used the value); the blur passes soften it, the combine pass
// adds the blurred bright areas. sum3 = r + g + b of that sample.
float CP_BloomMask(float sum3)
{
	float l = sum3 / 3.0;
	return saturate((l - CP_BLOOM_THRESHOLD) / max(1.0 - CP_BLOOM_THRESHOLD, 0.01));
}

float3 CP_Bloom(float4 blur)
{
	return blur.rgb * blur.a * CP_BLOOM_STRENGTH;
}

float CP_AmbientOcclusion(float2 uv)
{
	return lerp(1.0, CP_Ssao(uv, CP_PixelSize()), CP_SSAO_ON);
}
