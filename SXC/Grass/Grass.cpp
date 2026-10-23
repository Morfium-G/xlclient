// The instanced grass (SXC/Grass/Grass.h).

#include "SXC/Grass/Grass.h"

#include <algorithm>
#include <cstdio>
#include <unordered_map>
#include <vector>
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Model2/M2Data.h"
#include "Engine/Source/Model2/M2Model.h"
#include "Engine/Source/Model2/M2Shared.h"
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include "SXC/Log.h"
#include "SXC/Settings/Settings.h"
#include "WoW/Source/WorldClient/CDetailDoodadData.h"
#include "WoW/Source/WorldClient/DetailDoodad.h"
#include "WoW/Source/WorldClient/MapChunk.h"
#include "WoW/Source/WorldClient/World.h"

namespace {
    const SXC::Grass::Device* s_device = nullptr;

    SXC::Settings::Bool& Setting() {
        static SXC::Settings::Bool s_on("Grass", "Instanced", true, "Grass: instanced",
                                        "The ground-effect plants drawn instanced on the GPU (each chunk's plants uploaded once) instead of "
                                        "the client re-baking them on the CPU every frame. Vulkan only; off: the stock grass.");
        return s_on;
    }

    // A plant model on the device (by GroundEffectDoodad id).
    struct Model {
        bool ok = false;
        uint32_t firstIndex = 0, indexCount = 0;
        int32_t vertexOffset = 0;
    };
    std::unordered_map<uint32_t, Model> s_models;

    // A chunk's plants in the pool: per batch (texture) and plant model, a contiguous run.
    struct Group {
        CTexture* texture;
        uint32_t doodadId;
        uint32_t offset;   // into the chunk's range
        uint32_t count;
    };
    struct Chunk {
        bool ok = false;
        uint32_t first = 0, count = 0;
        uint32_t adds = 0;   // the batches' adds when built (a change rebuilds)
        std::vector<Group> groups;
    };
    std::unordered_map<const CDetailDoodadInst*, Chunk> s_chunks;

    // This frame's runs, per texture.
    struct TextureRuns {
        CTexture* texture;
        std::vector<SXC::Grass::Run> runs;
    };
    std::vector<TextureRuns> s_frame;
    bool s_collecting = false;

    struct Stats {
        uint32_t chunks = 0, plants = 0, runs = 0, draws = 0, built = 0, stock = 0;
    } s_stats, s_last;
    char s_status[256] = "";

    uint32_t AddsOf(CDetailDoodadInst* inst) {
        uint32_t n = 0;

        for (CDetailDoodadInstBatch& b : inst->m_batches) {
            n += b.m_texture ? b.m_adds.Count() : 0;
        }

        return n;
    }

    // The plant model's geometry on the device: every index of its skin, its skin's vertices (position,
    // first texture coordinate), as FillIndices / FillVertices take them.
    const Model& ModelFor(uint32_t id) {
        Model& m = s_models[id];

        if (m.ok || id >= CDetailDoodad::s_doodadList.Count()) {
            return m;
        }

        const CDetailDoodadData* data = CDetailDoodad::s_doodadList[id];
        CM2Model* model = data ? data->m_model : nullptr;

        if (!model || !(model->m_fFlags & 1) || !model->m_shared || !model->m_shared->m_skinData || !model->m_shared->m_data) {
            return m;
        }

        const M2SkinProfile* skin = model->m_shared->m_skinData;
        const M2Data* m2 = model->m_shared->m_data;
        static std::vector<float> vertices;
        static std::vector<uint16_t> indices;
        vertices.clear();
        indices.clear();

        for (uint32_t v = 0; v < skin->m_vertices.m_count; v++) {
            const uint16_t global = skin->m_vertices[v];
            const M2Vertex& src = global < m2->m_vertices.m_count ? m2->m_vertices[global] : m2->m_vertices[0];
            vertices.insert(vertices.end(), { src.m_position.x, src.m_position.y, src.m_position.z, src.m_texcoord[0].x, src.m_texcoord[0].y });
        }

        for (uint32_t i = 0; i < skin->m_indices.m_count; i++) {
            indices.push_back(skin->m_indices[i]);
        }

        m.ok = s_device->mesh(id, vertices.data(), skin->m_vertices.m_count, indices.data(), static_cast<uint32_t>(indices.size()), m.firstIndex, m.vertexOffset,
                              m.indexCount);
        return m;
    }

    void Release(Chunk& c) {
        if (c.count && s_device) {
            s_device->release(c.first, c.count);
        }

        c = Chunk{};
    }

    // A chunk's plants into the pool: each batch's plants grouped by model (contiguous runs), placed in
    // world space (the chunk's corner added to the stock bake's chunk space).
    bool Build(CDetailDoodadInst* inst, Chunk& c) {
        const uint32_t total = AddsOf(inst);
        c.adds = total;

        if (!total || !inst->m_chunk) {
            c.ok = total == 0;
            return c.ok;
        }

        if (!s_device->allocate(total, c.first)) {
            return false;
        }

        c.count = total;
        static std::vector<float> plants, sorted;
        plants.resize(static_cast<size_t>(total) * 16);
        sorted.resize(plants.size());
        uint32_t written = 0;

        for (CDetailDoodadInstBatch& b : inst->m_batches) {
            if (!b.m_texture || !b.m_adds.Count()) {
                continue;
            }

            float* base = plants.data() + static_cast<size_t>(written) * 16;
            SxcGrassPlants(b, inst->m_chunk->m_topLeftCoords, base);

            // The batch's plants by model, in their order.
            std::vector<uint32_t> order(b.m_adds.Count());

            for (uint32_t i = 0; i < order.size(); i++) {
                order[i] = i;
            }

            std::stable_sort(order.begin(), order.end(), [&](uint32_t x, uint32_t y) { return b.m_adds[x].m_doodadId < b.m_adds[y].m_doodadId; });

            for (uint32_t k = 0; k < order.size(); k++) {
                const uint32_t id = b.m_adds[order[k]].m_doodadId;
                std::copy(base + static_cast<size_t>(order[k]) * 16, base + static_cast<size_t>(order[k]) * 16 + 16, sorted.data() + static_cast<size_t>(written + k) * 16);

                if (c.groups.empty() || c.groups.back().doodadId != id || c.groups.back().texture != b.m_texture || k == 0) {
                    c.groups.push_back(Group{ b.m_texture, id, written + k, 0 });
                }

                c.groups.back().count++;
            }

            written += b.m_adds.Count();
        }

        s_device->write(c.first, sorted.data(), total);
        c.ok = true;
        s_stats.built++;
        return true;
    }

    TextureRuns& RunsFor(CTexture* texture) {
        for (TextureRuns& t : s_frame) {
            if (t.texture == texture) {
                return t;
            }
        }

        s_frame.push_back(TextureRuns{ texture, {} });
        return s_frame.back();
    }
}

namespace SXC::Grass {
    void SetDevice(const Device* device) {
        if (device == s_device) {
            return;
        }

        // A new device (or none): what the old one held is gone with it.
        s_device = device;
        s_models.clear();
        s_chunks.clear();
        s_frame.clear();
    }

    bool Enabled() {
        return Setting().Get();
    }

    void SetEnabled(bool on) {
        Setting().Set(on);
    }

    bool Active() {
        return s_device && CDetailDoodad::s_bUseShaders && Enabled();
    }

    void Begin() {
        s_collecting = Active();

        for (TextureRuns& t : s_frame) {
            t.runs.clear();
        }

        s_stats = Stats{};
    }

    bool Take(CDetailDoodadInst* inst) {
        if (!s_collecting || !inst) {
            return false;
        }

        Chunk& c = s_chunks[inst];

        if (c.adds != AddsOf(inst) || !c.ok) {
            Release(c);

            if (!Build(inst, c)) {
                Release(c);
                s_stats.stock++;
                return false;
            }
        }

        s_stats.chunks++;

        for (const Group& g : c.groups) {
            const Model& m = ModelFor(g.doodadId);

            if (!m.ok) {
                continue;
            }

            RunsFor(g.texture).runs.push_back(Run{ m.firstIndex, m.indexCount, m.vertexOffset, c.first + g.offset, g.count });
            s_stats.plants += g.count;
            s_stats.runs++;
        }

        return true;
    }

    void End() {
        if (!s_collecting) {
            return;
        }

        s_collecting = false;

        // One world matrix for all: the plants are in world space, the view is the scene's.
        C44Matrix world;
        world.Translate(C3Vector(0.0f, 0.0f, 0.0f) - CWorldScene::s_activeWorldView);
        CDetailDoodad::SetupVertexShader(world, 0);

        for (const TextureRuns& t : s_frame) {
            if (t.runs.empty()) {
                continue;
            }

            CGxTex* gxTex = TextureGetGxTex(t.texture, 0, nullptr);

            if (gxTex && s_device->draw(gxTex, t.runs.data(), static_cast<uint32_t>(t.runs.size()))) {
                s_stats.draws++;
            }
        }

        s_last = s_stats;
    }

    void Forget(CDetailDoodadInst* inst) {
        const auto it = s_chunks.find(inst);

        if (it != s_chunks.end()) {
            Release(it->second);
            s_chunks.erase(it);
        }
    }

    const char* Status() {
        if (!s_device) {
            return "off: this renderer draws the stock grass (Vulkan only)";
        }

        if (!Active()) {
            return CDetailDoodad::s_bUseShaders ? "off ([Grass] Instanced)" : "off: the client's grass shaders are off";
        }

        std::snprintf(s_status, sizeof(s_status), "%u chunks, %u plants in %u runs, %u draws; %u chunks built this frame, %u drawn the stock way; pool %u plants",
                      s_last.chunks, s_last.plants, s_last.runs, s_last.draws, s_last.built, s_last.stock, s_device->plantsInUse());
        return s_status;
    }
}
