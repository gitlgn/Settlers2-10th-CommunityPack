//
// Community Pack: anti-aliasing (FXAA) and ambient occlusion (SSAO) for the post-processing combine pass.
// Settings come from community_settings.inc, written by the patch DLL from [Graphics] in bin\d3d9.ini:
//   CP_FXAA (0/1), CP_SSAO (0/1), CP_SSAO_STRENGTH, CP_SSAO_RADIUS
//
// Texture1 = scene colour, Texture2 = linear view depth (R32F/R16F, written by every scene shader).
//

#include "community_settings.inc"

#ifndef CP_FXAA
#define CP_FXAA 0
#endif
#ifndef CP_SSAO
#define CP_SSAO 0
#endif
#ifndef CP_SSAO_STRENGTH
#define CP_SSAO_STRENGTH 0.6
#endif
#ifndef CP_SSAO_RADIUS
#define CP_SSAO_RADIUS 1.0
#endif

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

// depth-only SSAO: neighbours that are clearly closer to the camera (but not far in front) darken the pixel
static const float2 CP_AOKernel[8] =
{
	{ -0.326, -0.406 }, { -0.840, -0.074 }, { -0.696,  0.457 }, { -0.203,  0.621 },
	{  0.962, -0.195 }, {  0.473, -0.480 }, {  0.519,  0.767 }, {  0.185, -0.893 },
};

float CP_Ssao(float2 uv, float2 px)
{
	float d = tex2D(Texture2, uv).r;
	// radius shrinks with distance: about 24 px at view depth 100, 6..48 px
	float  r      = clamp(2400.0 * CP_SSAO_RADIUS / d, 6.0, 48.0);
	float  bias   = d * 0.004;
	float  range  = d * 0.08;
	float  occ    = 0.0;
	for (int i = 0; i < 8; i++)
	{
		float s    = tex2D(Texture2, uv + CP_AOKernel[i] * px * r).r;
		float diff = d - s - bias;
		occ += (diff > 0.0) ? saturate(1.0 - diff / range) : 0.0;
	}
	float valid = (d > 0.0 && d < 5000.0) ? 1.0 : 0.0;   // sky / nothing rendered: no occlusion
	return 1.0 - valid * CP_SSAO_STRENGTH * occ / 8.0;
}

float3 CP_SceneColor(float2 uv)
{
#if CP_FXAA
	return CP_Fxaa(uv, CP_PixelSize());
#else
	return tex2D(Texture1, uv).rgb;
#endif
}

float CP_AmbientOcclusion(float2 uv)
{
#if CP_SSAO
	return CP_Ssao(uv, CP_PixelSize());
#else
	return 1.0;
#endif
}
