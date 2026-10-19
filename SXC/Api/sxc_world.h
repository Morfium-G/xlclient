/*
 * sxc.world: where the player is, from the engine. One snapshot per call (cheap; a mod takes one per
 * frame) plus the area queries the climate and the place rules need.
 */

#ifndef SXC_API_WORLD_H
#define SXC_API_WORLD_H

#include "SXC/Api/sxc.h"

#define SXC_WORLD_SERVICE         "sxc.world"
#define SXC_WORLD_SERVICE_VERSION 1

#define SXC_WORLD_MAX_AREA_CHAIN 8

#ifdef __cplusplus
extern "C" {
#endif

typedef enum sxc_weather_type
{
    SXC_WEATHER_FINE = 0,
    SXC_WEATHER_RAIN = 1,
    SXC_WEATHER_SNOW = 2,
    SXC_WEATHER_SAND = 3
} sxc_weather_type;

typedef struct sxc_world_snapshot
{
    uint32_t    struct_size;
    wxl_bool    in_world;          ///< a player object exists
    wxl_vec3    player_position;
    int32_t     map_id;            ///< -1 outside the world
    /// The player's area and its parents, most specific first (sub-area, zone, ...); WMO areas count.
    uint32_t    area_chain[SXC_WORLD_MAX_AREA_CHAIN];
    int32_t     area_count;
    wxl_bool    outdoors;          ///< the client's verdict (gates sky, fog, outdoor light)
    int32_t     terrain_type;      ///< TerrainType ID under the player, -1 none
    float       weather_raw;       ///< storm intensity as the weather keeps it
    float       weather_intensity; ///< 0..1 after the engine's dead zone (below 0.25 nothing reacts)
    int32_t     weather_type;      ///< sxc_weather_type
    wxl_vec3    camera_position;
    const char* zone_text;         ///< for display; valid until the zone changes
    const char* subzone_text;
} sxc_world_snapshot;

typedef struct sxc_world_api
{
    uint32_t struct_size;

    /// Fills `out` (set out->struct_size first).
    void(WXL_CALL* snapshot)(sxc_world_snapshot* out);

    /// An area and its parents, most specific first. Returns how many were written (0 for area 0).
    int32_t(WXL_CALL* area_chain)(uint32_t area, uint32_t* out, int32_t max);

    /// The area of the terrain chunk under a world position (x, y); 0 where no chunk is loaded.
    uint32_t(WXL_CALL* terrain_area_at)(float x, float y);
} sxc_world_api;

#ifdef __cplusplus
}
#endif

#endif
