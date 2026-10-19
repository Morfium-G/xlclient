// sxc.fog (SXC/Api/sxc_fog.h) and its seam in the world render (SXC/Fog/Fog.h).

#include "SXC/Fog/Fog.h"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>
#include "SXC/Api/sxc_fog.h"
#include "Engine/Source/Os/TimeManager.h"
#include "WoW/Source/UIUtil/Camera.h"
#include "WoW/Source/Ui/WorldFrame.h"
#include "WoW/Source/WorldClient/DayNight.h"
#include "WoW/Source/WorldClient/DayNight__DNInfo.h"

namespace {
    using namespace SXC::Fog;

    Renderer s_renderer = nullptr;
    StatusFn s_status = nullptr;

    sxc_fog_look s_look = {};
    bool s_haveLook = false;

    // The field, as the renderer takes it (half floats), and its window.
    std::vector<uint16_t> s_light, s_pool, s_tint;
    bool s_haveTint = false;
    uint32_t s_size = 0;
    float s_cellSize = 2.0f;
    int32_t s_firstI = 0, s_firstJ = 0;
    uint32_t s_version = 0;

    // How far the patches have drifted (yd): the look's drift velocity summed frame by frame, so a
    // changing wind turns the drift instead of jumping the whole pattern.
    double s_drift[2] = {};
    uint32_t s_lastMs = 0;

    // IEEE half from float (round to nearest; overflow to infinity, underflow to zero).
    uint16_t Half(float value) {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof bits);
        const uint32_t sign = (bits >> 16) & 0x8000u;
        const int32_t exponent = static_cast<int32_t>((bits >> 23) & 0xFF) - 127 + 15;
        uint32_t mantissa = bits & 0x7FFFFFu;

        if (((bits >> 23) & 0xFF) == 0xFF) {
            return static_cast<uint16_t>(sign | 0x7C00u | (mantissa ? 0x200u : 0u));
        }

        if (exponent >= 31) {
            return static_cast<uint16_t>(sign | 0x7C00u);
        }

        if (exponent <= 0) {
            if (exponent < -10) {
                return static_cast<uint16_t>(sign);
            }

            mantissa |= 0x800000u;
            const uint32_t shift = static_cast<uint32_t>(14 - exponent);
            uint32_t half = mantissa >> shift;

            if ((mantissa >> (shift - 1)) & 1u) {
                half++;
            }

            return static_cast<uint16_t>(sign | half);
        }

        uint32_t half = sign | (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13);

        if (mantissa & 0x1000u) {
            half++; // may carry into the exponent: still the right rounding
        }

        return static_cast<uint16_t>(half);
    }

    void Convert(const float* src, std::vector<uint16_t>& dst, size_t count) {
        dst.resize(count * 4);

        for (size_t i = 0; i < count * 4; i++) {
            dst[i] = src ? Half(std::isfinite(src[i]) ? src[i] : 0.0f) : 0;
        }
    }

    // --- the service --------------------------------------------------------------------------------

    wxl_bool WXL_CALL Available() {
        return s_renderer ? WXL_TRUE : WXL_FALSE;
    }

    void WXL_CALL SetField(const sxc_fog_field* field) {
        if (!field || field->struct_size < offsetof(sxc_fog_field, colour) || !field->light || field->size == 0 || field->size > SXC_FOG_MAX_FIELD) {
            s_size = 0;
            s_light.clear();
            s_pool.clear();
            s_tint.clear();
            s_haveTint = false;
            s_version++;
            return;
        }

        const size_t cells = static_cast<size_t>(field->size) * field->size;
        Convert(field->light, s_light, cells);
        Convert(field->pool, s_pool, cells); // zeros without pools
        // The colour per cell (appended to the struct): without it, the look's colour everywhere.
        const float* tint = field->struct_size >= sizeof(sxc_fog_field) ? field->colour : nullptr;
        s_haveTint = tint != nullptr;

        if (s_haveTint) {
            Convert(tint, s_tint, cells);
        } else {
            s_tint.clear();
        }
        s_size = field->size;
        s_cellSize = field->cell_size > 0.01f ? field->cell_size : 0.01f;
        s_firstI = field->first_i;
        s_firstJ = field->first_j;
        s_version++;
    }

    void WXL_CALL SetLook(const sxc_fog_look* look) {
        if (!look || look->struct_size < offsetof(sxc_fog_look, step_length)) {
            s_haveLook = false;
            return;
        }

        s_look = sxc_fog_look{};
        std::memcpy(&s_look, look, look->struct_size < sizeof(sxc_fog_look) ? look->struct_size : sizeof(sxc_fog_look));
        s_haveLook = true;
    }

    const char* WXL_CALL Status() {
        if (!s_renderer) {
            return "off: this renderer doesn't draw volumetric fog (Vulkan only)";
        }

        if (!s_haveLook || !s_look.enabled) {
            return "off: no mod turned it on";
        }

        return s_status ? s_status() : "";
    }

    void WXL_CALL StockColour(float* out) {
        if (!out) {
            return;
        }

        const CImVector& stock = DayNight::GetInfo()->m_fog.m_color;
        out[0] = stock.r / 255.0f;
        out[1] = stock.g / 255.0f;
        out[2] = stock.b / 255.0f;
    }

    const sxc_fog_api s_api = { sizeof(sxc_fog_api), Available, SetField, SetLook, Status, StockColour };
}

namespace SXC::Fog {
    const sxc_fog_api* Api() {
        return &s_api;
    }

    void SetRenderer(Renderer renderer, StatusFn status) {
        s_renderer = renderer;
        s_status = status;
        s_version++; // a new device holds no field yet
    }

    bool WaterWritesDepth() {
        return s_renderer && s_haveLook && s_look.enabled && s_size > 0;
    }

    void Composite() {
        const CGWorldFrame* frame = CGWorldFrame::s_currentWorldFrame;

        if (!s_renderer || !s_haveLook || !s_look.enabled || !frame || !frame->m_camera) {
            return;
        }

        Input in;
        const C3Vector& camera = frame->m_camera->m_position;
        in.origin[0] = camera.x;
        in.origin[1] = camera.y;
        in.origin[2] = camera.z;
        const uint32_t now = OsGetAsyncTimeMs();
        const float dt = s_lastMs ? static_cast<float>(now - s_lastMs) * 0.001f : 0.0f;
        s_lastMs = now;
        in.seconds = static_cast<float>(now % 3600000u) * 0.001f;

        if (dt > 0.0f && dt < 1.0f) {
            for (int k = 0; k < 2; k++) {
                s_drift[k] = std::fmod(s_drift[k] + static_cast<double>(s_look.drift[k]) * dt, 100000.0);
            }
        }

        // The colour: the place's stock fog colour (what the sky's distance fog has), or the mod's.
        float colour[3] = { s_look.colour[0], s_look.colour[1], s_look.colour[2] };

        if (s_look.colour_mode == 0) {
            const CImVector& stock = DayNight::GetInfo()->m_fog.m_color;
            colour[0] = stock.r / 255.0f;
            colour[1] = stock.g / 255.0f;
            colour[2] = stock.b / 255.0f;
        }

        for (int k = 0; k < 3; k++) {
            in.colour[k] = colour[k] * s_look.brightness;
        }

        if (s_size) {
            in.fieldSize = s_size;
            in.cellSize = s_cellSize;
            in.firstI = s_firstI;
            in.firstJ = s_firstJ;
            in.light = s_light.data();
            in.pool = s_pool.data();
            in.tint = s_haveTint ? s_tint.data() : nullptr;
            in.tintBrightness = s_look.brightness;
        }

        in.fieldVersion = s_version;
        in.densityScale = s_look.density_scale;
        in.noiseAmount = s_look.noise_amount;
        in.noiseTile = s_look.noise_tile;
        in.drift[0] = static_cast<float>(s_drift[0]);
        in.drift[1] = static_cast<float>(s_drift[1]);
        in.farDistance = s_look.far_distance > 10.0f ? s_look.far_distance : 10.0f;
        in.hazeDensity = s_look.haze_density;
        in.hazeBase = s_look.haze_base;
        in.hazeThickness = s_look.haze_thickness;
        in.debug = s_look.debug;
        in.stepLength = s_look.step_length > 0.0f ? s_look.step_length : 0.5f;
        in.perPixel = s_look.method == 0;
        in.maxSteps = s_look.max_steps > 0 ? s_look.max_steps : 160;
        s_renderer(in);
    }
}
