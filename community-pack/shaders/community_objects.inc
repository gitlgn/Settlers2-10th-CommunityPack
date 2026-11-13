//
// Community Pack: sky / ground ambient light for objects (houses, trees, rocks, settlers, ...), included by
// shared.inc right before CalcLight. Live settings (community_live.inc, F10 menu / [Graphics] in bin\d3d9.ini):
//   CP_OBJECTS_ON (0/1, ImprovedObjects), CP_UNIT_RIM / CP_UNIT_RIM_VS (0 - 1, UnitRimLight)
//
// The objects are lit per vertex. The vertex shaders store how much the surface faces up in the otherwise unused
// alpha channel of their light colour (CP_SET_UP); CalcLight then mixes a slightly bluer sky light for surfaces
// facing up and a warm bounce from the ground for walls and undersides - the same model as the improved terrain.
//

#include "community_live.inc"

#define CP_SET_UP(_c, _n)	_c.a = 0.5 + 0.5 * normalize(_n).y

// rim light for settlers and animals: a soft highlight on the edges that turn away from the camera, so small
// figures stand out from the ground when seen from above
float3 CP_RimLight(float3 N, float3 V, float strength)
{
	float r = 1.0 - saturate(dot(N, V));
	return (strength * r * r * r) * float3(0.95, 0.97, 1.0);
}

float3 CP_UnitRim(float3 N, float3 V)		// per pixel (settlers): N, V normalised
{
	return CP_RimLight(N, V, CP_UNIT_RIM);
}

float3 CP_UnitRimVS(float3 N, float3 worldPos)	// per vertex (animals, donkeys)
{
	return CP_RimLight(normalize(N), normalize(g_CameraPosition.xyz - worldPos), CP_UNIT_RIM_VS);
}

// up: 1 = facing up, 0.5 = vertical wall, 0 = facing down
float3 CP_ObjectAmbient(float up)
{
	float3 sky = float3(1.06, 1.09, 1.16);
	float3 bnc = float3(0.90, 0.84, 0.74);
	return g_Ambient.rgb * lerp(float3(1, 1, 1), lerp(bnc, sky, saturate(up)), CP_OBJECTS_ON);
}
