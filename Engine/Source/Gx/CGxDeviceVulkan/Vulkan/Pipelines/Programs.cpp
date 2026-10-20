// The shader modules: the backend's own (the SPIR-V of Shaders/*.vert and *.frag, embedded by
// Shaders/Compile.py) and the translated ones, fetched from the manifest's files.
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Pipelines/Internal.hpp"

#include "Engine/Source/Gx/CGxDeviceVulkan/Shaders/Spirv.inc"
#include "Engine/Source/Gx/CGxDeviceVulkan/Shaders/TerrainSpirv.inc"  // SXC: our terrain shader

namespace
{
    using namespace gxvk::vk::pipelines::detail;

    bool CreateModule(const uint32_t* words, size_t bytes, VkShaderModule& out, const char* what)
    {
        VkShaderModuleCreateInfo info = {};
        info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = bytes;
        info.pCode    = words;
        const VkResult r = s.dapi->CreateShaderModule(s.device, &info, nullptr, &out);
        if (r != VK_SUCCESS)
        {
            out = VK_NULL_HANDLE;
            Write(Error, "vulkan: pipelines: vkCreateShaderModule (%s) failed: %s", what, ResultName(r));
            return false;
        }
        return true;
    }

    /// A translated record's module, by its manifest path.
    VkShaderModule RecordModule(uint16_t index)
    {
        const manifest::Record* r = manifest::At(index);
        return r ? shaders::Get(r->spv) : VK_NULL_HANDLE;
    }

    // SXC: our terrain pixel modules, by layers - 1 and shadow group (Shaders/Compile.py's order).
    struct TerrainFragSource
    {
        const uint32_t* words;
        size_t          bytes;
    };
    const TerrainFragSource kTerrainFrags[4][3] = {
        { { kTerrainFrag10, sizeof kTerrainFrag10 }, { kTerrainFrag11, sizeof kTerrainFrag11 }, { kTerrainFrag12, sizeof kTerrainFrag12 } },
        { { kTerrainFrag20, sizeof kTerrainFrag20 }, { kTerrainFrag21, sizeof kTerrainFrag21 }, { kTerrainFrag22, sizeof kTerrainFrag22 } },
        { { kTerrainFrag30, sizeof kTerrainFrag30 }, { kTerrainFrag31, sizeof kTerrainFrag31 }, { kTerrainFrag32, sizeof kTerrainFrag32 } },
        { { kTerrainFrag40, sizeof kTerrainFrag40 }, { kTerrainFrag41, sizeof kTerrainFrag41 }, { kTerrainFrag42, sizeof kTerrainFrag42 } },
    };

    // SXC: boolean / integer constants as 32-bit values, ids 0.. in order.
    const VkSpecializationInfo* Specialize(TerrainSpecialization& sp, const uint32_t* values, uint32_t count)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            sp.values[i]           = values[i];
            sp.entries[i].constantID = i;
            sp.entries[i].offset     = i * sizeof(uint32_t);
            sp.entries[i].size       = sizeof(uint32_t);
        }
        sp.info.mapEntryCount = count;
        sp.info.pMapEntries   = sp.entries;
        sp.info.dataSize      = count * sizeof(uint32_t);
        sp.info.pData         = sp.values;
        return &sp.info;
    }
}

namespace gxvk::vk::pipelines::detail
{
    bool CreatePrograms()
    {
        return CreateModule(kFfVert, sizeof kFfVert, s.vert[static_cast<uint32_t>(VertexProgram::FixedFunction)], "Ff.vert")
            && CreateModule(kUiVert, sizeof kUiVert, s.vert[static_cast<uint32_t>(VertexProgram::Ui)], "Ui.vert")
            && CreateModule(kFfSemVert, sizeof kFfSemVert, s.vert[static_cast<uint32_t>(VertexProgram::FixedFunctionSemantic)], "FfSem.vert")
            && CreateModule(kFfFrag, sizeof kFfFrag, s.frag[static_cast<uint32_t>(FragmentProgram::FixedFunction)], "Ff.frag")
            && CreateModule(kUiFrag, sizeof kUiFrag, s.frag[static_cast<uint32_t>(FragmentProgram::Ui)], "Ui.frag")
            && CreateModule(kDesaturateFrag, sizeof kDesaturateFrag, s.frag[static_cast<uint32_t>(FragmentProgram::Desaturate)], "Desaturate.frag")
            && CreateModule(kFfTranslatedFrag, sizeof kFfTranslatedFrag, s.frag[static_cast<uint32_t>(FragmentProgram::FixedFunctionTranslated)], "FfTranslated.frag")
            && CreateTerrainModules();  // SXC:
    }

    // SXC: our terrain shader's modules.
    bool CreateTerrainModules()
    {
        if (!CreateModule(kTerrainVert, sizeof kTerrainVert, s.terrainVert, "Terrain.vert")) return false;
        for (uint32_t l = 0; l < 4; ++l)
            for (uint32_t g = 0; g < 3; ++g)
                if (!CreateModule(kTerrainFrags[l][g].words, kTerrainFrags[l][g].bytes, s.terrainFrag[l][g], "Terrain.frag")) return false;
        return true;
    }

    const VkSpecializationInfo* VertexSpecialization(const Key& k, TerrainSpecialization& storage)
    {
        if (k.vertex != static_cast<uint32_t>(VertexProgram::Terrain)) return nullptr;
        const uint32_t o = k.terrainVertex;
        const uint32_t values[6] = {
            terrain::Layers(o), (o & terrain::kVertexShadow) ? 1u : 0u, (o & terrain::kVertexEnv) ? 1u : 0u,
            (o & terrain::kVertexColor) ? 1u : 0u, (o & terrain::kVertexSpecular) ? 1u : 0u, (o & terrain::kVertexPointLights) ? 1u : 0u,
        };
        return Specialize(storage, values, 6);
    }

    const VkSpecializationInfo* FragmentSpecialization(const Key& k, TerrainSpecialization& storage)
    {
        if (k.fragment != static_cast<uint32_t>(FragmentProgram::Terrain)) return nullptr;
        const uint32_t o = k.terrainFragment;
        const uint32_t values[3] = {
            (o & terrain::kFragmentBigAlpha) ? 1u : 0u, (o & terrain::kFragmentUnlit) ? 1u : 0u, (o & terrain::kFragmentNineTaps) ? 1u : 0u,
        };
        return Specialize(storage, values, 3);
    }

    void DestroyPrograms()
    {
        for (VkShaderModule& m : s.vert)
        {
            if (m != VK_NULL_HANDLE) s.dapi->DestroyShaderModule(s.device, m, nullptr);
            m = VK_NULL_HANDLE;
        }
        for (VkShaderModule& m : s.frag)
        {
            if (m != VK_NULL_HANDLE) s.dapi->DestroyShaderModule(s.device, m, nullptr);
            m = VK_NULL_HANDLE;
        }
        // SXC: our terrain shader's modules.
        if (s.terrainVert != VK_NULL_HANDLE) s.dapi->DestroyShaderModule(s.device, s.terrainVert, nullptr);
        s.terrainVert = VK_NULL_HANDLE;
        for (auto& row : s.terrainFrag)
            for (VkShaderModule& m : row)
            {
                if (m != VK_NULL_HANDLE) s.dapi->DestroyShaderModule(s.device, m, nullptr);
                m = VK_NULL_HANDLE;
            }
    }

    /// The vertex module of a key: the backend's own for its program, the record's for Translated.
    VkShaderModule VertexModule(const Key& k)
    {
        if (k.vertex == static_cast<uint32_t>(VertexProgram::Translated)) return RecordModule(k.vertexRecord);
        if (k.vertex == static_cast<uint32_t>(VertexProgram::Terrain)) return s.terrainVert;  // SXC:
        return k.vertex < kVertexPrograms ? s.vert[k.vertex] : VK_NULL_HANDLE;
    }

    VkShaderModule FragmentModule(const Key& k)
    {
        if (k.fragment == static_cast<uint32_t>(FragmentProgram::Translated)) return RecordModule(k.fragmentRecord);
        // SXC: our terrain pixel module of the options' layers and shadow group.
        if (k.fragment == static_cast<uint32_t>(FragmentProgram::Terrain))
        {
            const uint32_t group = terrain::ShadowGroup(k.terrainFragment);
            return group < 3 ? s.terrainFrag[terrain::Layers(k.terrainFragment) - 1][group] : VK_NULL_HANDLE;
        }
        return k.fragment < kFragmentPrograms ? s.frag[k.fragment] : VK_NULL_HANDLE;
    }
}
