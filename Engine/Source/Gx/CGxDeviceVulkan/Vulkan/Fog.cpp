// SXC: volumetric fog (Vulkan/Fog.hpp).
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Fog.hpp"

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Context.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Frame.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Loader.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Log.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Memory.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Pipelines.hpp"

#include "Engine/Source/Gx/CGxDeviceVulkan/Shaders/FogSpirv.inc"
#include "Engine/Source/Gx/CGxDeviceVulkan/Shaders/PresentSpirv.inc"

#include <cstdio>
#include <cstring>

namespace
{
    namespace context = gxvk::vk::context;
    namespace frame   = gxvk::vk::frame;
    namespace loader  = gxvk::vk::loader;
    namespace memory  = gxvk::vk::memory;

    using gxvk::log::Write;
    using gxvk::log::Debug;
    using gxvk::log::Error;
    using gxvk::vk::ResultName;

    constexpr uint32_t kSlots   = frame::kFramesInFlight;
    constexpr uint32_t kColumnsX = 160, kColumnsY = 90, kSlices = 64; // Fog.glsl's kSlices
    constexpr uint32_t kMaxField = 512;
    constexpr VkFormat kVolumeFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

    /// Shaders/Fog.glsl's FogParams (std140).
    struct Params
    {
        float invViewProj[16];
        float origin[4];
        float viewport[4];
        float depth[4];
        float colour[4];
        float field[4];
        float noise[4];
        float misc[4];
        float haze[4];
    };

    /// An image this module owns, with its memory and the layout it is in.
    struct Image
    {
        VkImage       image  = VK_NULL_HANDLE;
        VkImageView   view   = VK_NULL_HANDLE;
        memory::Block block;
        uint32_t      size   = 0;   ///< for the field images: texels per side
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };

    struct Slot
    {
        VkDescriptorSet set = VK_NULL_HANDLE;
        memory::Buffer  params;
    };

    struct State
    {
        bool                  ready  = false;
        bool                  failed = false;
        VkDevice              device = nullptr;
        const loader::Device* dapi   = nullptr;

        VkShaderModule        comp = VK_NULL_HANDLE, vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkPipelineLayout      layout    = VK_NULL_HANDLE;
        VkDescriptorPool      pool      = VK_NULL_HANDLE;
        VkPipeline            compute   = VK_NULL_HANDLE;
        VkSampler             linearRepeat = VK_NULL_HANDLE, linearClamp = VK_NULL_HANDLE, nearestClamp = VK_NULL_HANDLE;
        Slot                  slots[kSlots];

        // The composite's pass and pipeline, for one colour format (remade when it changes).
        VkFormat     format    = VK_FORMAT_UNDEFINED;
        VkRenderPass pass      = VK_NULL_HANDLE;
        VkPipeline   composite = VK_NULL_HANDLE;

        Image    volume;
        Image    fieldLight, fieldPool; ///< the near field's two layers
        uint32_t fieldVersion = 0;     ///< what the field images hold
        bool     fieldPresent = false;

        char status[160] = "not drawn yet";
    };
    State s;

    void SetStatus(const char* text)
    {
        std::snprintf(s.status, sizeof s.status, "%s", text);
    }

    bool Check(VkResult r, const char* what)
    {
        if (r == VK_SUCCESS) return true;
        Write(Error, "vulkan: fog: %s failed: %s", what, ResultName(r));
        s.failed = true;
        return false;
    }

    bool CreateModule(const uint32_t* words, size_t bytes, VkShaderModule& out, const char* what)
    {
        VkShaderModuleCreateInfo info = {};
        info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = bytes;
        info.pCode    = words;
        return Check(s.dapi->CreateShaderModule(s.device, &info, nullptr, &out), what);
    }

    VkSampler MakeSampler(VkFilter filter, VkSamplerAddressMode address)
    {
        VkSamplerCreateInfo info = {};
        info.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter    = filter;
        info.minFilter    = filter;
        info.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.addressModeU = address;
        info.addressModeV = address;
        info.addressModeW = address;
        info.maxLod       = 0.0f;
        VkSampler sampler = VK_NULL_HANDLE;
        Check(s.dapi->CreateSampler(s.device, &info, nullptr, &sampler), "vkCreateSampler");
        return sampler;
    }

    void DestroyImage(Image& img)
    {
        if (img.view != VK_NULL_HANDLE) s.dapi->DestroyImageView(s.device, img.view, nullptr);
        if (img.image != VK_NULL_HANDLE) s.dapi->DestroyImage(s.device, img.image, nullptr);
        memory::Free(img.block);
        img = Image();
    }

    /// A 2D or 3D image in device memory with a view of the whole of it.
    bool CreateImage(VkImageType type, uint32_t w, uint32_t h, uint32_t d, VkImageUsageFlags usage, Image& out)
    {
        VkImageCreateInfo info = {};
        info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType     = type;
        info.format        = kVolumeFormat;
        info.extent        = VkExtent3D{ w, h, d };
        info.mipLevels     = 1;
        info.arrayLayers   = 1;
        info.samples       = VK_SAMPLE_COUNT_1_BIT;
        info.tiling        = VK_IMAGE_TILING_OPTIMAL;
        info.usage         = usage;
        info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!Check(s.dapi->CreateImage(s.device, &info, nullptr, &out.image), "vkCreateImage")) return false;
        VkMemoryRequirements req = {};
        s.dapi->GetImageMemoryRequirements(s.device, out.image, &req);
        if (!memory::Allocate(req, memory::Where::DeviceLocal, out.block))
        {
            Write(Error, "vulkan: fog: no device memory for a %ux%ux%u image", w, h, d);
            s.dapi->DestroyImage(s.device, out.image, nullptr);
            out = Image();
            s.failed = true;
            return false;
        }
        VkImageViewCreateInfo view = {};
        view.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image                       = out.image;
        view.viewType                    = type == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
        view.format                      = kVolumeFormat;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.layerCount = 1;
        if (!Check(s.dapi->BindImageMemory(s.device, out.image, out.block.memory, out.block.offset), "vkBindImageMemory")
            || !Check(s.dapi->CreateImageView(s.device, &view, nullptr, &out.view), "vkCreateImageView"))
        {
            DestroyImage(out);
            return false;
        }
        out.size   = w;
        out.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        return true;
    }

    void Barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspects, VkImageLayout from, VkImageLayout to,
                 VkPipelineStageFlags srcStage, VkAccessFlags srcAccess, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess)
    {
        VkImageMemoryBarrier b = {};
        b.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask               = srcAccess;
        b.dstAccessMask               = dstAccess;
        b.oldLayout                   = from;
        b.newLayout                   = to;
        b.srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        b.image                       = image;
        b.subresourceRange.aspectMask = aspects;
        b.subresourceRange.levelCount = 1;
        b.subresourceRange.layerCount = 1;
        s.dapi->CmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
    }

    bool Init()
    {
        if (s.ready) return true;
        if (s.failed) return false;
        const context::Handles* h = context::Get();
        if (!h || !h->dapi) return false;
        s.device = h->device;
        s.dapi   = h->dapi;
        const loader::Device& d = *s.dapi;

        if (!CreateModule(kFogComp, sizeof kFogComp, s.comp, "vkCreateShaderModule (Fog.comp)")
            || !CreateModule(kPresentVert, sizeof kPresentVert, s.vert, "vkCreateShaderModule (Present.vert)")
            || !CreateModule(kFogFrag, sizeof kFogFrag, s.frag, "vkCreateShaderModule (Fog.frag)"))
            return false;

        // One set for both pipelines: 0 the parameters, 1 and 2 the near field, 3 the volume to
        // write, 4 the scene depth, 5 the volume to read.
        const VkShaderStageFlags both = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutBinding bindings[6] = {};
        const VkDescriptorType types[6] = {
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        };
        for (uint32_t i = 0; i < 6; ++i)
        {
            bindings[i].binding         = i;
            bindings[i].descriptorType  = types[i];
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags      = both;
        }
        VkDescriptorSetLayoutCreateInfo setLayout = {};
        setLayout.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        setLayout.bindingCount = 6;
        setLayout.pBindings    = bindings;
        if (!Check(d.CreateDescriptorSetLayout(s.device, &setLayout, nullptr, &s.setLayout), "vkCreateDescriptorSetLayout")) return false;

        VkPipelineLayoutCreateInfo layout = {};
        layout.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout.setLayoutCount = 1;
        layout.pSetLayouts    = &s.setLayout;
        if (!Check(d.CreatePipelineLayout(s.device, &layout, nullptr, &s.layout), "vkCreatePipelineLayout")) return false;

        VkDescriptorPoolSize sizes[3] = {};
        sizes[0].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        sizes[0].descriptorCount = kSlots;
        sizes[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sizes[1].descriptorCount = 4 * kSlots;
        sizes[2].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        sizes[2].descriptorCount = kSlots;
        VkDescriptorPoolCreateInfo pool = {};
        pool.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool.maxSets       = kSlots;
        pool.poolSizeCount = 3;
        pool.pPoolSizes    = sizes;
        if (!Check(d.CreateDescriptorPool(s.device, &pool, nullptr, &s.pool), "vkCreateDescriptorPool")) return false;

        for (Slot& slot : s.slots)
        {
            VkDescriptorSetAllocateInfo alloc = {};
            alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            alloc.descriptorPool     = s.pool;
            alloc.descriptorSetCount = 1;
            alloc.pSetLayouts        = &s.setLayout;
            if (!Check(d.AllocateDescriptorSets(s.device, &alloc, &slot.set), "vkAllocateDescriptorSets")) return false;
            if (!memory::CreateBuffer(sizeof(Params), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, memory::Where::Stream, slot.params) || !slot.params.block.mapped)
            {
                Write(Error, "vulkan: fog: no buffer for the parameters");
                s.failed = true;
                return false;
            }
        }

        VkComputePipelineCreateInfo compute = {};
        compute.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        compute.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        compute.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        compute.stage.module = s.comp;
        compute.stage.pName  = "main";
        compute.layout       = s.layout;
        if (!Check(d.CreateComputePipelines(s.device, VK_NULL_HANDLE, 1, &compute, nullptr, &s.compute), "vkCreateComputePipelines")) return false;

        s.linearRepeat = MakeSampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT);
        s.linearClamp  = MakeSampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
        s.nearestClamp = MakeSampler(VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
        if (s.failed) return false;

        // The volume: written by the compute shader, sampled by the composite; it stays in GENERAL.
        // The near field starts as 1 x 1 "no fog" until a field arrives.
        if (!CreateImage(VK_IMAGE_TYPE_3D, kColumnsX, kColumnsY, kSlices, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, s.volume)
            || !CreateImage(VK_IMAGE_TYPE_2D, 1, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, s.fieldLight)
            || !CreateImage(VK_IMAGE_TYPE_2D, 1, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, s.fieldPool))
            return false;

        s.ready = true;
        Write(Debug, "vulkan: fog: ready (%ux%ux%u froxels)", kColumnsX, kColumnsY, kSlices);
        return true;
    }

    /// The composite's pass over a colour format: the target is loaded, drawn over, left in its
    /// resting layout; the same dependencies as the frame's own passes.
    bool EnsureComposite(VkFormat format)
    {
        if (s.format == format && s.composite != VK_NULL_HANDLE) return true;
        const loader::Device& d = *s.dapi;
        if (s.composite != VK_NULL_HANDLE) d.DestroyPipeline(s.device, s.composite, nullptr);
        if (s.pass != VK_NULL_HANDLE) d.DestroyRenderPass(s.device, s.pass, nullptr);
        s.composite = VK_NULL_HANDLE;
        s.pass      = VK_NULL_HANDLE;
        s.format    = format;

        VkAttachmentDescription a = {};
        a.format         = format;
        a.samples        = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
        a.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        a.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub = {};
        sub.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments    = &ref;
        VkSubpassDependency dep[2] = {};
        dep[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
        dep[0].dstSubpass    = 0;
        dep[0].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
        dep[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        dep[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep[1].srcSubpass    = 0;
        dep[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
        dep[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo info = {};
        info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        info.attachmentCount = 1;
        info.pAttachments    = &a;
        info.subpassCount    = 1;
        info.pSubpasses      = &sub;
        info.dependencyCount = 2;
        info.pDependencies   = dep;
        if (!Check(d.CreateRenderPass(s.device, &info, nullptr, &s.pass), "vkCreateRenderPass")) return false;

        VkPipelineShaderStageCreateInfo stages[2] = {};
        stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = s.vert;
        stages[0].pName  = "main";
        stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = s.frag;
        stages[1].pName  = "main";
        VkPipelineVertexInputStateCreateInfo vertexInput = {};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo assembly = {};
        assembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport = {};
        viewport.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport.viewportCount = 1;
        viewport.scissorCount  = 1;
        VkPipelineRasterizationStateCreateInfo raster = {};
        raster.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode    = VK_CULL_MODE_NONE;
        raster.frontFace   = VK_FRONT_FACE_CLOCKWISE;
        raster.lineWidth   = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample = {};
        multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        // dst x transmittance (the source's alpha) + the scattered light; the target's alpha kept.
        VkPipelineColorBlendAttachmentState blend = {};
        blend.blendEnable         = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend.colorBlendOp        = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.alphaBlendOp        = VK_BLEND_OP_ADD;
        blend.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
        VkPipelineColorBlendStateCreateInfo blending = {};
        blending.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blending.attachmentCount = 1;
        blending.pAttachments    = &blend;
        const VkDynamicState dynamics[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamic = {};
        dynamic.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates    = dynamics;
        VkGraphicsPipelineCreateInfo pipeline = {};
        pipeline.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeline.stageCount          = 2;
        pipeline.pStages             = stages;
        pipeline.pVertexInputState   = &vertexInput;
        pipeline.pInputAssemblyState = &assembly;
        pipeline.pViewportState      = &viewport;
        pipeline.pRasterizationState = &raster;
        pipeline.pMultisampleState   = &multisample;
        pipeline.pColorBlendState    = &blending;
        pipeline.pDynamicState       = &dynamic;
        pipeline.layout              = s.layout;
        pipeline.renderPass          = s.pass;
        return Check(d.CreateGraphicsPipelines(s.device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &s.composite), "vkCreateGraphicsPipelines (fog)");
    }

    /// The near field into its two images when it changed: staged, copied, ready for the compute
    /// shader. A field of another size remakes the images.
    bool UploadField(VkCommandBuffer cmd, const gxvk::vk::fog::Frame& f)
    {
        const bool present = f.light && f.pool && f.fieldSize > 0 && f.fieldSize <= kMaxField;

        if (!present)
        {
            s.fieldPresent = false;
            return true;
        }

        if (s.fieldPresent && f.fieldVersion == s.fieldVersion && s.fieldLight.size == f.fieldSize) return true;

        if (s.fieldLight.size != f.fieldSize)
        {
            // Once per field size (normally once a session): the frames in flight may still read the
            // old images, so the device is let finish first. Nothing of this frame names them yet.
            s.dapi->DeviceWaitIdle(s.device);
            Image* layers[2] = { &s.fieldLight, &s.fieldPool };
            for (Image* img : layers)
            {
                DestroyImage(*img);
                if (!CreateImage(VK_IMAGE_TYPE_2D, f.fieldSize, f.fieldSize, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, *img)) return false;
            }
        }

        const VkDeviceSize bytes = static_cast<VkDeviceSize>(f.fieldSize) * f.fieldSize * 8; // RGBA16F
        VkBuffer     buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        void*        mapped = nullptr;
        if (!frame::Stage(bytes * 2, buffer, offset, mapped)) return false;
        std::memcpy(mapped, f.light, static_cast<size_t>(bytes));
        std::memcpy(static_cast<uint8_t*>(mapped) + bytes, f.pool, static_cast<size_t>(bytes));

        Image* layers[2] = { &s.fieldLight, &s.fieldPool };
        for (uint32_t i = 0; i < 2; ++i)
        {
            Image& img = *layers[i];
            Barrier(cmd, img.image, VK_IMAGE_ASPECT_COLOR_BIT, img.layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
            VkBufferImageCopy copy = {};
            copy.bufferOffset                = offset + bytes * i;
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.layerCount = 1;
            copy.imageExtent                 = VkExtent3D{ f.fieldSize, f.fieldSize, 1 };
            s.dapi->CmdCopyBufferToImage(cmd, buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            Barrier(cmd, img.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
            img.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }

        s.fieldVersion = f.fieldVersion;
        s.fieldPresent = true;
        return true;
    }

    /// The 1 x 1 placeholders hold nothing until first used: cleared to "no fog" once.
    void ReadyPlaceholders(VkCommandBuffer cmd)
    {
        Image* layers[2] = { &s.fieldLight, &s.fieldPool };
        for (Image* img : layers)
        {
            if (img->layout != VK_IMAGE_LAYOUT_UNDEFINED) continue;
            Barrier(cmd, img->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            VkClearColorValue zero = {};
            VkImageSubresourceRange range = {};
            range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            range.levelCount = 1;
            range.layerCount = 1;
            s.dapi->CmdClearColorImage(cmd, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
            Barrier(cmd, img->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
            img->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
    }

    /// A 4 x 4 inverse (Gauss-Jordan with partial pivoting). False when singular.
    bool Invert(const float in[16], float out[16])
    {
        double m[4][8];
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 8; ++c) m[r][c] = c < 4 ? in[r * 4 + c] : (c - 4 == r ? 1.0 : 0.0);
        for (int c = 0; c < 4; ++c)
        {
            int pivot = c;
            for (int r = c + 1; r < 4; ++r)
                if ((m[r][c] < 0 ? -m[r][c] : m[r][c]) > (m[pivot][c] < 0 ? -m[pivot][c] : m[pivot][c])) pivot = r;
            if (m[pivot][c] == 0.0) return false;
            if (pivot != c)
                for (int k = 0; k < 8; ++k) { const double t = m[c][k]; m[c][k] = m[pivot][k]; m[pivot][k] = t; }
            const double inv = 1.0 / m[c][c];
            for (int k = 0; k < 8; ++k) m[c][k] *= inv;
            for (int r = 0; r < 4; ++r)
            {
                if (r == c) continue;
                const double f = m[r][c];
                for (int k = 0; k < 8; ++k) m[r][k] -= f * m[c][k];
            }
        }
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) out[r * 4 + c] = static_cast<float>(m[r][c + 4]);
        return true;
    }
}

namespace gxvk::vk::fog
{
    bool Record(const Frame& f)
    {
        if (!frame::DepthSampled())
        {
            SetStatus("off: the depth buffers can't be sampled on this device");
            return false;
        }
        if (!Init())
        {
            SetStatus("off: the fog's objects could not be made (see the log)");
            return false;
        }

        Params p = {};
        if (!Invert(f.viewProj, p.invViewProj))
        {
            SetStatus("skipped: the view could not be inverted");
            return false;
        }

        frame::EffectTarget t;
        if (!frame::BeginEffect(t))
        {
            SetStatus("skipped: nothing drawn on the target yet");
            return false;
        }
        pipelines::ForgetBound();
        if (!EnsureComposite(t.format))
        {
            SetStatus("off: the composite pipeline could not be made");
            return false;
        }

        const loader::Device& d   = *s.dapi;
        VkCommandBuffer       cmd = t.cmd;
        ReadyPlaceholders(cmd);
        if (!UploadField(cmd, f))
        {
            SetStatus("skipped: the field could not be uploaded");
            return false;
        }

        // The parameters, into this slot's buffer (its previous frame has completed).
        Slot& slot = s.slots[frame::Slot()];
        p.origin[0]   = f.origin[0];
        p.origin[1]   = f.origin[1];
        p.origin[2]   = f.origin[2];
        p.origin[3]   = f.seconds;
        p.viewport[0] = f.viewport.x;
        p.viewport[1] = f.viewport.y;
        p.viewport[2] = f.viewport.width;
        p.viewport[3] = f.viewport.height;
        p.depth[0]    = f.viewport.minDepth;
        p.depth[1]    = f.viewport.maxDepth;
        p.depth[2]    = f.nearDistance > 0.05f ? f.nearDistance : 0.05f;
        p.depth[3]    = f.farDistance > p.depth[2] + 1.0f ? f.farDistance : p.depth[2] + 1.0f;
        p.colour[0]   = f.colour[0];
        p.colour[1]   = f.colour[1];
        p.colour[2]   = f.colour[2];
        p.field[0]    = f.cellSize;
        p.field[1]    = static_cast<float>(s.fieldPresent ? s.fieldLight.size : 1);
        p.field[2]    = static_cast<float>(f.firstI);
        p.field[3]    = static_cast<float>(f.firstJ);
        p.noise[0]    = f.noiseAmount;
        p.noise[1]    = f.noiseTile > 1.0f ? f.noiseTile : 1.0f;
        p.noise[2]    = f.drift[0];
        p.noise[3]    = f.drift[1];
        p.misc[0]     = f.densityScale;
        p.misc[1]     = static_cast<float>(f.debug);
        p.misc[2]     = s.fieldPresent ? 1.0f : 0.0f;
        p.misc[3]     = f.hazeDensity;
        p.haze[0]     = f.hazeBase;
        p.haze[1]     = f.hazeThickness;
        std::memcpy(slot.params.block.mapped, &p, sizeof p);
        memory::FlushHost(slot.params.block);

        VkDescriptorBufferInfo params = { slot.params.handle, 0, sizeof(Params) };
        VkDescriptorImageInfo images[5] = {};
        images[0] = { s.linearRepeat, s.fieldLight.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        images[1] = { s.linearRepeat, s.fieldPool.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        images[2] = { VK_NULL_HANDLE, s.volume.view, VK_IMAGE_LAYOUT_GENERAL };
        images[3] = { s.nearestClamp, t.depthView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
        images[4] = { s.linearClamp, s.volume.view, VK_IMAGE_LAYOUT_GENERAL };
        VkWriteDescriptorSet writes[6] = {};
        for (uint32_t i = 0; i < 6; ++i)
        {
            writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet          = slot.set;
            writes[i].dstBinding      = i;
            writes[i].descriptorCount = 1;
        }
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo    = &params;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo     = &images[0];
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[2].pImageInfo     = &images[1];
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[3].pImageInfo     = &images[2];
        writes[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[4].pImageInfo     = &images[3];
        writes[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[5].pImageInfo     = &images[4];
        d.UpdateDescriptorSets(s.device, 6, writes, 0, nullptr);

        // The volume: the last frame's composite has read it before this frame's compute writes it.
        Barrier(cmd, s.volume.image, VK_IMAGE_ASPECT_COLOR_BIT, s.volume.layout, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
        s.volume.layout = VK_IMAGE_LAYOUT_GENERAL;
        d.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.compute);
        d.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.layout, 0, 1, &slot.set, 0, nullptr);
        d.CmdDispatch(cmd, (kColumnsX + 7) / 8, (kColumnsY + 7) / 8, 1);

        // The volume to the composite; the depth from attachment to read-only (the world's writes done).
        Barrier(cmd, s.volume.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        Barrier(cmd, t.depth, t.depthAspects, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);

        VkFramebuffer fb = VK_NULL_HANDLE;
        VkFramebufferCreateInfo fbInfo = {};
        fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass      = s.pass;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments    = &t.colour;
        fbInfo.width           = t.extent.width;
        fbInfo.height          = t.extent.height;
        fbInfo.layers          = 1;
        const bool drawn = Check(d.CreateFramebuffer(s.device, &fbInfo, nullptr, &fb), "vkCreateFramebuffer (fog)");

        if (drawn)
        {
            VkRenderPassBeginInfo begin = {};
            begin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            begin.renderPass        = s.pass;
            begin.framebuffer       = fb;
            begin.renderArea.extent = t.extent;
            d.CmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
            d.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.composite);
            d.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.layout, 0, 1, &slot.set, 0, nullptr);
            VkViewport viewport = f.viewport;
            viewport.minDepth   = 0.0f;
            viewport.maxDepth   = 1.0f;
            VkRect2D scissor = {};
            scissor.offset = VkOffset2D{ static_cast<int32_t>(f.viewport.x), static_cast<int32_t>(f.viewport.y) };
            scissor.extent = VkExtent2D{ static_cast<uint32_t>(f.viewport.width), static_cast<uint32_t>(f.viewport.height) };
            d.CmdSetViewport(cmd, 0, 1, &viewport);
            d.CmdSetScissor(cmd, 0, 1, &scissor);
            d.CmdDraw(cmd, 3, 1, 0, 0);
            d.CmdEndRenderPass(cmd);
            frame::RetireFramebuffer(fb, t.serial);
        }

        // The depth back for the world's next draws.
        Barrier(cmd, t.depth, t.depthAspects, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

        char line[160];
        std::snprintf(line, sizeof line, "drawn: %ux%u target, field %s", t.extent.width, t.extent.height, s.fieldPresent ? "present" : "none (haze only)");
        SetStatus(line);
        return drawn;
    }

    const char* Status() { return s.status; }

    void Shutdown()
    {
        if (!s.dapi || !s.device) return;
        const loader::Device& d = *s.dapi;
        if (s.composite != VK_NULL_HANDLE) d.DestroyPipeline(s.device, s.composite, nullptr);
        if (s.pass != VK_NULL_HANDLE) d.DestroyRenderPass(s.device, s.pass, nullptr);
        if (s.compute != VK_NULL_HANDLE) d.DestroyPipeline(s.device, s.compute, nullptr);
        DestroyImage(s.volume);
        DestroyImage(s.fieldLight);
        DestroyImage(s.fieldPool);
        for (Slot& slot : s.slots) memory::DestroyBuffer(slot.params);
        if (s.pool != VK_NULL_HANDLE) d.DestroyDescriptorPool(s.device, s.pool, nullptr);
        if (s.linearRepeat != VK_NULL_HANDLE) d.DestroySampler(s.device, s.linearRepeat, nullptr);
        if (s.linearClamp != VK_NULL_HANDLE) d.DestroySampler(s.device, s.linearClamp, nullptr);
        if (s.nearestClamp != VK_NULL_HANDLE) d.DestroySampler(s.device, s.nearestClamp, nullptr);
        if (s.layout != VK_NULL_HANDLE) d.DestroyPipelineLayout(s.device, s.layout, nullptr);
        if (s.setLayout != VK_NULL_HANDLE) d.DestroyDescriptorSetLayout(s.device, s.setLayout, nullptr);
        if (s.comp != VK_NULL_HANDLE) d.DestroyShaderModule(s.device, s.comp, nullptr);
        if (s.vert != VK_NULL_HANDLE) d.DestroyShaderModule(s.device, s.vert, nullptr);
        if (s.frag != VK_NULL_HANDLE) d.DestroyShaderModule(s.device, s.frag, nullptr);
        s = State();
    }
}
