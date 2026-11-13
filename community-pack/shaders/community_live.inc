//
// Community Pack: settings that can change while the game runs (F10 menu).
// The values live in fixed shader constant registers. The defaults below are uploaded by the effect framework;
// the patch DLL recognises them by the marker in .w and replaces .xyz with the current values from bin\d3d9.ini /
// the in-game menu. Switches are factors 0 / 1, so every code path stays compiled in.
//

#ifndef CP_LIVE_INC
#define CP_LIVE_INC

float4 cp_LiveA : register(c28) = { 1.0, 1.0, 0.6, 7771.0 };    // FXAA on, SSAO on, SSAO strength
float4 cp_LiveB : register(c29) = { 1.0, 0.3, 1.0, 7772.0 };    // SSAO radius, depth-of-field strength, terrain on
float4 cp_LiveC : register(c30) = { 0.5, 1.0, 1.0, 7773.0 };    // terrain detail, water on, objects on
float4 cp_LiveD : register(c31) = { 0.5, 0.0, 0.0, 7774.0 };    // rim light (pixel shaders)
float4 cp_LiveV : register(c249) = { 0.5, 0.0, 0.0, 7775.0 };   // rim light (vertex shaders)

#define CP_FXAA_ON          cp_LiveA.x
#define CP_SSAO_ON          cp_LiveA.y
#define CP_SSAO_STRENGTH    cp_LiveA.z
#define CP_SSAO_RADIUS      cp_LiveB.x
#define CP_DOF_STRENGTH     cp_LiveB.y
#define CP_TERRAIN_ON       cp_LiveB.z
#define CP_TERRAIN_DETAIL   cp_LiveC.x
#define CP_WATER_ON         cp_LiveC.y
#define CP_OBJECTS_ON       cp_LiveC.z
#define CP_UNIT_RIM         cp_LiveD.x
#define CP_UNIT_RIM_VS      cp_LiveV.x

#endif
