// sxc.world (SXC/Api/sxc_world.h): the player's place, read from the engine.

#include "SXC/Api/sxc_world.h"

#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include "SXC/World/World.h"
#include "Engine/Source/Tempest/C4Vector.h"
#include "Engine/Source/Tempest/CMath.h"
#include "WoW/Source/UIUtil/Camera.h"
#include "WoW/Source/Ui/WorldFrame.h"
#include "WoW/Source/DB/DBClient/DBClient.h"
#include "WoW/Source/DB/Rec/AreaTableRec.h"
#include "WoW/Source/DB/Rec/GroundEffectTextureRec.h"
#include "WoW/Source/DB/Rec/LiquidTypeRec.h"
#include "WoW/Source/WorldClient/MapArea.h"
#include "WoW/Source/WorldClient/MapChunk.h"
#include "WoW/Source/WorldClient/Types.h"
#include "WoW/Source/Object/ObjectClient/Player_C.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"
#include "WoW/Source/Ui/GameUI.h"
#include "WoW/Source/WorldClient/DayNight.h"
#include "WoW/Source/WorldClient/DayNight__DNInfo.h"
#include "WoW/Source/WorldClient/Map.h"
#include "WoW/Source/WorldClient/MapWeather.h"
#include "WoW/Source/WorldClient/World.h"

namespace {
    // The engine's own dead zone: nothing reacts below it, and knee..1 maps onto 0..1.
    constexpr float kWeatherKnee = 0.25f;

    // --- the test weather ----------------------------------------------------------------------------
    // The server's last weather (applied again when the test ends) and the test's.
    struct ServerWeather {
        bool known = false;
        uint32_t type = 0;
        float intensity = 0.0f;
        const WeatherRec* rec = nullptr;
        bool immediate = true;
        float transition = 1.0f;
    };

    ServerWeather s_serverWeather;
    int32_t s_testType = -1;
    float s_testIntensity = 0.0f;

    void ApplyTest() {
        if (Weather* weather = ::World::s_weather) {
            // The snapshot's intensity is past the engine's knee: give it back the same way.
            const float raw = s_testIntensity <= 0.0f ? 0.0f : kWeatherKnee + (1.0f - kWeatherKnee) * (s_testIntensity > 1.0f ? 1.0f : s_testIntensity);
            weather->SetType(static_cast<uint32_t>(s_testType), s_testType ? raw : 0.0f, nullptr, true, 1.0f);
        }
    }

    // A map change or a teleport resets the client's weather: show the test again.
    void KeepTest() {
        const Weather* weather = ::World::s_weather;

        if (s_testType > 0 && weather && weather->m_pendingType == -1 && weather->GetType() != static_cast<uint32_t>(s_testType)) {
            ApplyTest();
        }
    }

    void WXL_CALL SetWeather(int32_t type, float intensity) {
        Weather* weather = ::World::s_weather;

        if (type > SXC_WEATHER_SAND) {
            return;
        }

        if (type >= 0) {
            s_testType = type;
            s_testIntensity = intensity;
            ApplyTest();
            return;
        }

        if (s_testType < 0) {
            return;
        }

        s_testType = -1;

        if (!weather) {
            return;
        }

        if (s_serverWeather.known) {
            weather->SetType(s_serverWeather.type, s_serverWeather.intensity, s_serverWeather.rec, true, s_serverWeather.transition);
        } else {
            weather->SetType(0, 0.0f, nullptr, true, 1.0f);
        }
    }

    int32_t WXL_CALL WeatherTest() {
        return s_testType;
    }

    // --- projection ------------------------------------------------------------------------------------
    // As the client places nameplates (CGWorldFrame::GetScreenCoordinates), in window fractions.
    wxl_bool WXL_CALL Project(const wxl_vec3* world, float* outX, float* outY) {
        const CGWorldFrame* frame = CGWorldFrame::s_currentWorldFrame;

        if (!world || !frame || !frame->m_camera) {
            return WXL_FALSE;
        }

        const C3Vector& camera = frame->m_camera->m_position;
        C4Vector relative;
        relative.x = world->x - camera.x;
        relative.y = world->y - camera.y;
        relative.z = world->z - camera.z;
        relative.w = 0.0f;
        const C4Vector projected = relative * frame->m_viewProjection;

        if (projected.z < frame->m_camera->m_nearZ || projected.w <= 0.0f) {
            return WXL_FALSE;
        }

        const float ndcX = (projected.x / projected.w + 1.0f) * 0.5f;
        const float ndcY = (projected.y / projected.w + 1.0f) * 0.5f;
        const CRect& viewport = frame->m_viewportNDC;

        if (outX) *outX = viewport.minX + ndcX * (viewport.maxX - viewport.minX);
        if (outY) *outY = 1.0f - (viewport.minY + ndcY * (viewport.maxY - viewport.minY)); // NDC runs up, windows down
        return WXL_TRUE;
    }

    int32_t WXL_CALL AreaChain(uint32_t area, uint32_t* out, int32_t max) {
        int32_t n = 0;

        // The cap guards against a malformed (cyclic) table.
        while (area && out && n < max) {
            out[n++] = area;
            const AreaTableRec* rec = g_areaTableDB.GetRecord(static_cast<int32_t>(area));

            if (!rec || static_cast<uint32_t>(rec->m_parentAreaID) == area) {
                break;
            }

            area = static_cast<uint32_t>(rec->m_parentAreaID);
        }

        return n;
    }

    void WXL_CALL Snapshot(sxc_world_snapshot* out) {
        if (!out || out->struct_size < sizeof(sxc_world_snapshot)) {
            return;
        }

        *out = sxc_world_snapshot{};
        out->struct_size = sizeof(sxc_world_snapshot);
        out->map_id = -1;
        out->outdoors = WXL_TRUE;
        out->terrain_type = -1;
        out->zone_text = "";
        out->subzone_text = "";

        const WOWGUID guid = ClntObjMgrGetActivePlayer();
        CGPlayer_C* player = guid ? static_cast<CGPlayer_C*>(ClntObjMgrObjectPtr(guid, TYPEMASK_PLAYER, __FILE__, __LINE__)) : nullptr;

        if (player) {
            out->in_world = WXL_TRUE;
            out->map_id = ::World::QueryMapId();
            const C3Vector position = player->GetPosition();
            out->player_position = wxl_vec3{ position.x, position.y, position.z };
            out->terrain_type = player->m_terrainTypeId;

            if (player->m_worldObject) {
                uint32_t area = 0;

                if (CMap::QueryAreaId(player->m_worldObject, area) && area) {
                    out->area_count = AreaChain(area, out->area_chain, SXC_WORLD_MAX_AREA_CHAIN);
                }

                out->outdoors = ::World::QueryOutdoors(player->m_worldObject) != 0;
            }

            out->zone_text = CGGameUI::m_zoneText ? CGGameUI::m_zoneText : "";
            out->subzone_text = CGGameUI::m_subZoneText ? CGGameUI::m_subZoneText : "";
        }

        KeepTest();

        if (const Weather* weather = ::World::s_weather) {
            out->weather_raw = weather->m_intensity;
            out->weather_intensity = weather->m_intensity <= kWeatherKnee ? 0.0f
                                   : (weather->m_intensity >= 1.0f ? 1.0f : (weather->m_intensity - kWeatherKnee) / (1.0f - kWeatherKnee));
            out->weather_type = static_cast<int32_t>(weather->GetType());
        }

        const C3Vector& camera = DayNight::GetInfo()->m_cameraPos;
        out->camera_position = wxl_vec3{ camera.x, camera.y, camera.z };
    }

    uint32_t WXL_CALL TerrainAreaAt(float x, float y) {
        const C3Vector position(x, y, 0.0f);
        return static_cast<uint32_t>(CMap::QueryAreaIdTerrain(&position));
    }

    // --- the terrain at a point ------------------------------------------------------------------------

    constexpr float kWorldHalf = 17066.666f;
    constexpr float kCellSize = 100.0f / 3.0f / 8.0f; // 8 cells per chunk

    // The loaded chunk under (x, y) and the cell in it, the way the client's own queries find them
    // (CMap::LinkStaticEntityGetChunk): cellX runs along -y, cellY along -x; each 0..7 inside the chunk.
    struct Spot {
        CMapArea* area = nullptr;
        CMapChunk* chunk = nullptr;
        int32_t cellX = 0, cellY = 0;
        float r = 0.0f, c = 0.0f; // cells from the chunk's corner along local x (rows) and y (columns), 0..8
        int32_t row = 0, col = 0;
    };

    bool FindSpot(float x, float y, Spot& out) {
        const float fx = -(x - kWorldHalf), fy = -(y - kWorldHalf);

        if (!(fx >= 0.0f && fy >= 0.0f && fx < 2.0f * kWorldHalf && fy < 2.0f * kWorldHalf)) {
            return false;
        }

        out.cellX = CMath::fint_(fy * 0.24f - 0.5f);
        out.cellY = CMath::fint_(fx * 0.24f - 0.5f);
        out.area = CMap::m_areaTable[64 * ((out.cellY >> 7) & 0x3F) + ((out.cellX >> 7) & 0x3F)];

        if (!out.area || out.area->m_asyncObject) {
            return false;
        }

        out.chunk = out.area->m_mapChunks[((out.cellX >> 3) & 0xF) + 16 * ((out.cellY >> 3) & 0xF)];

        if (!out.chunk || !out.chunk->m_header || !out.chunk->m_height) {
            return false;
        }

        out.row = out.cellY & 7;
        out.col = out.cellX & 7;
        const C3Vector& corner = out.chunk->m_topLeftCoords;
        out.r = (corner.x - x) / kCellSize;
        out.c = (corner.y - y) / kCellSize;
        return true;
    }

    bool IsHole(const Spot& at) {
        return (at.chunk->m_header->m_holes & CMap::s_holeMask[4 * (at.row >> 1) + (at.col >> 1)]) != 0;
    }

    wxl_bool WXL_CALL TerrainHeight(float x, float y, float* outZ) {
        Spot at;

        if (!outZ || !FindSpot(x, y, at) || IsHole(at)) {
            return WXL_FALSE;
        }

        C3Vector position(x, y, 0.0f);
        float z = 0.0f;

        if (!CMap::GetHeightTerrain(at.chunk, position, at.cellX, at.cellY, &z) || !std::isfinite(z)) {
            return WXL_FALSE;
        }

        *outZ = z;
        return WXL_TRUE;
    }

    // Alpha maps (MCAL), decoded once per chunk and kept: the client keeps no decoded copy (it reads
    // rows while it builds the blend texture, CMapRenderChunk). A layer has a map when its MCLY flags
    // have 0x100, at MCAL + its offset; 0x200 = RLE (8 bit, 64 x 64). Otherwise 8 bit uncompressed on
    // "big alpha" maps (the WDT's MPHD flag 0x4), else 4 bit (low nibble first) whose last row and
    // column repeat the one before unless the chunk's flags have 0x8000.
    //
    // A chunk object is reused for another place after a teleport or a tile reload, and the new tile's
    // buffers can land at the same addresses, so a cached decode is checked against the chunk's corner,
    // layer count and data pointers together. Texture names are copied: the tile's MTEX block goes with
    // the tile.
    struct DecodedChunk {
        const SMLayer* layers = nullptr;
        const uint8_t* alpha = nullptr;
        float cornerX = 0.0f, cornerY = 0.0f;
        uint32_t serial = 0;
        int32_t count = 0;
        std::vector<uint8_t> map; // layers 1..count-1, 64 x 64 each
        uint32_t effect[SXC_TERRAIN_MAX_LAYERS] = {};
        int32_t terrainType[SXC_TERRAIN_MAX_LAYERS] = { -1, -1, -1, -1 };
        std::string texture[SXC_TERRAIN_MAX_LAYERS];
    };

    std::unordered_map<const CMapChunk*, DecodedChunk> s_decoded;
    uint32_t s_serial = 0;

    void DecodeLayer(const uint8_t* src, uint32_t flags, bool fix, uint8_t* out) {
        if (flags & 0x200) {
            // RLE: a byte's top bit = fill (repeat the next byte) or copy (the next bytes), its low 7
            // bits the count.
            int n = 0;

            while (n < 64 * 64) {
                const uint8_t head = *src++;
                int count = head & 0x7F;

                if (!count) {
                    break; // malformed: don't loop forever
                }

                if (head & 0x80) {
                    const uint8_t v = *src++;

                    while (count-- && n < 64 * 64) {
                        out[n++] = v;
                    }
                } else {
                    while (count-- && n < 64 * 64) {
                        out[n++] = *src++;
                    }
                }
            }

            return;
        }

        if (CMap::header.m_flags & 0x4) {
            std::memcpy(out, src, 64 * 64);
            return;
        }

        for (int i = 0; i < 64 * 32; i++) {
            out[i * 2] = static_cast<uint8_t>((src[i] & 0x0F) * 17);
            out[i * 2 + 1] = static_cast<uint8_t>((src[i] >> 4) * 17);
        }

        if (fix) {
            for (int r = 0; r < 64; r++) {
                out[r * 64 + 63] = out[r * 64 + 62];
            }

            std::memcpy(out + 63 * 64, out + 62 * 64, 64);
        }
    }

    const DecodedChunk& Decoded(const Spot& at) {
        const CMapChunk* chunk = at.chunk;
        const SMLayer* layers = chunk->m_layers;
        const uint8_t* alpha = chunk->m_additionalShadowmap; // the chunk's MCAL
        const uint32_t layerCount = chunk->m_header->m_nLayers;
        const int32_t count = layers ? static_cast<int32_t>(layerCount > SXC_TERRAIN_MAX_LAYERS ? SXC_TERRAIN_MAX_LAYERS : layerCount) : 0;
        auto found = s_decoded.find(chunk);

        if (found != s_decoded.end() && found->second.layers == layers && found->second.alpha == alpha && found->second.count == count &&
            found->second.cornerX == chunk->m_topLeftCoords.x && found->second.cornerY == chunk->m_topLeftCoords.y) {
            return found->second;
        }

        // ~1500 chunks lie within the loaded range; a few KB each.
        if (s_decoded.size() > 2500) {
            s_decoded.clear();
        }

        DecodedChunk& d = s_decoded[chunk];
        d = DecodedChunk{};
        d.layers = layers;
        d.alpha = alpha;
        d.cornerX = chunk->m_topLeftCoords.x;
        d.cornerY = chunk->m_topLeftCoords.y;
        d.serial = ++s_serial;
        d.count = count;
        const bool fix = !(chunk->m_header->m_flags & 0x8000);

        if (count > 1) {
            d.map.assign(static_cast<size_t>(count - 1) * 64 * 64, 0);
        }

        TSGrowableArray<CMapAreaTexture>& textures = at.area->m_textures;

        for (int32_t l = 0; l < count; l++) {
            const SMLayer& layer = layers[l];

            if (layer.m_textureId < textures.Count() && textures[layer.m_textureId].m_textureName) {
                d.texture[l] = textures[layer.m_textureId].m_textureName;
            }

            d.effect[l] = layer.m_offectId;

            if (const GroundEffectTextureRec* rec = g_groundEffectTextureDB.GetRecord(static_cast<int32_t>(layer.m_offectId))) {
                d.terrainType[l] = rec->m_sound; // GroundEffectTexture's last column: the TerrainType
            }

            if (l > 0 && (layer.m_flags & 0x100) && alpha) {
                DecodeLayer(alpha + layer.m_offsetInMCAL, layer.m_flags, fix, d.map.data() + static_cast<size_t>(l - 1) * 64 * 64);
            }
        }

        return d;
    }

    // Bilinear between texels, coordinates in texels (0..63), clamped to the chunk. The map's rows run
    // along local x like the height rows and the low-res layer map (WXL confirmed it in-game: the
    // strongest layer matches the client's dominant layer).
    float Texel(const uint8_t* map, float u, float v) {
        u = u < 0.0f ? 0.0f : (u > 63.0f ? 63.0f : u);
        v = v < 0.0f ? 0.0f : (v > 63.0f ? 63.0f : v);
        const int u0 = static_cast<int>(u), v0 = static_cast<int>(v);
        const int u1 = u0 < 63 ? u0 + 1 : 63, v1 = v0 < 63 ? v0 + 1 : 63;
        const float fu = u - u0, fv = v - v0;
        auto at = [&](int a, int b) { return static_cast<float>(map[a * 64 + b]); };
        const float top = at(u0, v0) + (at(u0, v1) - at(u0, v0)) * fv;
        const float bottom = at(u1, v0) + (at(u1, v1) - at(u1, v0)) * fv;
        return (top + (bottom - top) * fu) / 255.0f;
    }

    wxl_bool WXL_CALL TerrainSample(float x, float y, sxc_terrain_sample* out) {
        Spot at;

        if (!out || out->struct_size < sizeof(sxc_terrain_sample) || !FindSpot(x, y, at) || IsHole(at)) {
            return WXL_FALSE;
        }

        const DecodedChunk& d = Decoded(at);
        *out = sxc_terrain_sample{};
        out->struct_size = sizeof(sxc_terrain_sample);
        out->area = static_cast<uint32_t>(at.chunk->m_areaId);
        out->layer_count = d.count;
        out->chunk_serial = d.serial;
        out->dominant_layer = -1;

        if (at.chunk->m_lowQualityTexMap) {
            const uint16_t bits = reinterpret_cast<const uint16_t*>(at.chunk->m_lowQualityTexMap)[at.row];
            out->dominant_layer = static_cast<int32_t>((CMap::s_2bitSplatMask[at.col] & bits) >> CMap::s_2bitSplatShft[at.col]);
        }

        // Texel centres at (i + 0.5) / 8 cells.
        const float u = at.r * 8.0f - 0.5f, v = at.c * 8.0f - 0.5f;
        float rest = 1.0f;

        for (int32_t l = 0; l < d.count; l++) {
            sxc_terrain_layer& layer = out->layers[l];
            layer.texture = d.texture[l].c_str();
            layer.ground_effect = d.effect[l];
            layer.terrain_type = d.terrainType[l];

            if (l > 0) {
                layer.weight = Texel(d.map.data() + static_cast<size_t>(l - 1) * 64 * 64, u, v);
                rest -= layer.weight;
            }
        }

        if (d.count > 0) {
            out->layers[0].weight = rest < 0.0f ? 0.0f : rest;
        }

        return WXL_TRUE;
    }

    wxl_bool WXL_CALL TerrainLiquid(float x, float y, float* outHeight, uint32_t* outType) {
        // Far below every surface: the probe takes the first liquid above the point, i.e. "the surface here".
        const C3Vector position(x, y, -100000.0f);
        int32_t type = 0;
        float height = 0.0f;

        if (!CMap::QueryLiquidTerrain(&position, &type, &height, 0) || !std::isfinite(height)) {
            return WXL_FALSE;
        }

        if (outHeight) *outHeight = height;
        if (outType) *outType = static_cast<uint32_t>(type);
        return WXL_TRUE;
    }

    int32_t WXL_CALL LiquidCategory(uint32_t liquidType, const char** outName) {
        const LiquidTypeRec* rec = g_liquidTypeDB.GetRecord(static_cast<int32_t>(liquidType));

        if (outName) {
            *outName = rec && rec->m_name ? rec->m_name : "";
        }

        return rec ? rec->m_soundBank : -1; // the Type column (named SoundBank in the decomp)
    }

    wxl_bool WXL_CALL Trace(const wxl_vec3* from, const wxl_vec3* to, uint32_t flags, float* outFraction) {
        if (!from || !to) {
            return WXL_FALSE;
        }

        const C3Vector start(from->x, from->y, from->z), end(to->x, to->y, to->z);
        C3Vector hit;
        float fraction = 1.0f; // the whole segment
        const bool blocked = ::World::Intersect(start, end, &hit, &fraction, flags ? flags : SXC_TRACE_TERRAIN_AND_WMO, nullptr) != 0;

        if (outFraction) {
            *outFraction = blocked ? fraction : 1.0f;
        }

        return blocked ? WXL_TRUE : WXL_FALSE;
    }

    const char* WXL_CALL AreaName(uint32_t area) {
        const AreaTableRec* rec = g_areaTableDB.GetRecord(static_cast<int32_t>(area));
        return rec && rec->m_areaName ? rec->m_areaName : "";
    }
}

namespace SXC::WorldApi {
    const sxc_world_api* Api() {
        static const sxc_world_api s_api = {
            sizeof(sxc_world_api), Snapshot, AreaChain, TerrainAreaAt, TerrainHeight, TerrainSample, TerrainLiquid, LiquidCategory, Trace, AreaName,
            Project, SetWeather, WeatherTest,
        };
        return &s_api;
    }

    bool HoldServerWeather(uint32_t type, float intensity, const WeatherRec* rec, bool immediate, float transition) {
        s_serverWeather = ServerWeather{ true, type, intensity, rec, immediate, transition };
        return s_testType >= 0;
    }
}
