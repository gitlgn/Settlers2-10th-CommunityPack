// Replaces the pre-transformed (XYZRHW) HUD vertices: virtual UI pixels -> clip space of the real back buffer.
// c255 = ( 2*sx/W, -2*sy/H, 1/W - 1, 1 - 1/H )   (includes the D3D9 half-pixel offset)
// c254 = material diffuse (used when the vertices carry no colour)
// c253 = texture coordinate scale for upscaled HUD textures (the game divides by the real texture size)
// variants: default = position+colour+uv (textured HUD, fonts)
//           -DNO_UV   = position+colour     (lines, untextured frames)
//           -DPOS_ONLY = position only       (lines coloured by the material)
float4 g_UIScale     : register(c255);
float4 g_MatDiffuse  : register(c254);
float4 g_UVScale     : register(c253);   // xy: real/original size of an upscaled HUD texture (1 = original)

#ifdef POS_ONLY
#define NO_UV
#endif

struct VIn {
    float4 pos : POSITION;
#ifndef POS_ONLY
    float4 col : COLOR0;
#endif
#ifndef NO_UV
    float2 uv  : TEXCOORD0;
#endif
};
struct VOut {
    float4 pos : POSITION;
    float4 col : COLOR0;
#ifndef NO_UV
    float2 uv  : TEXCOORD0;
#endif
};

VOut main(VIn i)
{
    VOut o;
    o.pos = float4(i.pos.x * g_UIScale.x + g_UIScale.z,
                   i.pos.y * g_UIScale.y + g_UIScale.w,
                   i.pos.z, 1.0);
#ifdef POS_ONLY
    o.col = g_MatDiffuse;
#else
    o.col = i.col;
#endif
#ifndef NO_UV
    o.uv  = i.uv * g_UVScale.xy;
#endif
    return o;
}
