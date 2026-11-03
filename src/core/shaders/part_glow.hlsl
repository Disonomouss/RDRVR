// [Holsters] InteractionHighlights=parts (run 29 item 2, research/run29/glow.md approach a): the pixel shaders of the gun
// parts' glow twins (pso.cpp). Each is drawn in place of the game's weapon pixel shader, with the game's own vertex shader,
// root signature and bindings, over the part's triangles only, blended onto the lit colour (RT0: SrcBlend BLEND_FACTOR,
// DestBlend ONE), so the part looks as if its own texture glowed. The glow's colour and strength are the blend factor.
//
// Their interfaces are the game's pixel shaders' (read with dxc -dumpbin from the player's own rdr2_weapon.fxc; nothing of
// it is in the repo):
//   PSWeapon     (blob w_036): input SV_Position, POSITION1, TEXCOORD0, TEXCOORD1, TEXCOORD2 (float4 each, registers 0-4);
//                the diffuse g_textures2D[TextureSampler] sampled with g_samplers[TextureSamplerSS] at TEXCOORD1.xy, squared
//   PSWeaponGold (blob w_037): input SV_Position, POSITION1, TEXCOORD0 (float3), TEXCOORD1, TEXCOORD2, TEXCOORD3 (registers
//                0-5); the diffuse at TEXCOORD2.xy
// $Globals is b0 (TextureSampler at offset 68, TextureSamplerSS at 72), the textures and samplers bindless (t0 space2,
// s0 space1, unbounded): only what the game's own pixel shader binds. They write SV_Target0 only (the twin's other targets
// have write mask 0: the motion vectors are the game's).
//
// Built by CMakeLists.txt with the Windows SDK's dxc.exe (signed by the dxil.dll beside it) into the build folder's
// generated/part_glow_ps.h. Shader model 6.0, as the game's.

cbuffer Globals : register(b0) {
    uint TextureSampler : packoffset(c4.y);    // offset 68
    uint TextureSamplerSS : packoffset(c4.z);  // offset 72
};

Texture2D g_textures2D[] : register(t0, space2);
SamplerState g_samplers[] : register(s0, space1);

// a black (or very dark) part still glows: its diffuse squared, at least this
static const float kFloor = 0.04;

float4 glow(float2 uv) {
    const float3 d = g_textures2D[TextureSampler].Sample(g_samplers[TextureSamplerSS], uv).rgb;
    return float4(max(d * d, kFloor.xxx), 1.0);
}

struct InWeapon {
    float4 pos : SV_Position;
    float4 p1 : POSITION1;
    float4 t0 : TEXCOORD0;
    float4 t1 : TEXCOORD1;
    float4 t2 : TEXCOORD2;
};
float4 PSPartGlow(InWeapon i) : SV_Target0 {
    return glow(i.t1.xy);
}

struct InGold {
    float4 pos : SV_Position;
    float4 p1 : POSITION1;
    float3 t0 : TEXCOORD0;
    float4 t1 : TEXCOORD1;
    float4 t2 : TEXCOORD2;
    float4 t3 : TEXCOORD3;
};
float4 PSPartGlowGold(InGold i) : SV_Target0 {
    return glow(i.t2.xy);
}
