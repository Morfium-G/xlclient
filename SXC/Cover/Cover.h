#pragma once

#include <cstdint>

class CGxTex;
class CMapRenderChunk;

// SXC's surface covers: the sxc.cover service (SXC/Api/sxc_cover.h) holds what a mod sets (the entries,
// the cover textures, the maps, the look, the resolver); a seam in the terrain chunk draw hands each
// chunk's layer entries to the terrain shader; the Vulkan device pulls the rest once per frame.
namespace SXC::Cover {
    constexpr uint32_t kMaxEntries = 127;
    constexpr uint32_t kMaxTextures = 1024;
    constexpr uint32_t kMaxSlots = 1024;     // texture slots the shader can sample at once (the device says how many it has)

    // What the device reads per frame (pointers valid until the next service call; the frame reads
    // them right away).
    struct Frame {
        bool on = false;                  // a mod turned it on
        bool fallenSnow = true;
        int32_t debug = 0;
        float brightness = 0.93f, wetDarkening = 0.45f, thinDepth = 0.08f;
        float depthScale = 1.0f, breakupScale = 1.0f, driftScale = 1.0f, textureTile = 33.3333333f / 8.0f;
        float outside[3] = { 0.0f, 1.0f, 0.0f };
        float tessDetail = 0.0f, tessReach = 250.0f, tessMost = 16.0f; // depth (sxc_cover_look::tess_*); detail 0 = flat

        const void* entries = nullptr;    // sxc_cover_entry[entryCount]: entry k + 1 at [k]
        uint32_t entryCount = 0, entryVersion = 0;

        // The maps (sxc_cover_map's layout, RGBA32F per cell); size 0 = none.
        uint32_t nearSize = 0;
        float nearCell = 2.0f;
        int32_t nearFirstI = 0, nearFirstJ = 0;
        const float* nearCells = nullptr;
        uint32_t nearVersion = 0;
        uint32_t farSize = 0;
        float farCell = 33.333f;
        int32_t farFirstI = 0, farFirstJ = 0;
        const float* farCells = nullptr;
        uint32_t farVersion = 0;

        // The textures with a slot this frame: slot s (1..slotCount) shows texture slotTexture[s - 1]
        // (an ID, 1..) through slotGx[s - 1] (loaded); textureGloss[id - 1]: its alpha is a gloss mask.
        uint32_t slotCount = 0;
        uint16_t slotTexture[kMaxSlots] = {};
        CGxTex* slotGx[kMaxSlots] = {};
        bool textureGloss[kMaxTextures] = {};
    };

    // The device's side: whether it can draw covers, how many texture slots its shader has, its one-line
    // status.
    using StatusFn = const char* (*)();
    void SetDevice(bool available, uint32_t textureSlots, StatusFn status);

    // This frame's data (the texture slots assigned now). Main thread.
    const Frame& Current();

    // The seam: CMapRenderChunk::RenderSinglePassVertexPixelShader, before the draw. Sets pixel
    // constants c11 (each layer's painted entry), c12 (each layer's fallen-snow look), c13.x (1: the
    // chunk can carry depth, so the device raises it) and c13.y (its liquids' lowest top: the water line
    // beyond the near map), asking the mod's resolver the first time a chunk is met, and counts the
    // textures the chunk shows.
    void BeforeChunkDraw(CMapRenderChunk* chunk);
}
