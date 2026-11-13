//
// Community Pack: improved water surface (ps_3_0), included by water_shader.fx.
// Live setting (community_live.inc, F10 menu / [Graphics] ImprovedWater): CP_WATER_ON (0/1)
//
// Tex0 / Tex1 = wave normal maps, Tex2 = mirror (reflection) map, WorldTex.b = relative water depth
// (0 deep ... 1 at the shore), WorldTex.a = fog of war.
//

#include "community_live.inc"

// wave normal from four layers (the original uses two): two extra scales break up the visible tiling
float3 CP_WaterNormal(float2 tc0, float2 tc1)
{
	float3 N0 = 2.0 * tex2D(Texture0, tc0).rgb - 1.0;
	float3 N1 = 2.0 * tex2D(Texture1, tc1).rgb - 1.0;
	float3 N2 = 2.0 * tex2D(Texture0, tc1 * 0.43 + float2(0.37, 0.11)).rgb - 1.0;
	float3 N3 = 2.0 * tex2D(Texture1, tc0 * 2.17 + float2(0.13, 0.71)).rgb - 1.0;
	float3 orig = 0.5 * (N0.xzy + N1.xzy);
	float3 four = normalize(N0.xzy * 0.35 + N1.xzy * 0.35 + N2.xzy * 0.18 + N3.xzy * 0.12);
	return lerp(orig, four, CP_WATER_ON);
}

// reflection amount: Fresnel term on top of the game's own factor (more mirror at grazing angles, more water
// colour when looking straight down)
float CP_WaterReflection(float s, float3 V, float3 N)
{
	float c = saturate(dot(V, N));
	float f = 0.02 + 0.98 * pow(1.0 - c, 5.0);
	return s * lerp(1.0, lerp(0.55, 1.15, saturate(f * 3.0)), CP_WATER_ON);
}

// water colour: lighter turquoise towards the shore, the game's colour in deep water
float3 CP_WaterColor(float3 C, float shore)
{
	return lerp(C, C * float3(0.85, 1.12, 1.05) + float3(0.02, 0.06, 0.04), saturate(shore * 1.4 - 0.3) * 0.6 * CP_WATER_ON);
}

// sun glint: the original broad highlight plus a sharp one
float CP_WaterSpecular(float3 L, float3 V, float3 N)
{
	float3 H  = normalize(normalize(L) + V);
	float  nh = saturate(dot(H, N));
	float  o  = pow(nh, 16);
	return saturate(lerp(o, o * 0.6 + pow(nh, 160) * 1.4, CP_WATER_ON));
}

// thin foam line where the water meets the shore, broken up by the waves
float CP_WaterFoam(float shore, float3 N)
{
	float edge  = saturate((shore - 0.82) * 6.0);
	float waves = saturate(0.5 + N.x * 2.0 + N.z * 1.5);
	return edge * waves * 0.55 * CP_WATER_ON;
}
