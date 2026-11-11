//
// Community Pack: sky / ground ambient light for objects (houses, trees, rocks, settlers, ...), included by
// shared.inc right before CalcLight. Setting from community_settings.inc (written by the patch DLL):
//   CP_OBJECTS (0/1) - [Graphics] ImprovedObjects
//
// The objects are lit per vertex. The vertex shaders store how much the surface faces up in the otherwise unused
// alpha channel of their light colour (CP_SET_UP); CalcLight then mixes a slightly bluer sky light for surfaces
// facing up and a warm bounce from the ground for walls and undersides - the same model as the improved terrain.
//

#include "community_settings.inc"

#ifndef CP_OBJECTS
#define CP_OBJECTS 0
#endif

#if CP_OBJECTS
#define CP_SET_UP(_c, _n)	_c.a = 0.5 + 0.5 * normalize(_n).y
#else
#define CP_SET_UP(_c, _n)
#endif

// rim light for settlers and animals ([Graphics] UnitRimLight = strength 0.0 - 1.0): a soft highlight on the edges
// that turn away from the camera, so small figures stand out from the ground when seen from above
#ifndef CP_UNIT_RIM
#define CP_UNIT_RIM 0.0
#endif

float3 CP_UnitRim(float3 N, float3 V)		// per pixel: N, V normalised
{
	float r = 1.0 - saturate(dot(N, V));
	return (CP_UNIT_RIM * r * r * r) * float3(0.95, 0.97, 1.0);
}

float3 CP_UnitRimVS(float3 N, float3 worldPos)	// per vertex (animals, donkeys)
{
	return CP_UnitRim(normalize(N), normalize(g_CameraPosition.xyz - worldPos));
}

// up: 1 = facing up, 0.5 = vertical wall, 0 = facing down
float3 CP_ObjectAmbient(float up)
{
#if CP_OBJECTS
	float3 sky = float3(1.06, 1.09, 1.16);
	float3 bnc = float3(0.90, 0.84, 0.74);
	return g_Ambient.rgb * lerp(bnc, sky, saturate(up));
#else
	return g_Ambient.rgb;
#endif
}
