// sxc.cover (SXC/Api/sxc_cover.h) and its seam in the terrain chunk draw (SXC/Cover/Cover.h).

#include "SXC/Cover/Cover.h"
#include "SXC/Cover/Trails.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include "SXC/Api/sxc_cover.h"
#include "SXC/Log.h"
#include "Engine/Source/Base/Handle.h"
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/Storm/H/storm.h"
#include "WoW/Source/DB/DBClient/DBClient.h"
#include "WoW/Source/DB/Rec/GroundEffectTextureRec.h"
#include "WoW/Source/WorldClient/CChunkLiquid.h"
#include "WoW/Source/WorldClient/CMapBaseObjLink.h"
#include "WoW/Source/WorldClient/Map.h"
#include "WoW/Source/WorldClient/MapArea.h"
#include "WoW/Source/WorldClient/MapChunk.h"
#include "WoW/Source/WorldClient/MapRenderChunk.h"
#include "WoW/Source/WorldClient/Types.h"
#include "WoW/Source/WorldClient/World.h"

namespace {
    using namespace SXC::Cover;

    static_assert(sizeof(sxc_cover_entry) == 80, "the terrain shader reads entries as five vec4");
    static_assert(SXC_COVER_MAX_ENTRIES == kMaxEntries && SXC_COVER_MAX_TEXTURES == kMaxTextures, "SXC/Cover/Cover.h follows the API");

    bool s_available = false;
    uint32_t s_slots = 0; // texture slots the device's shader has
    StatusFn s_deviceStatus = nullptr;

    sxc_cover_resolve_fn s_resolve = nullptr;
    void* s_resolveUser = nullptr;
    uint32_t s_generation = 1; // the resolver's answers; a chunk cached under another one asks again

    sxc_cover_look s_look = {};
    bool s_haveLook = false;

    std::vector<sxc_cover_entry> s_entries;
    uint32_t s_entryVersion = 1;

    struct Map {
        std::vector<float> cells;
        uint32_t size = 0;
        float cell = 1.0f;
        int32_t firstI = 0, firstJ = 0;
        uint32_t version = 1;
        float mostFallen = 0.0f; // yd: the deepest fallen snow in it
    };
    Map s_near, s_far;

    // The cover textures: loaded through the client's texture cache like the terrain's layers, when a
    // drawn chunk first shows them, then kept. A gloss texture tries its "_s" variant first (where later
    // tilesets keep the gloss mask in alpha). With fewer slots than textures (no per-pixel indexing),
    // the slots go to the textures the chunks drawn use most; one keeps its slot while it's in view.
    struct Texture {
        std::string path;
        uint32_t flags = 0;
        CTexture* texture = nullptr;
        bool specular = false;  // the "_s" variant was found
        bool failed = false;    // tried and couldn't be loaded
        uint32_t uses = 0;      // chunks drawn showing it since the slots were last assigned
        uint64_t lastUsed = 0;  // the frame it was last shown
        uint32_t slot = 0;      // 1.. while it has one
    };
    std::vector<Texture> s_textures;
    uint32_t s_textureCount = 0;
    uint64_t s_frameNo = 0;
    uint32_t s_wanted = 0, s_unslotted = 0; // last assignment: textures in view, and those without a slot
    std::vector<uint32_t> s_slotTexture;   // per slot (fallback): the texture holding it, 0 free
    constexpr uint64_t kKeepFrames = 120;  // a texture out of view this long gives its slot up

    // Per render chunk: what the resolver answered, and what it was asked about (a chunk object is
    // reused for other ground as the map streams).
    struct Cached {
        const CMapChunk* chunks[2] = {};
        int32_t textureIds[SXC_COVER_MAX_LAYERS] = {};
        uint32_t layerCount = 0;
        uint32_t generation = 0;
        float painted[4] = {}, snow[4] = {};
    };
    std::unordered_map<const CMapRenderChunk*, Cached> s_chunks;
    uint64_t s_resolved = 0, s_draws = 0, s_withCover = 0;

    Frame s_frame;
    char s_status[512] = "";

    void Release(Texture& t) {
        if (t.texture) {
            HandleClose(reinterpret_cast<HOBJECT>(t.texture));
        }

        t = Texture{};
    }

    CTexture* LoadTexture(Texture& t) {
        t.specular = false;

        if ((t.flags & SXC_COVER_TEXTURE_GLOSS) && t.path.size() > 4 && SStrCmpI(t.path.c_str() + t.path.size() - 4, ".blp", 4) == 0) {
            const std::string gloss = t.path.substr(0, t.path.size() - 4) + "_s.blp";

            if (SFile::FileExists(gloss.c_str())) {
                if (CTexture* texture = CMap::LoadTexture(gloss.c_str())) {
                    t.specular = true;
                    return texture;
                }
            }
        }

        return CMap::LoadTexture(t.path.c_str());
    }

    // The area tile a chunk belongs to (CMapRenderChunk::CreateLayers' way).
    CMapArea* AreaOf(const CMapChunk* chunk) {
        CMapBaseObjLink* link = chunk ? const_cast<CMapChunk*>(chunk)->m_parentLinkList.Head() : nullptr;
        return link ? static_cast<CMapArea*>(link->m_ref) : nullptr;
    }

    // A render layer's description, from the map chunk whose layers name its texture.
    bool DescribeLayer(const CMapRenderChunk* rc, int32_t textureId, sxc_cover_layer& out) {
        for (const CMapChunk* chunk : rc->m_mapChunks) {
            if (!chunk || !chunk->m_layers || !chunk->m_header) {
                continue;
            }

            CMapArea* area = AreaOf(chunk);

            for (uint32_t l = 0; l < chunk->m_header->m_nLayers; l++) {
                const SMLayer& layer = chunk->m_layers[l];

                if (static_cast<int32_t>(layer.m_textureId) != textureId) {
                    continue;
                }

                out.texture = area && layer.m_textureId < area->m_textures.Count() && area->m_textures[layer.m_textureId].m_textureName
                                  ? area->m_textures[layer.m_textureId].m_textureName
                                  : "";
                out.ground_effect = layer.m_offectId;
                out.terrain_type = -1;

                if (const GroundEffectTextureRec* rec = g_groundEffectTextureDB.GetRecord(static_cast<int32_t>(layer.m_offectId))) {
                    out.terrain_type = rec->m_sound; // GroundEffectTexture's last column: the TerrainType
                }

                return true;
            }
        }

        return false;
    }

    void Resolve(const CMapRenderChunk* rc, Cached& c) {
        sxc_cover_chunk q = {};
        q.struct_size = sizeof(q);
        q.map_id = ::World::QueryMapId();
        q.area = rc->m_mapChunks[0] ? static_cast<uint32_t>(rc->m_mapChunks[0]->m_areaId) : 0u;
        q.layer_count = static_cast<int32_t>(c.layerCount);

        for (uint32_t l = 0; l < c.layerCount; l++) {
            q.layers[l] = sxc_cover_layer{ "", 0, -1 };
            DescribeLayer(rc, c.textureIds[l], q.layers[l]);
        }

        s_resolve(s_resolveUser, &q);
        s_resolved++;

        for (uint32_t l = 0; l < SXC_COVER_MAX_LAYERS; l++) {
            const bool have = l < c.layerCount;
            c.painted[l] = have && q.painted[l] <= kMaxEntries ? static_cast<float>(q.painted[l]) : 0.0f;
            c.snow[l] = have && q.snow_look[l] <= kMaxEntries ? static_cast<float>(q.snow_look[l]) : 0.0f;
        }
    }

    // --- the service --------------------------------------------------------------------------------

    wxl_bool WXL_CALL Available() {
        return s_available ? WXL_TRUE : WXL_FALSE;
    }

    void WXL_CALL SetResolver(sxc_cover_resolve_fn resolve, void* user) {
        s_resolve = resolve;
        s_resolveUser = user;
        s_generation++;
    }

    void WXL_CALL Invalidate() {
        s_generation++;
    }

    void WXL_CALL SetEntries(const sxc_cover_entry* entries, uint32_t count) {
        count = entries ? (count < kMaxEntries ? count : kMaxEntries) : 0;
        s_entries.assign(entries, entries + count);
        s_entryVersion++;
    }

    void WXL_CALL SetTextures(const char* const* paths, const uint32_t* flags, uint32_t count) {
        count = paths ? (count < kMaxTextures ? count : kMaxTextures) : 0;

        // A texture whose path and flags stay keeps what's loaded (the mod sends the list again whenever
        // its table reloads); the others load when they're next shown.
        for (uint32_t k = count; k < s_textures.size(); k++) {
            Release(s_textures[k]);
        }

        s_textures.resize(count);

        for (uint32_t k = 0; k < count; k++) {
            Texture& t = s_textures[k];
            const char* path = paths[k] ? paths[k] : "";
            const uint32_t f = flags ? flags[k] : 0u;

            if (t.path != path || t.flags != f) {
                Release(t);
                t.path = path;
                t.flags = f;
            }
        }

        s_textureCount = count;
        s_slotTexture.clear();
    }

    // The slots for this frame from what the chunks showed: every texture its own where the device has a
    // slot for each, else the most shown ones (a texture in view keeps its slot).
    void AssignSlots() {
        s_frameNo++;
        s_wanted = 0;
        s_unslotted = 0;

        // Shown textures load (once).
        for (uint32_t k = 0; k < s_textureCount; k++) {
            Texture& t = s_textures[k];

            if (t.uses == 0 || t.texture || t.failed || t.path.empty()) {
                continue;
            }

            t.texture = LoadTexture(t);
            t.failed = !t.texture;

            if (t.failed) {
                SXC::LogOnce((std::string("cover-texture|") + t.path).c_str(), "cover: the texture \"%s\" could not be loaded", t.path.c_str());
            }
        }

        if (s_slots >= s_textureCount) {
            for (uint32_t k = 0; k < s_textureCount; k++) {
                Texture& t = s_textures[k];
                t.slot = t.texture ? k + 1 : 0;
                s_wanted += t.uses > 0;
                t.uses = 0;
            }

            return;
        }

        if (s_slotTexture.size() != s_slots) {
            s_slotTexture.assign(s_slots, 0);

            for (Texture& t : s_textures) {
                t.slot = 0;
            }
        }

        // Residents out of view for a while give their slot up.
        for (uint32_t s = 0; s < s_slots; s++) {
            const uint32_t id = s_slotTexture[s];

            if (id && (id > s_textureCount || !s_textures[id - 1].texture || s_frameNo - s_textures[id - 1].lastUsed > kKeepFrames)) {
                if (id <= s_textureCount) {
                    s_textures[id - 1].slot = 0;
                }

                s_slotTexture[s] = 0;
            }
        }

        // The shown ones without a slot, most shown first, into the free slots.
        std::vector<uint32_t> waiting;

        for (uint32_t k = 0; k < s_textureCount; k++) {
            const Texture& t = s_textures[k];
            s_wanted += t.uses > 0;

            if (t.uses > 0 && t.texture && !t.slot) {
                waiting.push_back(k + 1);
            }
        }

        std::sort(waiting.begin(), waiting.end(), [](uint32_t a, uint32_t b) { return s_textures[a - 1].uses > s_textures[b - 1].uses; });
        size_t next = 0;

        for (uint32_t s = 0; s < s_slots && next < waiting.size(); s++) {
            if (!s_slotTexture[s]) {
                s_slotTexture[s] = waiting[next];
                s_textures[waiting[next] - 1].slot = s + 1;
                next++;
            }
        }

        s_unslotted = static_cast<uint32_t>(waiting.size() - next);

        for (Texture& t : s_textures) {
            t.uses = 0;
        }
    }

    void SetMap(Map& map, const sxc_cover_map* in) {
        map.version++;

        map.mostFallen = 0.0f;

        if (!in || in->struct_size < sizeof(sxc_cover_map) || !in->cells || in->size == 0 || in->size > SXC_COVER_MAX_MAP) {
            map.size = 0;
            map.cells.clear();
            return;
        }

        const size_t floats = static_cast<size_t>(in->size) * in->size * 4;
        map.cells.resize(floats);

        for (size_t i = 0; i < floats; i++) {
            map.cells[i] = std::isfinite(in->cells[i]) ? in->cells[i] : 0.0f;

            if ((i & 3) == 0 && map.cells[i] > map.mostFallen) {
                map.mostFallen = map.cells[i];
            }
        }

        map.size = in->size;
        map.cell = in->cell_size > 0.01f ? in->cell_size : 0.01f;
        map.firstI = in->first_i;
        map.firstJ = in->first_j;
    }

    void WXL_CALL SetNear(const sxc_cover_map* map) { SetMap(s_near, map); }
    void WXL_CALL SetFar(const sxc_cover_map* map) { SetMap(s_far, map); }

    void WXL_CALL SetLook(const sxc_cover_look* look) {
        if (!look || look->struct_size < sizeof(sxc_cover_look)) {
            s_haveLook = false;
            return;
        }

        s_look = *look;
        s_haveLook = true;
    }

    const char* WXL_CALL Status() {
        if (!s_available) {
            return "off: this renderer doesn't draw covers (Vulkan with our terrain shader only)";
        }

        if (!s_haveLook || !s_look.enabled) {
            return "off: no mod turned it on";
        }

        uint32_t loaded = 0;

        for (uint32_t k = 0; k < s_textureCount; k++) {
            loaded += s_textures[k].texture != nullptr;
        }

        char textures[120];

        if (s_slots >= s_textureCount) {
            std::snprintf(textures, sizeof(textures), "textures: %u of %u loaded, %u in view (per-pixel indexing: all show)", loaded, s_textureCount, s_wanted);
        } else {
            std::snprintf(textures, sizeof(textures), "textures: %u of %u loaded, %u in view, %u slots (%u in view without one draw plain)", loaded, s_textureCount,
                          s_wanted, s_slots, s_unslotted);
        }

        char trails[120] = "trails off";

        if (s_look.trails) {
            const Trails::Stats t = Trails::GetStats();
            std::snprintf(trails, sizeof(trails), "trails: %u feet followed, %u carvings last frame, %llu footsteps", t.feet, t.stamps,
                          static_cast<unsigned long long>(t.footsteps));
        }

        std::snprintf(s_status, sizeof(s_status), "%s; chunks drawn %llu (%llu with cover), resolved %llu, %u entries; %s; %s", s_deviceStatus ? s_deviceStatus() : "",
                      static_cast<unsigned long long>(s_draws), static_cast<unsigned long long>(s_withCover), static_cast<unsigned long long>(s_resolved),
                      static_cast<unsigned>(s_entries.size()), textures, trails);
        return s_status;
    }

    int32_t WXL_CALL TextureState(uint32_t texture) {
        if (texture < 1 || texture > s_textureCount || !s_textures[texture - 1].texture) {
            return 0;
        }

        const Texture& t = s_textures[texture - 1];
        return (TextureIsLoaded(t.texture) ? (t.specular ? 3 : 2) : 1) + (t.slot ? 4 : 0);
    }

    const sxc_cover_api s_api = { sizeof(sxc_cover_api), Available, SetResolver, Invalidate, SetEntries, SetTextures, SetNear, SetFar, SetLook, Status, TextureState };
}

namespace SXC::Cover {
    const sxc_cover_api* Api() {
        return &s_api;
    }

    void SetDevice(bool available, uint32_t textureSlots, StatusFn status) {
        s_available = available;
        s_slots = textureSlots < kMaxSlots ? textureSlots : kMaxSlots;
        s_slotTexture.clear();
        s_deviceStatus = status;
        s_near.version++; // a new device holds no maps yet
        s_far.version++;
        s_entryVersion++;
    }

    const Frame& Current() {
        Frame& f = s_frame;
        f.on = s_available && s_haveLook && s_look.enabled;

        if (!f.on) {
            return f;
        }

        f.fallenSnow = s_look.fallen_snow != 0;
        f.debug = s_look.debug;
        f.brightness = s_look.brightness;
        f.wetDarkening = s_look.wet_darkening;
        f.thinDepth = s_look.thin_depth > 0.001f ? s_look.thin_depth : 0.001f;
        f.depthScale = s_look.depth_scale;
        f.breakupScale = s_look.breakup_scale;
        f.driftScale = s_look.drift_scale;
        f.textureTile = s_look.texture_tile > 0.01f ? s_look.texture_tile : 33.3333333f / 8.0f;

        for (int k = 0; k < 3; k++) {
            f.outside[k] = std::isfinite(s_look.outside[k]) ? s_look.outside[k] : 0.0f;
        }

        f.tessDetail = s_look.tess_detail > 0.0f ? (s_look.tess_detail > 0.002f ? s_look.tess_detail : 0.002f) : 0.0f;
        f.tessReach = s_look.tess_reach > 1.0f ? s_look.tess_reach : 1.0f;
        f.tessMost = s_look.tess_most >= 1.0f ? s_look.tess_most : 1.0f;

        // Trails: carved now (the feet's bones are this frame's), handed over with what changed.
        Trails::Settings trails;
        trails.on = s_look.trails != 0 && f.tessDetail > 0.0f;
        trails.reach = s_look.trail_reach > 1.0f ? s_look.trail_reach : 1.0f;
        Trails::Update(trails);
        f.trails = trails.on;
        f.trailNow = Trails::Now();
        f.trailPersist = s_look.trail_persist > 1.0f ? s_look.trail_persist : 1.0f;
        f.trailSlur = s_look.trail_slur > 0.0f ? s_look.trail_slur : 0.0f;
        f.trailRim = s_look.trail_rim > 0.0f ? s_look.trail_rim : 0.0f;
        static std::vector<int32_t> s_rects[2];
        const Trails::Layer* layers[2] = { &Trails::Fine(), &Trails::Coarse() };
        Frame::TrailLayer* views[2] = { &f.trailFine, &f.trailCoarse };

        for (int k = 0; k < 2; k++) {
            const Trails::Layer& L = *layers[k];
            Frame::TrailLayer& v = *views[k];
            s_rects[k].clear();

            for (const Trails::Rect& r : L.dirty) {
                s_rects[k].insert(s_rects[k].end(), { r.x, r.y, r.w, r.h });
            }

            v.size = trails.on ? L.size : 0;
            v.cell = L.cell;
            v.firstI = L.firstI;
            v.firstJ = L.firstJ;
            v.cells = L.cells.empty() ? nullptr : L.cells.data();
            v.rects = s_rects[k].empty() ? nullptr : s_rects[k].data();
            v.rectCount = static_cast<uint32_t>(L.dirty.size());
            v.full = L.full;
        }

        Trails::TookChanges();

        f.entries = s_entries.empty() ? nullptr : s_entries.data();
        f.entryCount = static_cast<uint32_t>(s_entries.size());
        f.entryVersion = s_entryVersion;

        f.nearSize = s_near.size;
        f.nearCell = s_near.cell;
        f.nearFirstI = s_near.firstI;
        f.nearFirstJ = s_near.firstJ;
        f.nearCells = s_near.size ? s_near.cells.data() : nullptr;
        f.nearVersion = s_near.version;
        f.farSize = s_far.size;
        f.farCell = s_far.cell;
        f.farFirstI = s_far.firstI;
        f.farFirstJ = s_far.firstJ;
        f.farCells = s_far.size ? s_far.cells.data() : nullptr;
        f.farVersion = s_far.version;

        AssignSlots();
        f.slotCount = 0;

        for (uint32_t k = 0; k < s_textureCount; k++) {
            const Texture& t = s_textures[k];
            f.textureGloss[k] = (t.flags & SXC_COVER_TEXTURE_GLOSS) != 0;

            if (!t.slot || t.slot > s_slots) {
                continue;
            }

            // Asking without waiting also moves a texture still loading up the loader's queue.
            f.slotTexture[t.slot - 1] = static_cast<uint16_t>(k + 1);
            f.slotGx[t.slot - 1] = TextureGetGxTex(t.texture, 0, nullptr);
            f.slotCount = t.slot > f.slotCount ? t.slot : f.slotCount;
        }

        // Slots below the highest with nothing in them this frame.
        for (uint32_t s = 0; s < f.slotCount; s++) {
            const uint32_t id = f.slotTexture[s];

            if (!id || id > s_textureCount || s_textures[id - 1].slot != s + 1) {
                f.slotTexture[s] = 0;
                f.slotGx[s] = nullptr;
            }
        }

        return f;
    }

    void BeforeChunkDraw(CMapRenderChunk* rc) {
        float constants[12] = {};

        if (rc && s_available && s_haveLook && s_look.enabled && s_resolve) {
            s_draws++;

            // The cache stays bounded (some thousand chunks stream in and out over a session).
            if (s_chunks.size() > 6000) {
                s_chunks.clear();
            }

            Cached& c = s_chunks[rc];
            const uint32_t layers = rc->m_layersCount < SXC_COVER_MAX_LAYERS ? rc->m_layersCount : SXC_COVER_MAX_LAYERS;
            bool same = c.generation == s_generation && c.layerCount == layers && c.chunks[0] == rc->m_mapChunks[0] && c.chunks[1] == rc->m_mapChunks[1];

            for (uint32_t l = 0; l < layers && same; l++) {
                same = c.textureIds[l] == rc->m_layers[l].m_textureId;
            }

            if (!same) {
                c = Cached{};
                c.generation = s_generation;
                c.layerCount = layers;
                c.chunks[0] = rc->m_mapChunks[0];
                c.chunks[1] = rc->m_mapChunks[1];

                for (uint32_t l = 0; l < layers; l++) {
                    c.textureIds[l] = rc->m_layers[l].m_textureId;
                }

                Resolve(rc, c);
            }

            for (int l = 0; l < 4; l++) {
                constants[l] = c.painted[l];
                constants[4 + l] = c.snow[l];
            }

            // The textures this chunk shows (for loading them and handing out the slots).
            for (int k = 0; k < 8; k++) {
                const uint32_t e = static_cast<uint32_t>(constants[k]);

                if (e >= 1 && e <= s_entries.size()) {
                    const int32_t id = s_entries[e - 1].texture;

                    if (id >= 1 && static_cast<uint32_t>(id) <= s_textureCount) {
                        s_textures[id - 1].uses++;
                        s_textures[id - 1].lastUsed = s_frameNo;
                    }
                }
            }

            s_withCover += (constants[0] + constants[1] + constants[2] + constants[3]) > 0.0f;

            // The chunk's water line beyond the near map: the lowest top of its liquids (none: far below).
            float surface = -100000.0f;

            for (CMapChunk* chunk : rc->m_mapChunks) {
                for (CChunkLiquid* liquid = chunk ? chunk->m_liquidChunkLinkList.Head() : nullptr; liquid; liquid = chunk->m_liquidChunkLinkList.Next(liquid)) {
                    if (std::isfinite(liquid->m_height.x) && (surface <= -100000.0f || liquid->m_height.x < surface)) {
                        surface = liquid->m_height.x;
                    }
                }
            }

            constants[9] = surface;

            // Depth where the chunk can have any: a painted cover with depth, or fallen snow around.
            if (s_look.tess_detail > 0.0f && s_look.depth_scale > 0.0f) {
                bool raised = s_look.fallen_snow && std::fmax(std::fmax(s_near.mostFallen, s_far.mostFallen), s_look.outside[0]) > 0.002f;

                for (int l = 0; l < 4 && !raised; l++) {
                    const uint32_t e = static_cast<uint32_t>(constants[l]);
                    raised = e >= 1 && e <= s_entries.size() && s_entries[e - 1].depth > 0.0f;
                }

                constants[8] = raised ? 1.0f : 0.0f;
            }
        }

        GxShaderConstantsSet(GxSh_Pixel, 11, constants, 3);
    }
}
