// sxc.world (SXC/Api/sxc_world.h): the player's place, read from the engine.

#include "SXC/Api/sxc_world.h"

#include "WoW/Source/DB/DBClient/DBClient.h"
#include "WoW/Source/DB/Rec/AreaTableRec.h"
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
}

namespace SXC::WorldApi {
    const sxc_world_api* Api() {
        static const sxc_world_api s_api = { sizeof(sxc_world_api), Snapshot, AreaChain, TerrainAreaAt };
        return &s_api;
    }
}
