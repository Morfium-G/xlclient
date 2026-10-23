// SXC: our point lights (port plan step 6; SXC/Lights, Vulkan/Lights), per pixel: the lights listed in
// the pixel's cell of a world grid around the camera. The light follows WXL's (TerrainLights): a ~1 yd
// source falling off like the client's 1 / (0.7 d + 0.03 d^2) on sqrt(d^2 + 1), windowed to 0 at the
// radius ((1 - (d/r)^2)^2 to the light's falloff power), wrapped Lambert ((N.L + 0.5) / 1.5), a spot
// cone; added to the stock light with a soft knee on the brightest channel (the hue kept).
//
// Set 3 of our terrain pipelines.

struct Light
{
    vec4 posInvRadius;    // world position, 1 / radius
    vec4 colorFalloff;    // colour (x brightness, fade, flicker), the window's power
    vec4 spotDirCos;      // where a spot points (world), cos of its outer angle (-2: a point light)
    vec4 extra;           // 1 / (cos inner - cos outer), flags, 0, 0
};

layout(std430, set = 3, binding = 0) readonly buffer LightBlock {
    vec4  lightGrid;      // corner x, y, 1 / cell size, cells per side
    vec4  lightInfo;      // lights per cell, lights, on, debug view
    Light lights[];
};

layout(std430, set = 3, binding = 1) readonly buffer LightCells {
    uint cellLights[];    // per cell (y * cells + x): light indices, 0xFFFFFFFF ends a list
};

const int kMaxLightsPerCell = 32;

bool LightsOn() { return lightInfo.z > 0.5; }
int LightsDebug() { return int(lightInfo.w + 0.5); }

// The first entry of a world point's cell (clamped to the grid: a light past its radius adds nothing).
int LightCellFirst(vec2 xy)
{
    int cells = int(lightGrid.w + 0.5);
    ivec2 c = clamp(ivec2(floor((xy - lightGrid.xy) * lightGrid.z)), ivec2(0), ivec2(cells - 1));
    return (c.y * cells + c.x) * int(lightInfo.x + 0.5);
}

// The lights at a world point with a world normal (unit), summed (rgb); `count` how many reach it.
vec3 LightsAt(vec3 p, vec3 n, out int count)
{
    vec3 sum = vec3(0.0);
    count = 0;

    if (!LightsOn())
    {
        return sum;
    }

    int first = LightCellFirst(p.xy);
    int perCell = int(lightInfo.x + 0.5);

    for (int k = 0; k < kMaxLightsPerCell; k++)
    {
        if (k >= perCell)
        {
            break;
        }

        uint index = cellLights[first + k];

        if (index == 0xFFFFFFFFu)
        {
            break;
        }

        Light l = lights[index];
        vec3 toLight = l.posInvRadius.xyz - p;
        float d2 = dot(toLight, toLight);
        float d = sqrt(d2);
        float t = clamp(d * l.posInvRadius.w, 0.0, 1.0);
        float window = (1.0 - t * t) * (1.0 - t * t);

        if (window <= 0.0)
        {
            continue;
        }

        window = pow(window, max(l.colorFalloff.w, 0.01));
        vec3 dir = toLight * inversesqrt(max(d2, 1e-8));
        float lambert = clamp((dot(n, dir) + 0.5) * (1.0 / 1.5), 0.0, 1.0);
        float spot = clamp((dot(-dir, l.spotDirCos.xyz) - l.spotDirCos.w) * l.extra.x, 0.0, 1.0);
        float s = sqrt(d2 + 1.0);
        float strength = window * lambert * spot / ((0.7 + 0.03 * s) * s);
        sum += l.colorFalloff.rgb * strength;
        count += strength > 0.0 ? 1 : 0;
    }

    return sum;
}

// The stock light `stock` (0..1 per channel, as the client lights the surface) with the lamps added like
// the client adds its own lights; above a knee (0.8, or the stock light's brightest channel if higher)
// the brightest channel is compressed smoothly (knee + room (1 - e^(-over / room)), slope 1 at the knee:
// no ring) and all three scaled alike, so a flame's hue stays. Without lamps: the stock light.
vec3 LightsCombine(vec3 stock, vec3 lamps)
{
    vec3 total = stock + lamps;
    float top = max(total.r, max(total.g, total.b));
    float knee = max(max(stock.r, max(stock.g, stock.b)), 0.8);
    float over = max(top - knee, 0.0);
    float room = max(1.0 - knee, 0.001);
    float shaped = top - over + room * (1.0 - exp(-over / room));
    return total * (shaped / max(top, 0.001));
}
