// The instanced grass (SXC/Grass/Grass.h).

#include "SXC/Grass/Grass.h"

#include <cmath>
#include <cstring>
#include <string>
#include "SXC/Api/sxc_grass.h"

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
        uint32_t generation = 0;   // the motion table's when built (step 7b)
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

    // --- the motion (step 7b; WXL's features/GrassDoodads and GrassMotion) ------------------------
    // Per plant model: the bend from its root to its tip in texture V, found from its geometry (the mean
    // V of the lowest and of the highest 12% of its vertices), unless the mod's table overrides it;
    // models under 0.15 yd don't sway. Its plants carry {root V, 1 / (tip V - root V), wind scale, push
    // scale}; a model with no usable mapping gets WXL's fallback (root at V 1, tip at V 0).
    constexpr float kRootTipBand = 0.12f;
    constexpr float kFlatHeight = 0.15f;

    struct Doodad {
        bool analyzed = false, valid = false, flat = false;
        float height = 0.0f, rootV = 1.0f, tipV = 0.0f;
        std::string path;
    };
    std::unordered_map<uint32_t, Doodad> s_doodads;
    std::unordered_map<uint32_t, sxc_grass_doodad> s_overrides;
    uint32_t s_generation = 1;   // bumped when the overrides change: every chunk is placed anew

    Doodad& Analyze(uint32_t id) {
        Doodad& d = s_doodads[id];

        if (d.analyzed || id >= CDetailDoodad::s_doodadList.Count()) {
            return d;
        }

        const CDetailDoodadData* data = CDetailDoodad::s_doodadList[id];
        CM2Model* model = data ? data->m_model : nullptr;

        if (!model || !(model->m_fFlags & 1) || !model->m_shared || !model->m_shared->m_skinData || !model->m_shared->m_data) {
            return d;
        }

        d.analyzed = true;
        d.path = model->m_shared->m_fileName ? model->m_shared->m_fileName : "";
        const M2SkinProfile* skin = model->m_shared->m_skinData;
        const M2Data* m2 = model->m_shared->m_data;
        float minZ = 1e30f, maxZ = -1e30f;

        for (uint32_t v = 0; v < skin->m_vertices.m_count; v++) {
            const uint16_t g = skin->m_vertices[v];

            if (g < m2->m_vertices.m_count) {
                minZ = std::min(minZ, m2->m_vertices[g].m_position.z);
                maxZ = std::max(maxZ, m2->m_vertices[g].m_position.z);
            }
        }

        if (minZ > maxZ) {
            return d;
        }

        const float band = (maxZ - minZ) * kRootTipBand;
        float rootSum = 0.0f, tipSum = 0.0f;
        uint32_t rootN = 0, tipN = 0;

        for (uint32_t v = 0; v < skin->m_vertices.m_count; v++) {
            const uint16_t g = skin->m_vertices[v];

            if (g >= m2->m_vertices.m_count) {
                continue;
            }

            const float z = m2->m_vertices[g].m_position.z, t = m2->m_vertices[g].m_texcoord[0].y;

            if (z <= minZ + band) {
                rootSum += t;
                rootN++;
            }

            if (z >= maxZ - band) {
                tipSum += t;
                tipN++;
            }
        }

        if (!rootN || !tipN) {
            return d;
        }

        d.height = maxZ - minZ;
        d.rootV = rootSum / rootN;
        d.tipV = tipSum / tipN;
        d.flat = d.height < kFlatHeight;
        d.valid = std::fabs(d.tipV - d.rootV) > 0.01f || d.flat;
        return d;
    }

    // What the plants of a model carry (false: its model isn't read yet).
    bool Entry(uint32_t id, float out[4]) {
        out[0] = 1.0f;
        out[1] = -1.0f;
        out[2] = 1.0f;
        out[3] = 1.0f;
        const Doodad& d = Analyze(id);

        if (!d.analyzed) {
            return false;
        }

        const auto o = s_overrides.find(id);
        float rootV = d.rootV, tipV = d.tipV, stiffness = 0.0f;
        bool wind = d.valid && !d.flat, push = d.valid && !d.flat, flip = false;

        if (o != s_overrides.end()) {
            const sxc_grass_doodad& r = o->second;
            rootV = r.root_v >= 0.0f ? r.root_v : rootV;
            tipV = r.tip_v >= 0.0f ? r.tip_v : tipV;
            const bool usable = d.valid || (r.root_v >= 0.0f && r.tip_v >= 0.0f);
            wind = usable && !(r.flags & SXC_GRASS_NO_WIND);
            push = usable && !(r.flags & SXC_GRASS_NO_PUSH);
            flip = (r.flags & SXC_GRASS_FLIP) != 0;
            stiffness = std::min(std::max(r.stiffness, 0.0f), 1.0f);
        } else if (!d.valid) {
            return true;   // the fallback
        }

        const float root = flip ? tipV : rootV, tip = flip ? rootV : tipV, span = tip - root, flex = 1.0f - stiffness;
        out[0] = root;
        out[1] = std::fabs(span) > 1e-4f ? 1.0f / span : 0.0f;
        out[2] = wind ? flex : 0.0f;
        out[3] = push ? flex : 0.0f;
        return true;
    }

    // The rows the motion reads from c64 on (Shaders/GrassMotion.glsl).
    constexpr uint32_t kMotionFirstRow = 64;
    constexpr uint32_t kMotionRows = 4 + SXC_GRASS_GRID * SXC_GRASS_GRID + SXC_GRASS_MAX_ACTORS;
    float s_motionRows[kMotionRows][4] = {};
    bool s_motionOn = false;

    void SetMotion(const sxc_grass_motion& m) {
        std::memset(s_motionRows, 0, sizeof(s_motionRows));
        s_motionOn = m.on != WXL_FALSE;

        if (!s_motionOn) {
            return;
        }

        const float anchor = std::min(std::max(m.anchor, 0.0f), 0.9f);
        const float inv = m.grid_spacing > 0.01f ? 1.0f / m.grid_spacing : 0.125f;
        const float h[4][4] = {
            { m.time, anchor, 1.0f / (1.0f - anchor), m.amplitude },
            { m.flutter, m.flutter_speed, m.push_strength, 1.0f },
            { inv, inv, m.grid_origin[0] * inv + 0.5f, m.grid_origin[1] * inv + 0.5f },
            { m.player[0], m.player[1], m.player[2], m.actor_range * m.actor_range },
        };
        std::memcpy(s_motionRows, h, sizeof(h));

        for (uint32_t c = 0; c < SXC_GRASS_GRID * SXC_GRASS_GRID; c++) {
            std::memcpy(s_motionRows[4 + c], m.wind[c], sizeof(m.wind[c]));
        }

        for (uint32_t a = 0; a < SXC_GRASS_MAX_ACTORS && a < m.actor_count; a++) {
            std::memcpy(s_motionRows[4 + SXC_GRASS_GRID * SXC_GRASS_GRID + a], m.actors[a], sizeof(m.actors[a]));
        }
    }

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
        sorted.resize(static_cast<size_t>(total) * 20);
        c.generation = s_generation;
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
                float* plant = sorted.data() + static_cast<size_t>(written + k) * 20;
                std::copy(base + static_cast<size_t>(order[k]) * 16, base + static_cast<size_t>(order[k]) * 16 + 16, plant);
                Entry(id, plant + 16);   // step 7b: the motion values

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

        if (c.adds != AddsOf(inst) || !c.ok || c.generation != s_generation) {
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

        // Step 7b: the motion's rows (after the client's c0..c22), every frame: other shaders use them too.
        g_theGxDevicePtr->ShaderConstantsSet(GxSh_Vertex, kMotionFirstRow, &s_motionRows[0][0], kMotionRows);

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

// --- sxc.grass ----------------------------------------------------------------------------------------
namespace {
    wxl_bool WXL_CALL ApiAvailable() {
        return SXC::Grass::Active() ? WXL_TRUE : WXL_FALSE;
    }

    void WXL_CALL ApiSetMotion(const sxc_grass_motion* motion) {
        if (!motion || motion->struct_size < sizeof(sxc_grass_motion)) {
            s_motionOn = false;
            std::memset(s_motionRows, 0, sizeof(s_motionRows));
            return;
        }

        SetMotion(*motion);
    }

    void WXL_CALL ApiSetDoodads(const sxc_grass_doodad* rows, uint32_t count) {
        s_overrides.clear();

        for (uint32_t i = 0; rows && i < count; i++) {
            s_overrides[rows[i].id] = rows[i];
        }

        if (++s_generation == 0) {
            s_generation = 1;
        }
    }

    wxl_bool WXL_CALL ApiDoodadInfo(uint32_t id, sxc_grass_doodad_info* out) {
        if (!out || out->struct_size < sizeof(sxc_grass_doodad_info) || id >= CDetailDoodad::s_doodadList.Count() || !CDetailDoodad::s_doodadList[id]) {
            return WXL_FALSE;
        }

        const uint32_t size = out->struct_size;
        *out = sxc_grass_doodad_info{};
        out->struct_size = size;
        const Doodad& d = Analyze(id);
        static std::string path;
        path = d.path;
        out->path = path.c_str();
        out->analyzed = d.analyzed ? WXL_TRUE : WXL_FALSE;
        out->height = d.height;
        out->root_v = d.rootV;
        out->tip_v = d.tipV;
        out->flat = d.flat ? WXL_TRUE : WXL_FALSE;
        Entry(id, out->entry);
        return WXL_TRUE;
    }

    const char* WXL_CALL ApiStatus() {
        return SXC::Grass::Status();
    }

    const sxc_grass_api s_api = { sizeof(sxc_grass_api), ApiAvailable, ApiSetMotion, ApiSetDoodads, ApiDoodadInfo, ApiStatus };
}

namespace SXC::Grass {
    const sxc_grass_api* Api() {
        return &s_api;
    }
}
