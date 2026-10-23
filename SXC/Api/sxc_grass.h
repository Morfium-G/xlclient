/*
 * sxc.grass: the ground-effect grass as the client draws it instanced (Vulkan): a mod moves it in the
 * wind and parts it around characters. Each frame the mod hands over the wind around the player (a
 * grid of samples), the characters near it and the motion's settings; the client bends every blade in
 * its vertex shader (Shaders/GrassMotion.glsl). Per plant model the bend runs from its root to its tip
 * in texture V, found from the model's geometry unless a row of the mod's table overrides it.
 *
 * On another renderer (or with the instanced grass off) available() answers false and nothing moves.
 */

#ifndef SXC_API_GRASS_H
#define SXC_API_GRASS_H

#include "SXC/Api/sxc.h"

#define SXC_GRASS_SERVICE         "sxc.grass"
#define SXC_GRASS_SERVICE_VERSION 1

#define SXC_GRASS_GRID       8   ///< wind samples per side
#define SXC_GRASS_MAX_ACTORS 16

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sxc_grass_motion
{
    uint32_t struct_size;
    wxl_bool on;
    float    time;            ///< seconds (keep it below an hour: the flutter's phase)
    float    anchor;          ///< 0..0.9: the bottom share of a blade that never moves
    float    amplitude;       ///< yd the tips move at wind strength 1
    float    flutter;         ///< yd of extra per-blade shimmer
    float    flutter_speed;   ///< radians a second
    float    push_strength;   ///< yd the blades lean away from a character
    float    grid_origin[2];  ///< world x, y of cell (0, 0)'s corner
    float    grid_spacing;    ///< yd between samples
    float    wind[SXC_GRASS_GRID * SXC_GRASS_GRID][3]; ///< cell (y * 8 + x): wind x and y (x strength), gust 0..1
    float    player[3];       ///< the actors are gathered around it
    float    actor_range;     ///< yd: grass farther from the player skips the actors
    uint32_t actor_count;
    float    actors[SXC_GRASS_MAX_ACTORS][4]; ///< position, push radius (yd)
} sxc_grass_motion;

/// A GroundEffectDoodadWind row: it replaces the automatic values of one plant model.
typedef struct sxc_grass_doodad
{
    uint32_t id;              ///< GroundEffectDoodad.dbc ID
    uint32_t flags;           ///< SXC_GRASS_NO_WIND, SXC_GRASS_NO_PUSH, SXC_GRASS_FLIP
    float    stiffness;       ///< 0 flexible .. 1 rigid (scales wind and push)
    float    root_v;          ///< the bend's start in texture V; < 0: automatic
    float    tip_v;           ///< its end; < 0: automatic
} sxc_grass_doodad;

#define SXC_GRASS_NO_WIND 0x1u
#define SXC_GRASS_NO_PUSH 0x2u
#define SXC_GRASS_FLIP    0x10u

/// What the client found for a plant model (the grass panel).
typedef struct sxc_grass_doodad_info
{
    uint32_t    struct_size;
    const char* path;         ///< its model file; valid until the next call
    wxl_bool    analyzed;     ///< its model has been read
    float       height;       ///< yd
    float       root_v, tip_v; ///< automatic
    wxl_bool    flat;         ///< too low to sway (pebbles, shells)
    float       entry[4];     ///< what its plants carry: root V, 1 / span, wind scale, push scale
} sxc_grass_doodad_info;

typedef struct sxc_grass_api
{
    uint32_t struct_size;

    /// Whether the client draws the grass instanced now (the motion only moves that grass).
    wxl_bool(WXL_CALL* available)(void);

    /// This frame's motion; copied. Without a call (or with on = false) the grass stands still.
    void(WXL_CALL* set_motion)(const sxc_grass_motion* motion);

    /// The table of overrides; copied. Call again when it changes (the plants are placed anew).
    void(WXL_CALL* set_doodads)(const sxc_grass_doodad* rows, uint32_t count);

    /// One plant model's values (false: the client doesn't know that ID).
    wxl_bool(WXL_CALL* doodad_info)(uint32_t id, sxc_grass_doodad_info* out);

    /// One line: what the grass did last frame.
    const char*(WXL_CALL* status)(void);
} sxc_grass_api;

#ifdef __cplusplus
}
#endif

#endif
