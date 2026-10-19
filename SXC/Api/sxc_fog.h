/*
 * sxc.fog: volumetric fog drawn by the client from a medium a mod describes. The client renders (a
 * froxel volume over the view, laid over the scene after the opaque world and the water, before the
 * weather and the see-through models); the mod decides where the fog is and how it looks.
 *
 * The medium: a near field around the player (a grid of cells, each with a light layer that thins out
 * upward and a flat-topped pool) and a haze everywhere. Vulkan only: on another renderer available()
 * answers false and the client's own fog stays alone.
 */

#ifndef SXC_API_FOG_H
#define SXC_API_FOG_H

#include "SXC/Api/sxc.h"

#define SXC_FOG_SERVICE         "sxc.fog"
#define SXC_FOG_SERVICE_VERSION 1

#define SXC_FOG_MAX_FIELD 512

#ifdef __cplusplus
extern "C" {
#endif

/// The near field: a world-aligned toroidal grid. Cell (i, j) of the world (i = floor(x / cell_size),
/// j = floor(y / cell_size)) is stored at [mod(j, size) * size + mod(i, size)], 4 floats each; only
/// cells with i in [first_i, first_i + size) (same for j) are drawn, fading out over the last 8.
typedef struct sxc_fog_field
{
    uint32_t     struct_size;
    uint32_t     size;      ///< cells per side, 1 .. SXC_FOG_MAX_FIELD
    float        cell_size; ///< yards
    int32_t      first_i, first_j;
    /// Per cell: extinction per yard at the layer's base (0 = no fog), the base (world z), the
    /// thickness (yards: it thins to nothing there), patchiness 0..1 (how much the noise breaks it up).
    const float* light;
    /// Per cell: extinction per yard (0 = no pool), the top (world z; flat), the soft band under the
    /// top (yards), unused. May be null.
    const float* pool;
} sxc_fog_field;

typedef struct sxc_fog_look
{
    uint32_t struct_size;
    wxl_bool enabled;
    int32_t  colour_mode;     ///< 0 the place's stock fog colour, 1 `colour`
    float    colour[3];       ///< 0..1
    float    brightness;      ///< x the colour
    float    density_scale;   ///< x every density
    float    noise_amount;    ///< 0..1 x the cells' patchiness
    float    noise_tile;      ///< yards per noise tile (patches ~1/8 of it)
    float    drift[2];        ///< yards per second the patches move (the wind)
    float    far_distance;    ///< yards: the volume's reach (and the sky's fog distance)
    float    haze_density;    ///< extinction per yard at the haze's base; 0 = no haze
    float    haze_base;       ///< world z
    float    haze_thickness;  ///< yards: the haze thins by e over this height
    int32_t  debug;           ///< 1: the fog drawn magenta
} sxc_fog_look;

typedef struct sxc_fog_api
{
    uint32_t struct_size;

    /// Whether the client draws the fog (a renderer that can: Vulkan).
    wxl_bool(WXL_CALL* available)(void);

    /// The near field; copied (call again when it changes). Null clears it.
    void(WXL_CALL* set_field)(const sxc_fog_field* field);

    /// The look; copied.
    void(WXL_CALL* set_look)(const sxc_fog_look* look);

    /// One line: what the renderer did last frame, or why not. Valid until the next call.
    const char*(WXL_CALL* status)(void);
} sxc_fog_api;

#ifdef __cplusplus
}
#endif

#endif
