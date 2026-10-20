// SXC: surface covers in our terrain shader (included by Terrain.frag; Vulkan/Cover.hpp, SXC/Api/
// sxc_cover.h). Per spot the covers stack over the stock layers: the painted cover (each layer's entry
// where that layer is painted, from the chunk's alpha map), fallen snow on top, the ground showing
// through where they're thin; then wetness darkens what shows and makes it shine.
//
// Pixel registers (set per chunk by SXC::Cover::BeforeChunkDraw): p11 each layer's painted entry, p12
// each layer's fallen-snow look (entry indices as floats, 0 = none). Set 2: the parameters and entries,
// the near and far maps (fallen snow, painted snow's kept share, moisture above rest, the water surface),
// the cover textures. The look follows WXL's SurfaceCover (Shape, the noises, the slope limit, the melt).

layout(std140, set = 2, binding = 0) uniform CoverBlock {
    vec4 enable;      // on, debug view, wet darkening, plain brightness
    vec4 nearMap;     // cell (yd), size, first i, first j
    vec4 farMap;
    vec4 outside;     // beyond the far map: fallen snow, kept share, moisture above rest; near map present
    vec4 tuning;      // thin depth (yd), 1 / texture tile (1/yd), depth scale, far map present
    vec4 tuning2;     // breakup scale, drift scale, fallen snow on, entries
    vec4 texInfo[256]; // texture k + 1 at [k / 4][k % 4]: its slot x 4 (0 = none: drawn plain), + 2 its alpha is a gloss mask
    vec4 entries[127 * 4];
} cover;

layout(set = 2, binding = 1) uniform sampler2D sCoverNear;
layout(set = 2, binding = 2) uniform sampler2D sCoverFar;

// The cover textures: one array indexed per pixel (COVER_BINDLESS: a Vulkan 1.2 device with non-uniform
// indexing), else 16 slots the client gives to the textures in view.
#if COVER_BINDLESS
layout(set = 2, binding = 3) uniform sampler2D sCoverTex[1024];
#else
layout(set = 2, binding = 3) uniform sampler2D sCoverTex1;
layout(set = 2, binding = 4) uniform sampler2D sCoverTex2;
layout(set = 2, binding = 5) uniform sampler2D sCoverTex3;
layout(set = 2, binding = 6) uniform sampler2D sCoverTex4;
layout(set = 2, binding = 7) uniform sampler2D sCoverTex5;
layout(set = 2, binding = 8) uniform sampler2D sCoverTex6;
layout(set = 2, binding = 9) uniform sampler2D sCoverTex7;
layout(set = 2, binding = 10) uniform sampler2D sCoverTex8;
layout(set = 2, binding = 11) uniform sampler2D sCoverTex9;
layout(set = 2, binding = 12) uniform sampler2D sCoverTex10;
layout(set = 2, binding = 13) uniform sampler2D sCoverTex11;
layout(set = 2, binding = 14) uniform sampler2D sCoverTex12;
layout(set = 2, binding = 15) uniform sampler2D sCoverTex13;
layout(set = 2, binding = 16) uniform sampler2D sCoverTex14;
layout(set = 2, binding = 17) uniform sampler2D sCoverTex15;
layout(set = 2, binding = 18) uniform sampler2D sCoverTex16;
#endif

layout(location = 11) in vec4 coverWorld;   // world position, how much the ground faces up
layout(location = 12) in vec4 coverView;    // view position
layout(location = 13) in vec4 coverNormal;  // view normal (not normalised)
layout(location = 14) in vec4 coverTint;    // the factor the vertex colour put into color0

const uint kCoverPreventUnderwater = 1u;
const uint kCoverPreventOnLand     = 2u;
const uint kCoverUseVertexColor    = 4u;
const uint kCoverSnow              = 8u;
const float kShoreFade = 0.3;               // yd under the surface over which "prevent" covers taper

// --- noise (WXL's SurfaceCoverFill: the same hash, so a spot looks the same as it did there) ---------
float CoverHash(ivec2 p, uint seed)
{
    uint h = uint(p.x) * 374761393u + uint(p.y) * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return float((h ^ (h >> 16)) & 0xFFFFu) / 65535.0;
}

float CoverValueNoise(vec2 p, uint seed)
{
    vec2 i = floor(p);
    vec2 f = p - i;
    f = f * f * (3.0 - 2.0 * f);
    ivec2 q = ivec2(i);
    float a = CoverHash(q, seed), b = CoverHash(q + ivec2(1, 0), seed);
    float c = CoverHash(q + ivec2(0, 1), seed), d = CoverHash(q + ivec2(1, 1), seed);
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// Two scales: broad drifts and small lumps (the depth); fine noise for ragged, patchy edges.
float CoverDrift(vec2 p) { return 0.65 * CoverValueNoise(p / 9.0, 0u) + 0.35 * CoverValueNoise(p / 2.5, 0u); }
float CoverEdgeNoise(vec2 p) { return 0.6 * CoverValueNoise(p / 1.6, 3u) + 0.4 * CoverValueNoise(p / 0.7, 4u); }

// A coverage made steeper by the breakup, its threshold moved by the edge noise: the transition breaks
// into patches; fully covered and bare stay so (WXL's Shape).
float CoverShape(float coverage, float breakup, float edge)
{
    float m = (coverage - 0.5) * (1.0 + 2.0 * breakup) + 0.5 + 1.5 * breakup * (edge - 0.5);
    m = clamp(m, 0.0, 1.0);
    return m * m * (3.0 - 2.0 * m);
}

// 1 on ground flatter than (max - fade) degrees, 0 steeper than max, smooth between.
float CoverSlope(float up, float maxSlope, float fade)
{
    float cosMax = cos(radians(clamp(maxSlope, 0.0, 90.0)));
    float cosFull = cos(radians(clamp(maxSlope - fade, 0.0, 90.0)));
    float t = cosFull > cosMax ? (up - cosMax) / (cosFull - cosMax) : (up >= cosMax ? 1.0 : 0.0);
    t = clamp(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// --- the maps ----------------------------------------------------------------------------------------
const float kCoverNoWater = -100000.0;

// A map's value at a world position, and how much it holds there: 1 inside, fading out over its last
// 2 cells (the toroidal texture holds another place beyond them).
vec4 CoverMap(sampler2D map, vec4 m, vec2 xy, out float inside)
{
    vec2 c = xy / m.x;
    vec2 rel = c - m.zw;
    float edge = min(min(rel.x, rel.y), min(m.y - rel.x, m.y - rel.y));
    inside = clamp((edge - 0.5) * 0.5, 0.0, 1.0);
    return textureLod(map, c / m.y, 0.0);
}

// Fallen snow (yd), painted snow's kept share, moisture above rest, the water surface (world z).
vec4 CoverGround(vec2 xy)
{
    vec4 g = vec4(cover.outside.xyz, kCoverNoWater);
    float k;

    if (cover.tuning.w > 0.5)
    {
        vec4 f = CoverMap(sCoverFar, cover.farMap, xy, k);
        g.xyz = mix(g.xyz, f.xyz, k);
    }

    if (cover.outside.w > 0.5)
    {
        vec4 n = CoverMap(sCoverNear, cover.nearMap, xy, k);
        g.xyz = mix(g.xyz, n.xyz, k);
        g.w = k >= 1.0 ? n.w : kCoverNoWater;
    }

    return g;
}

// --- entries and textures ----------------------------------------------------------------------------
vec4 CoverEntry(int e, int row) { return cover.entries[(e - 1) * 4 + row]; }

// Row 3 holds integers (sxc_cover_entry::texture, flags), not floats: read their bits.
int CoverEntryTexture(vec4 e3) { return floatBitsToInt(e3.x); }
uint CoverEntryFlags(vec4 e3) { return floatBitsToUint(e3.y); }

float CoverTexInfo(int t) { return cover.texInfo[(t - 1) >> 2][(t - 1) & 3]; }

// A texture's slot (0: none, drawn plain) and whether its alpha is a gloss mask.
int CoverTexSlot(int t) { return int(CoverTexInfo(t) * 0.25 + 0.01); }
bool CoverTexGloss(int t) { return mod(CoverTexInfo(t), 4.0) >= 2.0; }

// Explicit gradients: the cover textures are read in branches that differ from pixel to pixel.
vec4 CoverTexture(int slot, vec2 uv, vec2 dx, vec2 dy)
{
#if COVER_BINDLESS
    return textureGrad(sCoverTex[nonuniformEXT(slot - 1)], uv, dx, dy);
#else
    switch (slot)
    {
        case 1: return textureGrad(sCoverTex1, uv, dx, dy);
        case 2: return textureGrad(sCoverTex2, uv, dx, dy);
        case 3: return textureGrad(sCoverTex3, uv, dx, dy);
        case 4: return textureGrad(sCoverTex4, uv, dx, dy);
        case 5: return textureGrad(sCoverTex5, uv, dx, dy);
        case 6: return textureGrad(sCoverTex6, uv, dx, dy);
        case 7: return textureGrad(sCoverTex7, uv, dx, dy);
        case 8: return textureGrad(sCoverTex8, uv, dx, dy);
        case 9: return textureGrad(sCoverTex9, uv, dx, dy);
        case 10: return textureGrad(sCoverTex10, uv, dx, dy);
        case 11: return textureGrad(sCoverTex11, uv, dx, dy);
        case 12: return textureGrad(sCoverTex12, uv, dx, dy);
        case 13: return textureGrad(sCoverTex13, uv, dx, dy);
        case 14: return textureGrad(sCoverTex14, uv, dx, dy);
        case 15: return textureGrad(sCoverTex15, uv, dx, dy);
        case 16: return textureGrad(sCoverTex16, uv, dx, dy);
    }

    return vec4(1.0);
#endif
}

// An entry's colour (rgb, as a layer's albedo) and gloss (a): its texture or the plain colour, its tint,
// the vertex colour taken out again unless it uses it.
vec4 CoverLook(int e, vec2 uv, vec2 dx, vec2 dy)
{
    vec4 e1 = CoverEntry(e, 1), e3 = CoverEntry(e, 3);
    int t = CoverEntryTexture(e3);
    uint flags = CoverEntryFlags(e3);
    vec4 look = vec4(vec3(cover.enable.w), 0.0);

    int slot = t >= 1 && t <= 1024 ? CoverTexSlot(t) : 0;

    if (slot >= 1)
    {
        vec4 tex = CoverTexture(slot, uv, dx, dy);
        look = vec4(tex.rgb, CoverTexGloss(t) ? tex.a : 0.0);
    }

    look.rgb = mix(look.rgb, e1.rgb, clamp(e1.w, 0.0, 1.0));

    if ((flags & kCoverUseVertexColor) == 0u)
    {
        look.rgb *= min(vec3(0.5) / max(coverTint.rgb, vec3(0.02)), vec3(4.0));
    }

    return look;
}

// A layer's weight after its water flags: "prevent underwater" thins out below the surface, "prevent on
// land" only exists there.
float CoverWater(uint flags, float under)
{
    float d = under / kShoreFade;
    float f = 1.0;
    if ((flags & kCoverPreventUnderwater) != 0u) f *= 1.0 - clamp(d, 0.0, 1.0);
    if ((flags & kCoverPreventOnLand) != 0u) f *= clamp(d, 0.0, 1.0);
    return f;
}

// --- the stack ---------------------------------------------------------------------------------------
// The layers' weights as the colour blends them: lerped in turn, or (big alpha) layer 0 taking the rest.
vec4 CoverLayerWeights(vec4 alpha, bool bigAlpha)
{
    vec4 w = vec4(1.0, 0.0, 0.0, 0.0);

    if (bigAlpha)
    {
        w = vec4(1.0 - clamp(dot(alpha.xyz, vec3(1.0)), 0.0, 1.0), alpha.xyz);
    }
    else
    {
        for (int k = 1; k < LAYERS; k++)
        {
            w *= 1.0 - alpha[k - 1];
            w[k] = alpha[k - 1];
        }
    }

    for (int k = LAYERS; k < 4; k++) w[k] = 0.0;
    return w;
}

struct CoverResult
{
    vec4  c;        // the colour (rgb) and gloss (a) that show
    float wet;      // 0..1
    float painted;  // the painted cover's share
    float snow;     // fallen snow's share
    float under;    // yd below the water surface
    float inNear;   // debug: how much the near map holds here
    float inFar;
    int   entry;    // debug: the strongest painted entry
};

// c: the stock layers (rgb lit per layer when the chunk has unlit layers, a their gloss); w: the layers'
// weights; twice: twice the diffuse light (what unlit-layer chunks light each layer by).
CoverResult Covers(vec4 c, vec4 w, vec3 twice, bool litLayers)
{
    CoverResult r;
    r.c = c;
    r.wet = 0.0;
    r.painted = 0.0;
    r.snow = 0.0;
    r.under = -1.0e6;
    r.inNear = 0.0;
    r.inFar = 0.0;
    r.entry = 0;

    if (cover.enable.x < 0.5)
    {
        return r;
    }

    vec3 world = coverWorld.xyz;
    float up = coverWorld.w;
    // Tiled like the terrain's layers (8 repeats per chunk, the same orientation), from the chunk's corner.
    vec2 uv = -(world.yx - regs.v[23].yx) * cover.tuning.y;
    vec2 dx = dFdx(uv), dy = dFdy(uv);
    vec4 g = CoverGround(world.xy);
    float edge = CoverEdgeNoise(world.xy);
    float drift = CoverDrift(world.xy);
    int entries = int(cover.tuning2.w + 0.5);
    r.under = g.w - world.z;

    // The painted covers: each layer's entry by how strongly the layer is painted, after its water
    // flags, its moisture need (mud) and, for painted snow, what's left of it (patchy as it melts).
    ivec4 painted = ivec4(regs.p[11] + 0.5);
    float covered = 0.0, maxSlope = 0.0, slopeFade = 0.0, breakup = 0.0, rowWet = 0.0, strongest = 0.0;
    vec4 look = vec4(0.0);

    for (int l = 0; l < LAYERS; l++)
    {
        int e = painted[l];

        if (e <= 0 || e > entries || w[l] <= 0.0)
        {
            continue;
        }

        vec4 e0 = CoverEntry(e, 0), e2 = CoverEntry(e, 2), e3 = CoverEntry(e, 3);
        uint flags = CoverEntryFlags(e3);

        if (e0.x <= 0.0)
        {
            continue;
        }

        float pw = w[l] * CoverWater(flags, r.under);

        if (e2.z <= 1.0)
        {
            pw *= clamp((e2.w + g.z - e2.z) * 10.0, 0.0, 1.0); // full 0.1 above its threshold
        }

        if ((flags & kCoverSnow) != 0u)
        {
            float noise = 0.6 * drift + 0.4 * edge;
            pw *= clamp(g.y * 1.6 - 0.6 * (1.0 - noise), 0.0, 1.0);
        }

        if (pw <= 0.0)
        {
            continue;
        }

        covered += pw;
        maxSlope += pw * e0.y;
        slopeFade += pw * e0.z;
        breakup += pw * e2.x;
        rowWet += pw * e2.y;
        look += pw * CoverLook(e, uv, dx, dy);

        if (pw > strongest)
        {
            strongest = pw;
            r.entry = e;
        }
    }

    vec4 paintedLook = vec4(0.0);

    if (covered > 0.0)
    {
        float inv = 1.0 / covered;
        float b = clamp(breakup * inv * cover.tuning2.x, 0.0, 1.0);
        r.painted = CoverShape(min(covered, 1.0), b, edge) * CoverSlope(up, maxSlope * inv, slopeFade * inv);
        r.painted *= cover.tuning.z > 0.0 ? 1.0 : 0.0;
        paintedLook = look * inv;
        rowWet *= inv;
    }

    // Fallen snow on top: its look from the layers' snow looks by their weights (built in where none),
    // its depth varied by the drifts, thinned on slopes, none below water; it hides what's below from
    // the thin depth on, patchy before.
    vec4 snowLook = vec4(vec3(cover.enable.w), 0.0);

    if (cover.tuning2.z > 0.5 && g.x > 0.0005)
    {
        ivec4 looks = ivec4(regs.p[12] + 0.5);
        float lw = 0.0, sMax = 0.0, sFade = 0.0, sBreak = 0.0, sDrift = 0.0, sWet = 0.0;
        vec4 sLook = vec4(0.0);

        for (int l = 0; l < LAYERS; l++)
        {
            int e = looks[l];

            if (e <= 0 || e > entries || w[l] <= 0.0)
            {
                continue;
            }

            // Layers sharing a look count once.
            bool seen = false;

            for (int k = 0; k < l; k++)
            {
                seen = seen || (looks[k] == e && w[k] > 0.0);
            }

            float wl = 0.0;

            for (int k = l; k < LAYERS; k++)
            {
                wl += looks[k] == e ? w[k] : 0.0;
            }

            if (seen)
            {
                continue;
            }

            vec4 e0 = CoverEntry(e, 0), e2 = CoverEntry(e, 2);
            lw += wl;
            sMax += wl * e0.y;
            sFade += wl * e0.z;
            sDrift += wl * e0.w;
            sBreak += wl * e2.x;
            sWet += wl * e2.y;
            sLook += wl * CoverLook(e, uv, dx, dy);
        }

        float maxS = 50.0, fadeS = 15.0, breakS = 0.5, driftS = 0.35, wetS = 0.0;

        if (lw > 0.0)
        {
            float inv = 1.0 / lw;
            maxS = sMax * inv;
            fadeS = sFade * inv;
            breakS = sBreak * inv;
            driftS = sDrift * inv;
            wetS = sWet * inv;
            snowLook = sLook * inv;
        }

        float driftF = max(1.0 + driftS * cover.tuning2.y * (drift * 2.0 - 1.0), 0.0);
        float dry = 1.0 - clamp(r.under / kShoreFade, 0.0, 1.0);
        float depth = g.x * cover.tuning.z * driftF * CoverSlope(up, maxS, fadeS) * dry;
        r.snow = CoverShape(clamp(depth / cover.tuning.x, 0.0, 1.0), clamp(breakS * cover.tuning2.x, 0.0, 1.0), edge);
        rowWet = mix(rowWet, wetS, r.snow);
    }

    // Covers on chunks with unlit layers carry their light like the layers do.
    if (!litLayers)
    {
        paintedLook.rgb *= twice;
        snowLook.rgb *= twice;
    }

    // The stack: the ground, the painted cover over it, fallen snow over both.
    r.c = mix(r.c, paintedLook, r.painted);
    r.c = mix(r.c, snowLook, r.snow);

    // Wetness: the covers' own wet look where they show, plus the ground's moisture above its rest
    // (snow keeps its own: melt water soaks in below it).
    float shown = max(r.painted, r.snow);
    r.wet = clamp(rowWet * shown + g.z * (1.0 - r.snow), 0.0, 1.0);
    r.c.rgb *= 1.0 - cover.enable.z * r.wet;
    return r;
}

// The wet shine (WXL's cover: a stronger, tighter highlight): per pixel, the sun's Blinn-Phong with the
// exponent sharpened by the wetness, by the gloss made stronger by it. Nothing while the stock highlight
// is off.
float CoverWetShine(float wet, float gloss, float specularOn)
{
    if (wet <= 0.0 || specularOn < 0.5)
    {
        return 0.0;
    }

    vec3 n = normalize(coverNormal.xyz);
    vec3 v = normalize(-coverView.xyz);
    vec3 h = normalize(regs.v[24].xyz + v);
    float e = max(regs.v[27].w, 1.0) * (1.0 + 3.0 * wet);
    float mask = clamp(gloss * (1.0 + 2.0 * wet), 0.0, 1.0) - gloss;
    return pow(max(dot(n, h), 0.0), e) * max(mask, 0.0);
}

// Debug views (the look's debug): 1 what lies where (red bare, green painted, white fallen snow, blue
// water line), 2 wetness, 3 the painted entries (a colour each), 4 the maps' reach (green near, blue far,
// grey beyond).
vec3 CoverDebug(CoverResult r, int view)
{
    if (view == 1)
    {
        vec3 colour = mix(vec3(0.35, 0.05, 0.05), vec3(0.1, 0.9, 0.1), r.painted);
        colour = mix(colour, vec3(1.0), r.snow);
        return r.under > 0.0 ? mix(colour, vec3(0.1, 0.3, 1.0), 0.5) : colour;
    }

    if (view == 2)
    {
        return vec3(0.1, 0.1, 0.1) + vec3(0.0, 0.3, 0.9) * r.wet;
    }

    if (view == 3)
    {
        if (r.entry == 0) return vec3(0.15);
        float hue = fract(float(r.entry) * 0.618034);
        vec3 k = clamp(abs(fract(vec3(hue) + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
        return mix(vec3(0.15), k, max(r.painted, 0.35));
    }

    float k;
    CoverMap(sCoverNear, cover.nearMap, coverWorld.xy, k);
    float near = cover.outside.w > 0.5 ? k : 0.0;
    CoverMap(sCoverFar, cover.farMap, coverWorld.xy, k);
    float far = cover.tuning.w > 0.5 ? k : 0.0;
    return mix(mix(vec3(0.3), vec3(0.1, 0.2, 1.0), far), vec3(0.1, 0.9, 0.1), near);
}
