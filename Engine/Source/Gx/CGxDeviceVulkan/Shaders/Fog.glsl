// SXC: the volumetric fog's shared block and medium (Vulkan/Fog.cpp, Fog.comp, Fog.frag).
//
// Two ways to draw it: per pixel (Fog.frag marches each pixel's own ray to its depth: thin layers on
// cliffs and around the feet stay sharp) or from the froxel volume (Fog.comp fills 160 x 90 columns of
// slices by the distance from the camera, Fog.frag reads its pixel's texel: cheap, soft, too coarse
// across the screen for layers thinner than a few yards).
//
// Positions are relative to `origin` (the camera), in the world's frame: +x north, +y west, +z up.
// Matrices are the engine's row-vector ones read as column-major (so M * v is the row-vector v * M).

layout(std140, set = 0, binding = 0) uniform FogParams
{
    mat4 invViewProj;  // clip (D3D: y up, z 0..1) -> camera-relative position
    vec4 origin;       // xyz the camera's world position, w seconds
    vec4 viewport;     // x, y, width, height in target pixels
    vec4 depth;        // the viewport's minDepth, maxDepth; the volume's near, far (yd)
    vec4 colour;       // rgb the fog's colour (lit), a the step length (yd)
    vec4 field;        // the near field: cell size (yd), cells per side, first cell i, first cell j
    vec4 noise;        // amount 0..1, tile (yd), how far the patches have drifted: x, y (yd, summed frame by frame)
    vec4 misc;         // density scale, debug view (0 off, 1 magenta), field present (1/0), haze density (per yd at the ground)
    vec4 haze;         // haze: base height (world z), thickness (yd); the field's colours present (1/0), x their brightness
    vec4 march;        // x 1 = march per pixel (0 = the froxel volume), y the step's growth with distance, z the most steps
} fp;

layout(set = 0, binding = 1) uniform sampler2D fieldLight; // per cell: density (1/yd), layer floor (world z), thickness (yd), patchiness
layout(set = 0, binding = 2) uniform sampler2D fieldPool;  // per cell: density (1/yd), pool top (world z), soft top (yd), unused
layout(set = 0, binding = 6) uniform sampler2D fieldTint;  // per cell: the fog's colour (rgb), unused

const int kSlices = 128;

// The distance (yd) of slice edge e (0 .. kSlices).
float SliceEdge(float e)
{
    return fp.depth.z * pow(fp.depth.w / fp.depth.z, e / float(kSlices));
}

// The slice coordinate (0 .. kSlices) of a distance.
float SliceOf(float d)
{
    return log(max(d, fp.depth.z) / fp.depth.z) / log(fp.depth.w / fp.depth.z) * float(kSlices);
}

// A camera-relative position from clip x, y (D3D: y up) and depth z 0..1.
vec3 Unproject(vec2 ndc, float z)
{
    vec4 p = fp.invViewProj * vec4(ndc, z, 1.0);
    return p.xyz / p.w;
}

// --- value noise ------------------------------------------------------------------------------------
float Hash(vec3 p)
{
    p = fract(p * 0.3183099 + vec3(0.71, 0.113, 0.419));
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float Noise(vec3 x)
{
    vec3 i = floor(x);
    vec3 f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(Hash(i + vec3(0, 0, 0)), Hash(i + vec3(1, 0, 0)), f.x),
                   mix(Hash(i + vec3(0, 1, 0)), Hash(i + vec3(1, 1, 0)), f.x), f.y),
               mix(mix(Hash(i + vec3(0, 0, 1)), Hash(i + vec3(1, 0, 1)), f.x),
                   mix(Hash(i + vec3(0, 1, 1)), Hash(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}

// 0..1, mean ~0.5: two octaves, the second smaller.
float Patches(vec3 world)
{
    vec3 drift = vec3(fp.noise.z, fp.noise.w, 0.0);
    vec3 p = (world - drift) / fp.noise.y * 8.0;
    return 0.65 * Noise(p * vec3(1.0, 1.0, 2.0)) + 0.35 * Noise(p * vec3(2.3, 2.3, 4.0) + vec3(5.2, 1.3, 0.0) - drift.yxz * 0.05);
}

// --- the layers' profiles, averaged exactly over a step ---------------------------------------------
// A step's density is the layer's exact average over the step's height range above its floor (the
// floor taken at both ends), as WXL's march did: one point per step hit a thin layer or missed it,
// rings around the camera along the terrain's contours.

// The light layer's factor exp(-max(h, 0) / T) (h = height above its floor: full below it, falling off
// above) averaged over h from ha to hb. Its antiderivative: h below the floor, T (1 - exp(-h / T)) above.
float LayerIntegral(float h, float T) { return h < 0.0 ? h : T * (1.0 - exp(-h / T)); }

float LayerAverage(float ha, float hb, float T)
{
    if (abs(hb - ha) < 1e-3)
    {
        return exp(-max(0.5 * (ha + hb), 0.0) / T);
    }

    return (LayerIntegral(hb, T) - LayerIntegral(ha, T)) / (hb - ha);
}

// The pool's factor clamp(u, 0, 1) (u = (top - z) / soft + 0.5) averaged over u from ua to ub. Its
// antiderivative: 0, u^2 / 2, u - 1/2.
float RampIntegral(float u) { u = max(u, 0.0); return u < 1.0 ? 0.5 * u * u : u - 0.5; }

float RampAverage(float ua, float ub)
{
    if (abs(ub - ua) < 1e-3)
    {
        return clamp(0.5 * (ua + ub), 0.0, 1.0);
    }

    return (RampIntegral(ub) - RampIntegral(ua)) / (ub - ua);
}

// --- the near field at a point ------------------------------------------------------------------------

struct Field
{
    vec4 light;  // density, floor, thickness, patchiness (density x reach)
    vec4 pool;   // density, top, soft top (density x reach)
    vec3 tint;
};

// How much of the near field a world position sees: 1 inside, fading to 0 over the last 8 cells of
// the window (beyond it the texels hold other places).
float FieldReach(vec2 world)
{
    float cells = fp.field.y;
    vec2 cell = world / fp.field.x - fp.field.zw; // 0 .. cells inside the window
    vec2 edge = min(cell, vec2(cells) - cell);
    return clamp(min(edge.x, edge.y) / 8.0, 0.0, 1.0);
}

Field SampleField(vec2 xy)
{
    Field f;
    f.light = vec4(0.0, 0.0, 1.0, 0.0);
    f.pool = vec4(0.0, -1.0e6, 1.0, 0.0);
    f.tint = fp.colour.rgb;

    if (fp.misc.z < 0.5)
    {
        return f;
    }

    float reach = FieldReach(xy);

    if (reach <= 0.0)
    {
        return f;
    }

    // The textures hold the field toroidally (cell i at i mod size): repeat addressing finds any cell.
    vec2 uv = xy / (fp.field.x * fp.field.y);
    f.light = textureLod(fieldLight, uv, 0.0);
    f.pool = textureLod(fieldPool, uv, 0.0);
    f.light.x *= reach;
    f.pool.x *= reach;

    if (fp.haze.z > 0.5)
    {
        f.tint = textureLod(fieldTint, uv, 0.0).rgb * fp.haze.w;
    }

    return f;
}

// The medium over the step from a to b (their field samples fa, fb): rgb the light it scatters per yd,
// a its extinction per yd, both the step's average.
vec4 StepMedium(vec3 a, vec3 b, Field fa, Field fb)
{
    // The light layer: its density at both ends, the height above its floor going linearly from one
    // end's to the other's, averaged exactly; broken up by the noise at the step's middle.
    float light = 0.0;
    float da = fa.light.x, db = fb.light.x;

    if (da > 0.0 || db > 0.0)
    {
        float thickness = max(0.5 * (fa.light.z + fb.light.z), 0.25);
        float profile = LayerAverage(a.z - fa.light.y, b.z - fb.light.y, thickness);
        float patchy = 0.5 * (fa.light.w + fb.light.w) * fp.noise.x;
        float n = Patches(0.5 * (a + b));
        light = 0.5 * (da + db) * profile * clamp(1.0 - patchy + 2.0 * patchy * n, 0.0, 2.0);
    }

    // Pools: below their flat top, a soft band under it.
    float pool = 0.0;
    float pa = fa.pool.x, pb = fb.pool.x;

    if (pa > 0.0 || pb > 0.0)
    {
        float ua = (fa.pool.y - a.z) / max(fa.pool.z, 0.05) + 0.5;
        float ub = (fb.pool.y - b.z) / max(fb.pool.z, 0.05) + 0.5;
        pool = 0.5 * (pa + pb) * RampAverage(ua, ub);
    }

    // The haze: everywhere, thinning above its base (the same profile), in the look's colour.
    float haze = 0.0;

    if (fp.misc.w > 0.0)
    {
        haze = fp.misc.w * LayerAverage(a.z - fp.haze.x, b.z - fp.haze.x, max(fp.haze.y, 1.0));
    }

    float field = light + pool;
    float sigma = (field + haze) * fp.misc.x;
    vec3 tint = 0.5 * (fa.tint + fb.tint);
    vec3 colour = field + haze > 0.0 ? (field * tint + haze * fp.colour.rgb) / (field + haze) : fp.colour.rgb;
    return vec4(colour * sigma, sigma);
}
