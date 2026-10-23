// SXC: our point lights' GPU side (Vulkan/Lights.hpp).
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Lights.hpp"

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Context.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/DeviceCaps.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Frame.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Loader.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Log.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Memory.hpp"

#include <cstdio>
#include <cstring>

namespace
{
    namespace context = gxvk::vk::context;
    namespace frame   = gxvk::vk::frame;
    namespace loader  = gxvk::vk::loader;
    namespace memory  = gxvk::vk::memory;
    using gxvk::vk::lights::Frame;
    using gxvk::vk::lights::kMaxCells;
    using gxvk::vk::lights::kMaxLights;
    using gxvk::vk::lights::kMaxPerCell;

    using gxvk::log::Write;
    using gxvk::log::Info;
    using gxvk::log::Error;
    using gxvk::vk::ResultName;

    constexpr uint32_t kSlots      = frame::kFramesInFlight;
    constexpr uint32_t kLightBytes = 64;
    /// Shaders/Lights.glsl's LightBlock: the grid (origin x, y, 1 / cell, cells), the info (per cell,
    /// lights, on, debug), the view's inverse (three rows), then the lights.
    constexpr VkDeviceSize kHeaderBytes = 80;
    constexpr VkDeviceSize kLightsBytes = kHeaderBytes + kMaxLights * kLightBytes;
    constexpr VkDeviceSize kCellsBytes  = static_cast<VkDeviceSize>(kMaxCells) * kMaxCells * kMaxPerCell * sizeof(uint32_t);

    struct Slot
    {
        VkDescriptorSet set    = VK_NULL_HANDLE;
        VkDescriptorSet off    = VK_NULL_HANDLE;   ///< both bindings on `State::offBuffer` (the lights off)
        memory::Buffer  lights, cells;
        uint64_t        serial = 0;
    };

    struct State
    {
        bool                  ready  = false;
        bool                  failed = false;
        VkDevice              device = nullptr;
        const loader::Device* dapi   = nullptr;
        gxvk::vk::lights::Provider provider = nullptr;
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkDescriptorPool      pool      = VK_NULL_HANDLE;
        Slot                  slots[kSlots];
        memory::Buffer        offBuffer;          ///< a zero header: the lights off
        char                  status[160] = "no terrain drawn yet";
    };
    State s;

    bool Check(VkResult r, const char* what)
    {
        if (r == VK_SUCCESS) return true;
        Write(Error, "vulkan: lights: %s failed: %s", what, ResultName(r));
        s.failed = true;
        return false;
    }
}

namespace gxvk::vk::lights
{
    void SetProvider(Provider provider) { s.provider = provider; }
    bool Ready() { return s.ready; }
    VkDescriptorSetLayout SetLayout() { return s.setLayout; }
    const char* Status() { return s.status; }

    bool Init()
    {
        if (s.ready) return true;
        if (s.failed) return false;
        const context::Handles* h = context::Get();
        if (!h || !h->dapi) return false;
        s.device = h->device;
        s.dapi   = h->dapi;
        const loader::Device& d = *s.dapi;

        // Read where the pixels are lit (and where the vertices or the tessellation may light later).
        VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        if (context::Caps().enabled & gxvk::vk::kCapTessellation)
            stages |= VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
        VkDescriptorSetLayoutBinding bindings[2] = {};
        for (uint32_t i = 0; i < 2; ++i)
        {
            bindings[i].binding         = i;
            bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags      = stages;
        }
        VkDescriptorSetLayoutCreateInfo setLayout = {};
        setLayout.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        setLayout.bindingCount = 2;
        setLayout.pBindings    = bindings;
        if (!Check(d.CreateDescriptorSetLayout(s.device, &setLayout, nullptr, &s.setLayout), "vkCreateDescriptorSetLayout")) return false;

        VkDescriptorPoolSize size = {};
        size.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        size.descriptorCount = 4 * kSlots;
        VkDescriptorPoolCreateInfo pool = {};
        pool.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool.maxSets       = 2 * kSlots;
        pool.poolSizeCount = 1;
        pool.pPoolSizes    = &size;
        if (!Check(d.CreateDescriptorPool(s.device, &pool, nullptr, &s.pool), "vkCreateDescriptorPool")) return false;

        if (!memory::CreateBuffer(kHeaderBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, memory::Where::Stream, s.offBuffer) || !s.offBuffer.block.mapped)
        {
            Write(Error, "vulkan: lights: no buffer for the lights off");
            s.failed = true;
            return false;
        }
        std::memset(s.offBuffer.block.mapped, 0, static_cast<size_t>(kHeaderBytes));
        memory::FlushHost(s.offBuffer.block);

        auto write = [&](VkDescriptorSet set, VkBuffer lights, VkDeviceSize lightsBytes, VkBuffer cells, VkDeviceSize cellsBytes) {
            VkDescriptorBufferInfo infos[2] = { { lights, 0, lightsBytes }, { cells, 0, cellsBytes } };
            VkWriteDescriptorSet writes[2] = {};
            for (uint32_t i = 0; i < 2; ++i)
            {
                writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstSet          = set;
                writes[i].dstBinding      = i;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[i].pBufferInfo     = &infos[i];
            }
            d.UpdateDescriptorSets(s.device, 2, writes, 0, nullptr);
        };

        for (Slot& slot : s.slots)
        {
            const VkDescriptorSetLayout layouts[2] = { s.setLayout, s.setLayout };
            VkDescriptorSet sets[2] = {};
            VkDescriptorSetAllocateInfo alloc = {};
            alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            alloc.descriptorPool     = s.pool;
            alloc.descriptorSetCount = 2;
            alloc.pSetLayouts        = layouts;
            if (!Check(d.AllocateDescriptorSets(s.device, &alloc, sets), "vkAllocateDescriptorSets")) return false;
            slot.set = sets[0];
            slot.off = sets[1];
            write(slot.off, s.offBuffer.handle, kHeaderBytes, s.offBuffer.handle, kHeaderBytes);
            if (!memory::CreateBuffer(kLightsBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, memory::Where::Stream, slot.lights) || !slot.lights.block.mapped
                || !memory::CreateBuffer(kCellsBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, memory::Where::Stream, slot.cells) || !slot.cells.block.mapped)
            {
                Write(Error, "vulkan: lights: no buffers for the lights");
                s.failed = true;
                return false;
            }
            std::memset(slot.lights.block.mapped, 0, static_cast<size_t>(kHeaderBytes));
            memory::FlushHost(slot.lights.block);

            // The buffers never change: the sets are written once.
            write(slot.set, slot.lights.handle, kLightsBytes, slot.cells.handle, kCellsBytes);
        }

        s.ready = true;
        Write(Info, "vulkan: lights: ready (up to %u lights, %u x %u cells of up to %u)", kMaxLights, kMaxCells, kMaxCells, kMaxPerCell);
        return true;
    }

    VkDescriptorSet FrameSet(uint32_t slotIndex, uint64_t serial)
    {
        if (!s.ready || slotIndex >= kSlots) return VK_NULL_HANDLE;
        Slot& slot = s.slots[slotIndex];
        if (slot.serial == serial) return slot.set;

        Frame f;
        const bool on = s.provider && s.provider(f) && f.on && f.cells > 0 && f.cells <= kMaxCells && f.perCell > 0 && f.perCell <= kMaxPerCell
                        && f.cellLights && f.lightCount <= kMaxLights && (f.lightCount == 0 || f.lights);

        float header[20] = {};
        if (on)
        {
            header[0] = f.originX;
            header[1] = f.originY;
            header[2] = 1.0f / f.cellSize;
            header[3] = static_cast<float>(f.cells);
            header[4] = static_cast<float>(f.perCell);
            header[5] = static_cast<float>(f.lightCount);
            header[6] = 1.0f;
            header[7] = static_cast<float>(f.debug);
            std::memcpy(header + 8, f.viewInv, sizeof f.viewInv);
            uint8_t* out = static_cast<uint8_t*>(slot.lights.block.mapped);
            std::memcpy(out + kHeaderBytes, f.lights, static_cast<size_t>(f.lightCount) * kLightBytes);
            std::memcpy(slot.cells.block.mapped, f.cellLights, static_cast<size_t>(f.cells) * f.cells * f.perCell * sizeof(uint32_t));
            memory::FlushHost(slot.cells.block);
            std::snprintf(s.status, sizeof s.status, "%u lights on a %u x %u grid (up to %u per cell)", f.lightCount, f.cells, f.cells, f.perCell);
        }
        else
            std::snprintf(s.status, sizeof s.status, "terrain drawn, lights off");
        std::memcpy(slot.lights.block.mapped, header, sizeof header);
        memory::FlushHost(slot.lights.block);
        slot.serial = serial;
        return slot.set;
    }

    VkDescriptorSet OffSet(uint32_t slotIndex)
    {
        return s.ready && slotIndex < kSlots ? s.slots[slotIndex].off : VK_NULL_HANDLE;
    }

    void Shutdown()
    {
        if (!s.dapi || !s.device)
        {
            const Provider provider = s.provider;
            s = State();
            s.provider = provider;
            return;
        }
        const loader::Device& d = *s.dapi;
        for (Slot& slot : s.slots)
        {
            memory::DestroyBuffer(slot.lights);
            memory::DestroyBuffer(slot.cells);
        }
        memory::DestroyBuffer(s.offBuffer);
        if (s.pool != VK_NULL_HANDLE) d.DestroyDescriptorPool(s.device, s.pool, nullptr);
        if (s.setLayout != VK_NULL_HANDLE) d.DestroyDescriptorSetLayout(s.device, s.setLayout, nullptr);
        const Provider provider = s.provider;
        s = State();
        s.provider = provider;   // the device's, kept for the next bring-up
    }
}
