//
// Community Pack: improved terrain lighting (ps_3_0), included by terrain_shader.fx.
// Settings come from community_settings.inc, written by the patch DLL from [Graphics] in bin\d3d9.ini:
//   CP_TERRAIN (0/1), CP_TERRAIN_DETAIL (0.0 - 1.0)
//
// Tex0 = diffuse colour of the ground pattern; TextureParameter[0].zw = 1 / texture size.
//

#include "community_settings.inc"

#ifndef CP_TERRAIN
#define CP_TERRAIN 0
#endif
#ifndef CP_TERRAIN_DETAIL
#define CP_TERRAIN_DETAIL 0.5
#endif

// sky / ground ambient: surfaces facing up get a little more (and slightly bluer) light than slopes, which get
// a warm bounce from the ground - the relief reads better than with one flat ambient colour.
// N = per-pixel normal from the normal map (tangent space, z = away from the surface)
float3 CP_TerrainAmbient(float3 N)
{
#if CP_TERRAIN
	float  up   = saturate(N.z * 0.5 + 0.5);
	float3 sky  = float3(1.04, 1.08, 1.16);
	float3 bnc  = float3(0.86, 0.80, 0.70);
	return g_Ambient.rgb * lerp(bnc, sky, up);
#else
	return g_Ambient.rgb;
#endif
}

float CP_TerrainLuma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

// detail layer for close zoom: the pattern itself again at 4x frequency, only as brightness variation around its
// local average. Fades out where its texels get smaller than a screen pixel (no shimmer when zoomed out).
float3 CP_TerrainDetail(float3 color, float2 uv)
{
#if CP_TERRAIN
	float2 duv     = uv * 4.0;
	float2 texels  = fwidth(duv) / TextureParameter[0].zw;          // detail texels per screen pixel
	float  fade    = saturate(1.6 - max(texels.x, texels.y));
	float  d      = CP_TerrainLuma(tex2D(Texture0, duv).rgb);
	float  avg     = CP_TerrainLuma(tex2Dlod(Texture0, float4(duv, 0, 6)).rgb);
	float  k       = 1.0 + (d - avg) * (2.0 * CP_TERRAIN_DETAIL) * fade;
	return color * clamp(k, 0.6, 1.4);
#else
	return color;
#endif
}
