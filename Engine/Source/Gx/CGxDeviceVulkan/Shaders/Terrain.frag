// SXC: our terrain pixel shader (Vulkan/Pipelines: FragmentProgram::Terrain), in place of the client's
// "Terrain1" records (no shadow map: 32) and "Terrain3" records (shadow map modes 1-3: 96). One
// source, compiled once per layer count (LAYERS 1..4) and shadow group (SHADOW 0 none, 1 one cascade,
// 2 three cascades and the far one), because a module must have every sampler it declares bound; the
// finer options are specialization constants. The device sets them from the record the engine binds:
//   Terrain1:n  n = 16 unlit + 8 bigAlpha + 2 (layers - 1) + cube
//   Terrain3:n  n = 48 unlit + 24 bigAlpha + 8 (mode - 1) + 2 (layers - 1) + cube
// (CMap::GetTerrainPixelShader). Records with a cube layer stay the stock ones.
//
// Samplers: s0..s(LAYERS-1) the layers, s(LAYERS) the alpha map (rgb the layers' weights, a the baked
// shadow), s5..s8 the shadow cascades. Pixel registers: p1 the layers' lighting (0 = unlit), p2 the fog
// colour, p3..p10 the shadow taps' offsets. The math follows the stock records operation by operation
// (5a: the stock look, unchanged); the covers (5b, TerrainCover.glsl) change the layers' colour and
// gloss before the light, and add a wet shine, only while a mod turns them on.
#version 450
#extension GL_GOOGLE_include_directive : require
#if COVER_BINDLESS
#extension GL_EXT_nonuniform_qualifier : require
#endif

#ifndef LAYERS
#define LAYERS 1
#endif
#ifndef SHADOW
#define SHADOW 0
#endif

layout(constant_id = 0) const bool kBigAlpha = false; // layer 0 takes what the others leave (else lerped in turn)
layout(constant_id = 1) const bool kUnlit    = false; // layers may opt out of the diffuse light (p1 per layer)
layout(constant_id = 2) const bool kNineTaps = false; // shadow mode 3: the first cascade with nine taps, the cascades chained
layout(constant_id = 3) const bool kUnderlay = false; // the lowered underlay of a raised chunk: not the covers' surface

layout(location = 0) in vec4 color0;   // diffuse light
layout(location = 1) in vec4 color1;   // the sun's highlight
layout(location = 2) in vec4 texcoord0;
layout(location = 3) in vec4 texcoord1;
layout(location = 4) in vec4 texcoord2;
layout(location = 5) in vec4 texcoord3;
layout(location = 6) in vec4 texcoord4;
layout(location = 7) in vec4 texcoord5;
layout(location = 8) in vec4 texcoord6;
layout(location = 10) in float fog0;

layout(location = 0) out vec4 oC0;

layout(set = 0, binding = 0) uniform sampler2D s0;
#if LAYERS >= 2
layout(set = 0, binding = 1) uniform sampler2D s1;
#endif
#if LAYERS >= 3
layout(set = 0, binding = 2) uniform sampler2D s2;
#endif
#if LAYERS >= 4
layout(set = 0, binding = 3) uniform sampler2D s3;
#endif
layout(set = 0, binding = LAYERS) uniform sampler2D sAlpha;
#if SHADOW >= 1
layout(set = 0, binding = 5) uniform sampler2D s5;
#endif
#if SHADOW >= 2
layout(set = 0, binding = 6) uniform sampler2D s6;
layout(set = 0, binding = 7) uniform sampler2D s7;
layout(set = 0, binding = 8) uniform sampler2D s8;
#endif

layout(std140, set = 1, binding = 0) uniform Regs {
    vec4 p[16];
    vec4 misc;   // misc.x = alphaRef / 255 (0 = off)
    vec4 v[64];
} regs;

#include "TerrainCover.glsl"

// Coordinate k (the vertex shader's packing): one per varying, or two per varying with the cascades.
vec2 Coord(int k)
{
    vec4 tc[5] = vec4[5](texcoord0, texcoord1, texcoord2, texcoord3, texcoord4);

    if (SHADOW == 0 || k == 0)
    {
        return tc[k].xy;
    }

    return (k & 1) == 1 ? tc[(k + 1) / 2].xy : tc[k / 2].wz;
}

// A layer's colour; with unlit layers, the diffuse light applied here per layer unless p1 says unlit.
vec4 Layer(vec4 c, float lighting, vec3 twice)
{
    if (kUnlit)
    {
        vec3 lit = c.xyz * twice;
        c.xyz = abs(lighting) <= 0.0 ? c.xyz : lit;
    }

    return c;
}

#if SHADOW >= 1
// One cascade's lit share: the centre and the taps (offsets p3, p5, p7, p9; or p3..p10 with nine),
// each 1 where the shadow map's depth is at or beyond the pixel's, summed.
float Taps(sampler2D map, vec2 coord, float depth, bool nine)
{
    vec2 uv = coord * 0.5 + 0.5;
    float sum = textureLod(map, uv, 0.0).x - depth >= 0.0 ? 1.0 : 0.0;

    for (int i = 3; i <= 10; i++)
    {
        if (!nine && (i & 1) == 0)
        {
            continue;
        }

        sum += textureLod(map, uv + regs.p[i].xy, 0.0).x - depth >= 0.0 ? 1.0 : 0.0;
    }

    return sum;
}
#endif

void main()
{
    vec3 twice = color0.xyz + color0.xyz;
    vec4 alpha = texture(sAlpha, Coord(LAYERS));
    vec4 c = Layer(texture(s0, Coord(0)), regs.p[1].x, twice);

    // The layers: lerped in turn by the alpha map's channels, or weighted with layer 0 taking the rest.
    if (kBigAlpha)
    {
        float rest = 1.0 - clamp(dot(alpha.xyz, vec3(1.0)), 0.0, 1.0);
        c = c * rest;
#if LAYERS >= 2
        c = Layer(texture(s1, Coord(1)), regs.p[1].y, twice) * alpha.x + c;
#endif
#if LAYERS >= 3
        c = Layer(texture(s2, Coord(2)), regs.p[1].z, twice) * alpha.y + c;
#endif
#if LAYERS >= 4
        c = Layer(texture(s3, Coord(3)), regs.p[1].w, twice) * alpha.z + c;
#endif
    }
    else
    {
#if LAYERS >= 2
        c = mix(c, Layer(texture(s1, Coord(1)), regs.p[1].y, twice), alpha.x);
#endif
#if LAYERS >= 3
        c = mix(c, Layer(texture(s2, Coord(2)), regs.p[1].z, twice), alpha.y);
#endif
#if LAYERS >= 4
        c = mix(c, Layer(texture(s3, Coord(3)), regs.p[1].w, twice), alpha.z);
#endif
    }

    // SXC: the covers over the layers (painted, fallen snow, wetness); unchanged while they're off.
    CoverResult cov = Covers(c, CoverLayerWeights(alpha, kBigAlpha), twice, !kUnlit);
    c = cov.c;

    // SXC: the covers' surface here into the surface grid, and this pixel's depth for the projected
    // textures (step 5e; the underlay lies below the surface).
    if (!kUnderlay && cover.enable.x > 0.5)
    {
        CoverWriteSurface(coverWorld.xyz, coverTint.w);
        CoverWriteDepth(coverWorld.z - coverTint.w > 0.005);
    }

    // The shadow: the alpha map's baked one, or the cascades'.
    float shadow = alpha.w;

#if SHADOW == 1
    // Mode 1: one cascade fading out toward its edge, never brighter than the baked shadow.
    float fade = clamp(max(abs(texcoord4.x), abs(texcoord4.y)) * -3.4482758 + 3.41379309, 0.0, 1.0);

    if (fade > 0.0)
    {
        float lit = Taps(s5, texcoord4.xy, texcoord4.z, false) * 0.2 - 1.0;
        shadow = min(fade * lit + 1.0, alpha.w);
    }
#elif SHADOW == 2
    // The far cascade: its coordinates in the w of the three, fading out toward its edge.
    float fade0 = clamp(max(abs(texcoord4.x), abs(texcoord4.y)) * -3.4482758 + 3.41379309, 0.0, 1.0);
    float rest;

    if (max(abs(texcoord5.x), abs(texcoord5.y)) < 1.0)
    {
        rest = Taps(s6, texcoord5.xy, texcoord5.z, false) * 0.2;
    }
    else if (max(abs(texcoord6.x), abs(texcoord6.y)) < 1.0)
    {
        rest = Taps(s7, texcoord6.xy, texcoord6.z, false) * 0.2;
    }
    else
    {
        vec2 far = vec2(texcoord4.w, texcoord5.w);
        float farFade = clamp(max(abs(far.x), abs(far.y)) * -11.1111107 + 11.0, 0.0, 1.0);
        rest = farFade * (Taps(s8, far, texcoord6.w, false) * 0.2 - 1.0) + 1.0;
    }

    if (kNineTaps)
    {
        // Mode 3: the nearest cascade that holds the pixel, the first one with nine taps, unfaded.
        shadow = fade0 > 0.0 ? Taps(s5, texcoord4.xy, texcoord4.z, true) * 0.111111112 : rest;
    }
    else
    {
        // Mode 2: the first cascade fading out, and the next one holding the pixel; the darker.
        float first = fade0 > 0.0 ? fade0 * (Taps(s5, texcoord4.xy, texcoord4.z, false) * 0.2 - 1.0) + 1.0 : 1.0;
        shadow = min(rest, first);
    }
#endif

    // Lit: the layers x (the shadow's 0.7..1) x twice the diffuse light, plus the highlight in the
    // layers' alpha (their gloss) where unshadowed. Unlit layers carry their light already.
    vec3 colour;
    float shade = shadow * 0.3 + 0.7;
    // SXC: the wet shine, on top of the stock highlight (zero while the covers are off or it's dry).
    vec3 shine = regs.v[27].xyz * CoverWetShine(cov.wet, c.w, color1.w);

    if (kUnlit)
    {
        vec3 highlight = c.www * color1.xyz + shine;
        highlight = shadow * highlight;
        colour = c.xyz * shade + highlight;
    }
    else
    {
        colour = c.xyz * shade;
        colour = colour * color0.xyz;
        colour = colour + colour;
        vec3 highlight = c.www * color1.xyz + shine;
        colour = highlight * shadow + colour;
    }

    // SXC: the covers' debug views, unlit and unfogged (7, the decals', leaves the terrain as it is).
    if (cover.enable.x > 0.5 && cover.enable.y > 0.5 && cover.enable.y < 6.5)
    {
        oC0 = vec4(CoverDebug(cov, int(cover.enable.y + 0.5)), 1.0);
        return;
    }

    // The fog: toward its colour by the vertex fog factor.
    colour = colour + -regs.p[2].xyz;
    oC0 = vec4(fog0 * colour + regs.p[2].xyz, 1.0);

    // The alpha test (GxRs_AlphaRef), as the translated modules do it.
    if (regs.misc.x > 0.0 && oC0.a < regs.misc.x) discard;
}
