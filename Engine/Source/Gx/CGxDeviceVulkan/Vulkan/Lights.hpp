// SXC: our point lights (port plan step 6; SXC/Lights). The pipelines that light per pixel carry a
// descriptor set of two storage buffers (Shaders/Lights.glsl): the grid and the lights, and per grid
// cell the indices of the lights that reach it. One pair per frame slot, written at the frame's first
// use from what the provider hands over. Set 3 of our terrain pipelines (after the covers' set 2) and
// of the translated pixel modules flagged `lights` (models, WMOs; step 6b).
#pragma once

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Vk.hpp"

#include <cstdint>

namespace gxvk::vk::lights
{
    constexpr uint32_t kMaxLights  = 512;
    constexpr uint32_t kMaxPerCell = 32;
    constexpr uint32_t kMaxCells   = 64;

    struct Frame
    {
        bool        on = false;
        int         debug = 0;
        float       originX = 0.0f, originY = 0.0f, cellSize = 16.0f;
        uint32_t    cells = 0, perCell = 0, lightCount = 0;
        const void* lights = nullptr;       ///< 64 bytes each (Shaders/Lights.glsl's Light)
        const uint32_t* cellLights = nullptr; ///< cells x cells x perCell light indices, 0xFFFFFFFF ends a list
        float       viewInv[12] = {};       ///< the world scene's view back to the world, three rows (x, y, z; w the translation)
    };

    /// Fills this frame's lights; false when they're off.
    using Provider = bool (*)(Frame& out);
    void SetProvider(Provider provider);

    bool Init();
    bool Ready();

    /// The set's layout (null before Init()).
    VkDescriptorSetLayout SetLayout();

    /// This frame's set for slot `slot`: filled at the first call of the frame. Null when not ready.
    VkDescriptorSet FrameSet(uint32_t slot, uint64_t serial);

    /// A set with the lights off, for draws outside the world scene (portraits, the character screen:
    /// another view, so the world's lights would land in the wrong places).
    VkDescriptorSet OffSet(uint32_t slot);

    const char* Status();

    void Shutdown();
}
