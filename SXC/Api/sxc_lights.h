/*
 * sxc.lights: point lights drawn per pixel by the client's own shaders, from what a mod sets. The client
 * gathers the lights (the models' own point lights: torches, braziers, lanterns; and the lights a mod
 * puts on model files by data), merges close ones, lets them flicker and hands the nearest to the GPU
 * on a world grid around the camera; each pixel adds its grid cell's lights to the stock light.
 *
 * Vulkan only (our terrain shader for now; models next): on another renderer available() answers false.
 */

#ifndef SXC_API_LIGHTS_H
#define SXC_API_LIGHTS_H

#include "SXC/Api/sxc.h"

#define SXC_LIGHTS_SERVICE         "sxc.lights"
#define SXC_LIGHTS_SERVICE_VERSION 1

#define SXC_LIGHTS_MAX_POOL     512 ///< lights in range at most (the farthest are left out past this)
#define SXC_LIGHTS_MAX_PER_CELL 32  ///< a grid cell's lights at most (the shaders' loop bound)
#define SXC_LIGHTS_MAX_CELLS    64  ///< grid cells per side at most

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sxc_lights_settings
{
    uint32_t struct_size;
    wxl_bool enabled;
    wxl_bool stock_off;       ///< the client's own point lights (3 per terrain chunk) off while ours are on
    int32_t  debug;           ///< 0 off, 1 our lights only (on grey ground), 2 the cell's light count
    float    radius;          ///< yd: a light is exactly 0 here (the stock torches reach ~13 yd to 5%)
    float    brightness;      ///< x the lights' colours
    float    range;           ///< yd from the camera: no lights beyond, fading out over the last third
    float    cell_size;       ///< yd per grid cell (the grid reaches the range, SXC_LIGHTS_MAX_CELLS per side at most)
    int32_t  max_per_cell;    ///< lights a cell holds (the strongest at its middle), 1..SXC_LIGHTS_MAX_PER_CELL
    float    merge_distance;  ///< yd: closer lights merge into one (torch groups)
    float    flicker;         ///< 0..1: how much a flame dips (each light its own phase)
    float    flicker_speed;   ///< x the default pace (~3 dips a second)
    /// Inside and outside apart: a light inside an indoor WMO group lights only indoor surfaces (indoor
    /// WMO groups), one outside only outdoor ones (the terrain, outdoor groups); models take both. Lights
    /// with a shadow map don't need it (their walls, floors and roofs are in the map).
    wxl_bool separate_indoor;
    /// Shadows: the nearest lights that stand still get a cube shadow map of the WMOs and the terrain
    /// around them (walls between rooms, floors, roofs); drawn once and kept until the light moves or its
    /// surroundings change (a WMO finishing loading).
    wxl_bool shadows;
    int32_t  shadow_count;    ///< lights with a shadow map at most (the nearest), up to the device's (stats.shadow_slots)
    int32_t  shadow_updates;  ///< shadow maps drawn per frame at most
} sxc_lights_settings;

/// What a light is (a DoodadLightProperties row). Negative values (and flicker_mode -1) take the
/// settings' defaults.
typedef struct sxc_lights_kind
{
    uint32_t id;
    uint32_t type;            ///< SXC_LIGHTS_POINT, SXC_LIGHTS_SPOT
    float    color[3];        ///< linear 0..1, x the intensity
    float    radius;          ///< yd (x the model's scale); <= 0: the settings' radius
    float    falloff;         ///< the window's power: 1 smooth, higher tighter; <= 0: 1
    float    inner_angle;     ///< spot: degrees from the axis at full strength
    float    outer_angle;     ///< spot: degrees from the axis beyond which there is none
    int32_t  flicker_mode;    ///< SXC_LIGHTS_FLICKER_*
    float    flicker_speed;   ///< x the default pace; < 0: 1
    float    flicker_amount;  ///< 0..1 how much it dips; < 0: the settings' flicker
    uint32_t flags;           ///< reserved
} sxc_lights_kind;

/// Where a light goes (a DoodadLightAssignment row): on every model of a file.
typedef struct sxc_lights_assignment
{
    const char* model_path;   ///< the model file (case and / vs \ don't matter); copied
    uint32_t    attach_type;  ///< SXC_LIGHTS_ATTACH_*
    int32_t     attach_index; ///< the attachment point's ID, the bone index or the particle emitter index
    float       offset[3];    ///< yd from the attachment, in the model's space (scaled with it)
    float       direction[3]; ///< spot: where the cone points, in the model's space
    uint32_t    light_id;     ///< a kind's id; 0 = no light (with the flag: only suppresses)
    uint32_t    flags;        ///< SXC_LIGHTS_SUPPRESS_MODEL_LIGHTS
    uint32_t    id;           ///< the row's ID (the log names it)
} sxc_lights_assignment;

#define SXC_LIGHTS_POINT 0u
#define SXC_LIGHTS_SPOT  1u

#define SXC_LIGHTS_FLICKER_DEFAULT -1 ///< smooth, by the settings' amount
#define SXC_LIGHTS_FLICKER_OFF      0
#define SXC_LIGHTS_FLICKER_SMOOTH   1 ///< three sines
#define SXC_LIGHTS_FLICKER_NOISE    2 ///< random levels ~6 a second, eased
#define SXC_LIGHTS_FLICKER_STEPS    3 ///< the same levels, held

#define SXC_LIGHTS_ATTACH_ORIGIN    0u
#define SXC_LIGHTS_ATTACH_POINT     1u ///< an attachment point by its ID
#define SXC_LIGHTS_ATTACH_BONE      2u ///< a bone by its index
#define SXC_LIGHTS_ATTACH_EMITTER   3u ///< a particle emitter by its index

#define SXC_LIGHTS_SUPPRESS_MODEL_LIGHTS 0x1u ///< the model's own lights are left out

typedef struct sxc_lights_stats
{
    uint32_t struct_size;
    uint32_t models;          ///< models in the scene
    uint32_t model_lights;    ///< point lights on them
    uint32_t placed;          ///< of those, at the position the client set this frame (the model animates)
    uint32_t in_range;        ///< within the range
    uint32_t merged;          ///< merged into another
    uint32_t active;          ///< handed to the GPU
    uint32_t cells;           ///< grid cells per side
    uint32_t busiest_cell;    ///< lights in the busiest cell
    uint32_t full_cells;      ///< cells that had more than they hold
    uint32_t table_lights;    ///< lights from the assignments
    uint32_t suppressed;      ///< models whose own lights an assignment left out
    uint32_t attach_missing;  ///< assignments whose attachment the model doesn't have (placed at its origin)
    uint32_t dropped;         ///< lights past SXC_LIGHTS_MAX_POOL (the farthest)
    uint32_t indoor;          ///< drawn lights inside an indoor WMO group
    uint32_t indoor_tests;    ///< points located this frame (the rest from the cache)
    uint32_t model_tests;     ///< models located this frame (for which lights they take)
    uint32_t shadow_slots;    ///< shadow maps the device holds (0: none)
    uint32_t shadowed;        ///< drawn lights with a shadow map
    uint32_t shadow_draws;    ///< shadow maps drawn this frame
    uint32_t shadow_triangles;///< triangles in them
    uint32_t shadow_waiting;  ///< lights that should have one and wait their turn (or still moving)
} sxc_lights_stats;

typedef struct sxc_lights_api
{
    uint32_t struct_size;

    /// Whether the client draws our lights (Vulkan with our terrain shader).
    wxl_bool(WXL_CALL* available)(void);

    /// The settings; copied.
    void(WXL_CALL* set_settings)(const sxc_lights_settings* settings);

    /// Last frame's numbers (out->struct_size set by the caller).
    void(WXL_CALL* stats)(sxc_lights_stats* out);

    /// One line: what the lights did last frame, or why not. Valid until the next call.
    const char*(WXL_CALL* status)(void);

    /// The lights by data (DoodadLightProperties, DoodadLightAssignment); copied. Call again when they
    /// change. Null / 0: none.
    void(WXL_CALL* set_table)(const sxc_lights_kind* kinds, uint32_t kind_count, const sxc_lights_assignment* rows, uint32_t row_count);
} sxc_lights_api;

#ifdef __cplusplus
}
#endif

#endif
