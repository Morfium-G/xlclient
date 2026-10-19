// SXC: the volumetric fog over the scene: the pixel's distance from its depth, the volume's texel
// there (trilinear), blended as dst x transmittance + scattered light (blend ONE, SRC_ALPHA).
#version 450
#extension GL_GOOGLE_include_directive : require

#include "Fog.glsl"

layout(set = 0, binding = 4) uniform sampler2D sceneDepth;
layout(set = 0, binding = 5) uniform sampler3D volume;

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 oC0;

void main()
{
    vec2 pixel = gl_FragCoord.xy;
    vec2 uv = (pixel - fp.viewport.xy) / fp.viewport.zw;
    float d = texelFetch(sceneDepth, ivec2(pixel), 0).r;

    // The sky (nothing drawn, or the far plane): the fog's own far edge.
    float distance = fp.depth.w;

    if (d < fp.depth.y * 0.99999)
    {
        float z = (d - fp.depth.x) / max(fp.depth.y - fp.depth.x, 1e-6);
        vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
        distance = min(length(Unproject(ndc, z)), fp.depth.w);
    }

    // Texel k holds the fog up to slice edge k + 1; closer than the first edge, a share of it.
    float s = SliceOf(distance);
    float first = 1.0;

    if (s < 1.0)
    {
        first = distance / SliceEdge(1.0);
        s = 1.0;
    }

    vec4 fog = textureLod(volume, vec3(uv, (s - 0.5) / float(kSlices)), 0.0);
    fog.rgb *= first;
    fog.a = mix(1.0, fog.a, first);

    if (fp.misc.y > 0.5)
    {
        fog.rgb = vec3(1.0, 0.0, 1.0) * (1.0 - fog.a);
    }

    oC0 = vec4(fog.rgb, fog.a);
}
