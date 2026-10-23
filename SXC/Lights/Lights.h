#pragma once

#include <cstdint>

// SXC's point lights (port plan step 6): the sxc.lights service (SXC/Api/sxc_lights.h) holds what a mod
// sets; once per frame the lights are gathered from the M2 scene (the models' own point lights), merged,
// flickered, faded toward the range and listed per cell of a world grid around the camera; the Vulkan
// device pulls the result at its first terrain draw and every pixel adds its cell's lights.
namespace SXC::Lights {
    constexpr uint32_t kMaxPool = 512;
    constexpr uint32_t kMaxPerCell = 32;
    constexpr uint32_t kMaxCells = 64;
    constexpr uint32_t kNoLight = 0xFFFFFFFFu;

    // One light as the shaders read it (64 bytes, std430).
    struct GpuLight {
        float pos[3];        // world
        float invRadius;     // 1 / yd: exactly 0 light at the radius
        float color[3];      // x brightness x fade x flicker
        float falloff;       // the window's power: 1 smooth, higher tighter
        float spotDir[3];    // world, unit: where a spot points
        float cosOuter;      // spot: cos of the outer angle; -2 a point light (always inside)
        float spotScale;     // 1 / (cos inner - cos outer)
        float flags;         // reserved (indoor, occlusion: 6c, 6d)
        float pad[2];
    };
    static_assert(sizeof(GpuLight) == 64, "the shaders read a light as four vec4");

    // What the device reads per frame (pointers valid until the next Current()).
    struct Frame {
        bool on = false;
        int32_t debug = 0;
        float originX = 0.0f, originY = 0.0f; // the grid's corner (world)
        float cellSize = 16.0f;
        uint32_t cells = 0;                   // per side (0: none)
        uint32_t perCell = 0;
        uint32_t lightCount = 0;
        const GpuLight* lights = nullptr;
        const uint32_t* cellLights = nullptr; // cell (x, y) at [(y * cells + x) * perCell ..]: light indices, kNoLight ends a list
    };

    // The device's side: whether it draws our lights, its one-line status.
    using StatusFn = const char* (*)();
    void SetDevice(bool available, StatusFn status);

    // This frame's lights, gathered now. Main thread, once per frame (the device's first terrain draw).
    const Frame& Current();

    // Whether the stock point lights of the terrain chunks are to be left out (ours replace them).
    bool StockTerrainOff();
}
