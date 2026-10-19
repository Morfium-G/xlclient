// SXC: the volumetric fog's shared block (Vulkan/Fog.cpp, Fog.comp, Fog.frag).
//
// The froxel volume covers the world viewport: x, y across it (texel centres), z in slices by the
// distance from the camera, spaced exponentially from `near` to `far`. Texel k of a column holds the
// fog from the camera to the far edge of slice k: rgb the light it scatters toward the camera, a the
// transmittance (1 = clear).
//
// Positions are relative to `origin` (the camera), in the world's frame: +x north, +y west, +z up.
// Matrices are the engine's row-vector ones read as column-major (so M * v is the row-vector v * M).

layout(std140, set = 0, binding = 0) uniform FogParams
{
    mat4 invViewProj;  // clip (D3D: y up, z 0..1) -> camera-relative position
    vec4 origin;       // xyz the camera's world position, w seconds (drift)
    vec4 viewport;     // x, y, width, height in target pixels
    vec4 depth;        // the viewport's minDepth, maxDepth; the volume's near, far (yd)
    vec4 colour;       // rgb the fog's colour (lit), a the ambient share in the shadowed parts (unused yet)
    vec4 field;        // the near field: cell size (yd), cells per side, first cell i, first cell j
    vec4 noise;        // amount 0..1, tile (yd), how far the patches have drifted: x, y (yd, summed frame by frame)
    vec4 misc;         // density scale, debug view (0 off, 1 magenta), field present (1/0), haze density (per yd at the ground)
    vec4 haze;         // haze: base height (world z), thickness (yd), unused, unused
} fp;

layout(set = 0, binding = 1) uniform sampler2D fieldLight; // per cell: density (1/yd), layer base (world z), thickness (yd), patchiness
layout(set = 0, binding = 2) uniform sampler2D fieldPool;  // per cell: density (1/yd), pool top (world z), soft top (yd), unused

const int kSlices = 64;

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
