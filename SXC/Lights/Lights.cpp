// sxc.lights (SXC/Api/sxc_lights.h) and the light list the device pulls (SXC/Lights/Lights.h). Ported
// from WXL's env/Lights and features/DoodadLightTable (port plan, step 6a), reading the client's own
// structures instead of offsets.

#include "SXC/Lights/Lights.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "SXC/Api/sxc_lights.h"
#include "Engine/Source/Model2/M2Data.h"
#include "Engine/Source/Model2/M2Model.h"
#include "Engine/Source/Model2/M2ModelLight.h"
#include "Engine/Source/Model2/M2Scene.h"
#include "Engine/Source/Model2/M2Shared.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include "WoW/Source/WorldClient/World.h"

namespace {
    using namespace SXC::Lights;

    static_assert(SXC_LIGHTS_MAX_POOL == kMaxPool && SXC_LIGHTS_MAX_PER_CELL == kMaxPerCell && SXC_LIGHTS_MAX_CELLS == kMaxCells,
                  "SXC/Lights/Lights.h follows the API");

    bool s_available = false;
    StatusFn s_deviceStatus = nullptr;

    sxc_lights_settings s_settings = {};
    bool s_haveSettings = false;

    sxc_lights_stats s_stats = {};
    char s_status[512] = "";

    // --- the lights by data ------------------------------------------------------------------------
    struct Row {
        sxc_lights_assignment a = {};
        std::string path;   // normalised
        int kind = -1;      // index into s_kinds, -1 none
    };
    std::vector<sxc_lights_kind> s_kinds;
    std::vector<Row> s_rows;
    std::unordered_map<std::string, std::vector<uint32_t>> s_byPath; // normalised path -> rows
    uint32_t s_tableGeneration = 1;

    // Per model file (CM2Shared, shared by every model of the file): its rows, and whether its own lights
    // are left out. The file name is kept to notice a CM2Shared freed and reused by another file.
    struct FileRows {
        uint32_t generation = 0;
        std::string name;
        const std::vector<uint32_t>* rows = nullptr;
        bool suppress = false;
    };
    std::unordered_map<const CM2Shared*, FileRows> s_files;

    std::string Normalize(const char* path) {
        std::string s = path ? path : "";

        for (char& c : s) {
            c = c == '/' ? '\\' : (c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
        }

        return s;
    }

    const FileRows& RowsFor(const CM2Shared* shared) {
        if (s_files.size() > 16384) {
            s_files.clear();
        }

        FileRows& f = s_files[shared];

        if (f.generation != s_tableGeneration || f.name != shared->m_fileName) {
            f.generation = s_tableGeneration;
            f.name = shared->m_fileName;
            const auto it = s_byPath.find(Normalize(shared->m_fileName));
            f.rows = it != s_byPath.end() ? &it->second : nullptr;
            f.suppress = false;

            if (f.rows) {
                for (uint32_t r : *f.rows) {
                    f.suppress = f.suppress || (s_rows[r].a.flags & SXC_LIGHTS_SUPPRESS_MODEL_LIGHTS) != 0;
                }
            }
        }

        return f;
    }

    // --- this frame's lights -----------------------------------------------------------------------
    struct Found {
        C3Vector pos;
        C3Vector color;
        float radius = 0.0f;       // yd, scaled with the model; 0: the settings' radius
        float falloff = 1.0f;
        C3Vector spotDir = C3Vector(0.0f, 0.0f, -1.0f);
        float cosOuter = -2.0f, spotScale = 1.0f;
        int flickerMode = SXC_LIGHTS_FLICKER_DEFAULT;
        float flickerSpeed = -1.0f, flickerAmount = -1.0f;
        float distance = 0.0f;     // from the camera
        bool merged = false;
    };

    std::vector<Found> s_found;
    std::vector<GpuLight> s_lights;
    std::vector<float> s_strength;   // per light: its brightness before fade and flicker (full cells rank by it)
    std::vector<uint32_t> s_cellLights;
    Frame s_frame;

    constexpr int kMaxAttachDepth = 8;

    bool Settled() { return s_available && s_haveSettings && s_settings.enabled; }

    float Brightest(const C3Vector& c) { return std::fmax(c.x, std::fmax(c.y, c.z)); }

    // A direction through a matrix's rotation and scale (row vectors), and the length it came out with.
    C3Vector Dir(const C3Vector& v, const C44Matrix& m) {
        return C3Vector(v.x * m.a0 + v.y * m.b0 + v.z * m.c0, v.x * m.a1 + v.y * m.b1 + v.z * m.c1, v.x * m.a2 + v.y * m.b2 + v.z * m.c2);
    }

    float Length(const C3Vector& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

    // The first key of a track's first sequence, if it has one.
    template <class T>
    bool FirstKey(const M2Track<T>& track, T& out) {
        if (!track.m_sequenceKeys.m_count || !track.m_sequenceKeys[0].m_count) {
            return false;
        }

        out = track.m_sequenceKeys[0][0];
        return true;
    }

    bool Finite(const C3Vector& p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }

    // How a model puts a point of its own space into the world this frame: through a bone (it animated
    // this frame: its bone matrices are this frame's, view space), else its world matrix (a doodad out of
    // view: at rest). An attached model not animated has no position to trust.
    struct Placement {
        const CM2Scene* scene;
        const CM2Model* model;
        bool animated;
    };

    bool Place(const Placement& at, const C3Vector& local, uint32_t bone, const C3Vector* localDir, C3Vector& out, C3Vector* outDir, float& scale) {
        const M2Data* data = at.model->m_shared->m_data;

        if (at.animated && at.model->m_boneMatrices && bone < data->m_bones.m_count) {
            const C44Matrix& b = at.model->m_boneMatrices[bone];
            out = local * b * at.scene->m_viewInv;
            const C3Vector unit = Dir(Dir(C3Vector(1.0f, 0.0f, 0.0f), b), at.scene->m_viewInv);
            scale = Length(unit);

            if (localDir && outDir) {
                *outDir = Dir(Dir(*localDir, b), at.scene->m_viewInv);
            }
        } else if (!at.model->m_attachParent) {
            out = local * at.model->m_worldTransform;
            scale = Length(Dir(C3Vector(1.0f, 0.0f, 0.0f), at.model->m_worldTransform));

            if (localDir && outDir) {
                *outDir = Dir(*localDir, at.model->m_worldTransform);
            }
        } else {
            return false;
        }

        if (outDir) {
            const float len = Length(*outDir);
            *outDir = len > 1e-6f ? *outDir * (1.0f / len) : C3Vector(0.0f, 0.0f, -1.0f);
        }

        scale = scale > 1e-4f && std::isfinite(scale) ? scale : 1.0f;
        return Finite(out);
    }

    // An assignment's light on one model.
    void AddRow(const Placement& at, const Row& row, const C3Vector& camera, float reach) {
        if (row.kind < 0) {
            return;
        }

        const sxc_lights_kind& k = s_kinds[row.kind];
        const M2Data* data = at.model->m_shared->m_data;
        const sxc_lights_assignment& a = row.a;
        C3Vector local(a.offset[0], a.offset[1], a.offset[2]);
        uint32_t bone = 0;  // the root bone for the origin (it carries the model's own movement)
        bool found = a.attach_type == SXC_LIGHTS_ATTACH_ORIGIN;

        if (a.attach_type == SXC_LIGHTS_ATTACH_POINT && a.attach_index >= 0 && static_cast<uint32_t>(a.attach_index) < data->m_attachmentIndicesById.m_count) {
            const uint16_t index = data->m_attachmentIndicesById[a.attach_index];

            if (index < data->m_attachments.m_count) {
                const M2Attachment& p = data->m_attachments[index];
                local = local + p.m_position;
                bone = p.m_bone;
                found = true;
            }
        } else if (a.attach_type == SXC_LIGHTS_ATTACH_BONE && a.attach_index >= 0 && static_cast<uint32_t>(a.attach_index) < data->m_bones.m_count) {
            local = local + data->m_bones[a.attach_index].m_pivot;
            bone = static_cast<uint32_t>(a.attach_index);
            found = true;
        } else if (a.attach_type == SXC_LIGHTS_ATTACH_EMITTER && a.attach_index >= 0 && static_cast<uint32_t>(a.attach_index) < data->m_particles.m_count) {
            const M2Particle& p = data->m_particles[a.attach_index];
            local = local + p.m_position;
            bone = p.m_boneIndex;
            found = true;
        }

        if (!found) {
            s_stats.attach_missing++;
        }

        Found f;
        const C3Vector localDir(a.direction[0], a.direction[1], a.direction[2]);
        float scale = 1.0f;

        if (!Place(at, local, bone, &localDir, f.pos, &f.spotDir, scale)) {
            return;
        }

        const C3Vector d = f.pos - camera;
        f.distance = Length(d);

        if (f.distance > reach) {
            return;
        }

        f.color = C3Vector(k.color[0], k.color[1], k.color[2]);
        f.radius = k.radius > 0.0f ? k.radius * scale : 0.0f;
        f.falloff = k.falloff > 0.0f ? k.falloff : 1.0f;
        f.flickerMode = k.flicker_mode;
        f.flickerSpeed = k.flicker_speed;
        f.flickerAmount = k.flicker_amount;

        if (k.type == SXC_LIGHTS_SPOT) {
            constexpr float kRad = 3.14159265f / 180.0f;
            const float inner = std::cos(std::fmin(k.inner_angle, k.outer_angle) * kRad);
            f.cosOuter = std::cos(k.outer_angle * kRad);
            f.spotScale = inner - f.cosOuter > 1e-4f ? 1.0f / (inner - f.cosOuter) : 1e4f;
        }

        if (Brightest(f.color) > 0.0f) {
            s_found.push_back(f);
            s_stats.table_lights++;
        }
    }

    // A model's lights: its file's assignments, and its own point lights unless they're left out (where
    // the client placed them this frame, else the file's position through the model's world matrix; the
    // colour as the client last had it, else the file's first diffuse colour x intensity). Then the
    // models hanging on it (a torch in a hand).
    void Visit(const CM2Scene* scene, CM2Model* model, const C3Vector& camera, float reach, int depth) {
        if (!model || depth > kMaxAttachDepth) {
            return;
        }

        s_stats.models++;
        const M2Data* data = model->m_shared ? model->m_shared->m_data : nullptr;

        if (data) {
            const Placement at{ scene, model, model->m_frameStamp == scene->m_frameStamp };
            bool suppress = false;

            if (!s_rows.empty()) {
                const FileRows& file = RowsFor(model->m_shared);
                suppress = file.suppress;
                s_stats.suppressed += suppress ? 1 : 0;

                if (file.rows) {
                    for (uint32_t r : *file.rows) {
                        AddRow(at, s_rows[r], camera, reach);
                    }
                }
            }

            for (uint32_t i = 0; model->m_lights && !suppress && i < data->m_lights.m_count; i++) {
                const M2Light& file = data->m_lights[i];
                const M2ModelLight& light = model->m_lights[i];

                if (file.m_type != 1 || !light.m_enabled) {
                    continue;
                }

                s_stats.model_lights++;
                const bool placed = light.m_light.m_stamp != 0 && light.m_light.m_stamp + 1 >= scene->m_frameStamp;
                Found f;
                float scale = 1.0f;

                if (placed) {
                    if (!light.m_light.m_visible) {
                        continue;
                    }

                    f.pos = light.m_light.m_pos;
                    s_stats.placed++;
                } else if (!Place(at, file.m_position, file.m_bone, nullptr, f.pos, nullptr, scale)) {
                    continue;
                }

                f.color = light.m_light.m_dirColor;

                if (Brightest(f.color) <= 0.0f) {
                    C3Vector c(0.0f, 0.0f, 0.0f);
                    float intensity = 1.0f;
                    FirstKey(file.m_diffuseColor, c);
                    FirstKey(file.m_diffuseIntensity, intensity);
                    f.color = c * intensity;
                }

                if (Brightest(f.color) <= 0.001f || !Finite(f.pos)) {
                    continue;
                }

                f.distance = Length(f.pos - camera);

                if (f.distance <= reach) {
                    s_found.push_back(f);
                }
            }
        }

        for (CM2Model* child = model->m_attachList; child; child = child->m_attachNext) {
            Visit(scene, child, camera, reach, depth + 1);
        }
    }

    // Point lights closer than the merge distance become one (Blizzard's torch groups): the colours
    // summed, the position weighted by them, the larger radius. Spots never merge (their cones differ).
    // Neighbours through a hash of merge-distance cells (a city holds thousands of lights: comparing every
    // pair cost hundreds of milliseconds in WXL, Dalaran 2026-10-08).
    void Merge(float distance) {
        if (distance <= 1e-3f) {
            return;
        }

        const float inv = 1.0f / distance, d2 = distance * distance;
        auto key = [inv](const C3Vector& p, int dx, int dy, int dz) {
            const int64_t x = static_cast<int64_t>(std::floor(p.x * inv)) + dx, y = static_cast<int64_t>(std::floor(p.y * inv)) + dy,
                          z = static_cast<int64_t>(std::floor(p.z * inv)) + dz;
            return (static_cast<uint64_t>(x + 1048576) & 0x1FFFFF) | ((static_cast<uint64_t>(y + 1048576) & 0x1FFFFF) << 21) |
                   ((static_cast<uint64_t>(z + 1048576) & 0x1FFFFF) << 42);
        };
        static std::vector<std::pair<uint64_t, uint32_t>> cells;
        static std::vector<C3Vector> original;
        cells.clear();
        original.resize(s_found.size());

        for (uint32_t k = 0; k < s_found.size(); k++) {
            original[k] = s_found[k].pos;

            if (s_found[k].cosOuter < -1.5f) {
                cells.emplace_back(key(s_found[k].pos, 0, 0, 0), k);
            }
        }

        std::sort(cells.begin(), cells.end());

        for (uint32_t a = 0; a < s_found.size(); a++) {
            Found& keep = s_found[a];

            if (keep.merged || keep.cosOuter > -1.5f) {
                continue;
            }

            float wa = keep.color.x + keep.color.y + keep.color.z;

            for (int n = 0; n < 27; n++) {
                const uint64_t k = key(original[a], n % 3 - 1, (n / 3) % 3 - 1, n / 9 - 1);

                for (auto it = std::lower_bound(cells.begin(), cells.end(), std::make_pair(k, 0u)); it != cells.end() && it->first == k; ++it) {
                    const uint32_t b = it->second;
                    Found& other = s_found[b];

                    if (b <= a || other.merged) {
                        continue;
                    }

                    const C3Vector d = original[a] - original[b];

                    if (d.x * d.x + d.y * d.y + d.z * d.z > d2) {
                        continue;
                    }

                    const float wb = other.color.x + other.color.y + other.color.z;
                    const float share = wa + wb > 0.0f ? wb / (wa + wb) : 0.5f;
                    keep.pos = keep.pos + (other.pos - keep.pos) * share;
                    keep.color = keep.color + other.color;
                    keep.radius = std::fmax(keep.radius, other.radius);
                    keep.distance = std::fmin(keep.distance, other.distance);
                    wa += wb;
                    other.merged = true;
                    s_stats.merged++;
                }
            }
        }
    }

    uint32_t Hash(uint32_t x) {
        x ^= x >> 16;
        x *= 0x7FEB352Du;
        x ^= x >> 15;
        x *= 0x846CA68Bu;
        x ^= x >> 16;
        return x;
    }

    float Unit(uint32_t x) { return static_cast<float>(Hash(x) >> 8) * (1.0f / 16777216.0f); }

    // How much a flame dips now, 0..1; each light its own phase from its position, so neighbouring
    // torches don't pulse together (WXL's). Smooth: three sines at unrelated rates. Noise: random levels
    // ~6 a second, eased into each other. Steps: the same levels, held.
    float Flicker(int mode, const C3Vector& pos, double time) {
        const double phase = std::fmod(std::fabs(pos.x * 12.9898 + pos.y * 78.233 + pos.z * 37.719), 1000.0);
        const double t = time + phase;

        if (mode == SXC_LIGHTS_FLICKER_OFF) {
            return 0.0f;
        }

        if (mode == SXC_LIGHTS_FLICKER_NOISE || mode == SXC_LIGHTS_FLICKER_STEPS) {
            const double u = t * 6.0;
            const uint32_t seed = static_cast<uint32_t>(phase * 1000.0);
            const uint32_t k = static_cast<uint32_t>(static_cast<int64_t>(std::floor(u)));
            const float a = Unit(k * 2654435761u + seed), b = Unit((k + 1) * 2654435761u + seed);

            if (mode == SXC_LIGHTS_FLICKER_STEPS) {
                return a;
            }

            float f = static_cast<float>(u - std::floor(u));
            f = f * f * (3.0f - 2.0f * f);
            return a + (b - a) * f;
        }

        const double s = 0.5 * std::sin(t * 7.13) + 0.3 * std::sin(t * 13.71 + 1.3) + 0.2 * std::sin(t * 23.17 + 2.1);
        return static_cast<float>(0.5 + 0.5 * s);
    }

    // The grid snaps to whole cells around the camera. A light goes into every cell its radius reaches
    // (circle against square); a cell holding more than perCell keeps the strongest at its middle (the
    // light's base brightness x (1 - d / reach)^2: no flicker or fade, so the choice doesn't flip).
    void BuildGrid(const C3Vector& camera, float range, float cell, uint32_t perCell) {
        const int half = std::min(static_cast<int>(std::ceil(range / cell)), static_cast<int>(kMaxCells / 2));
        const int cells = std::max(1, half * 2);
        const float ox = (std::floor(camera.x / cell) - half) * cell, oy = (std::floor(camera.y / cell) - half) * cell;
        s_frame.originX = ox;
        s_frame.originY = oy;
        s_frame.cellSize = cell;
        s_frame.cells = static_cast<uint32_t>(cells);
        s_frame.perCell = perCell;
        s_cellLights.assign(static_cast<size_t>(cells) * cells * perCell, kNoLight);
        s_stats.cells = static_cast<uint32_t>(cells);

        struct Entry {
            uint32_t cell;
            float score;
            uint32_t light;
        };
        static std::vector<Entry> entries;
        entries.clear();
        const float halfDiagonal = cell * 0.70710678f;

        for (uint32_t i = 0; i < s_lights.size(); i++) {
            const GpuLight& l = s_lights[i];
            const float r = 1.0f / l.invRadius, lx = l.pos[0] - ox, ly = l.pos[1] - oy;
            const int x0 = std::max(0, static_cast<int>(std::floor((lx - r) / cell))), y0 = std::max(0, static_cast<int>(std::floor((ly - r) / cell)));
            const int x1 = std::min(cells - 1, static_cast<int>(std::floor((lx + r) / cell))), y1 = std::min(cells - 1, static_cast<int>(std::floor((ly + r) / cell)));

            for (int y = y0; y <= y1; y++) {
                for (int x = x0; x <= x1; x++) {
                    const float nx = std::fmax(x * cell, std::fmin(lx, (x + 1) * cell)) - lx, ny = std::fmax(y * cell, std::fmin(ly, (y + 1) * cell)) - ly;

                    if (nx * nx + ny * ny > r * r) {
                        continue;
                    }

                    const float cx = (x + 0.5f) * cell - lx, cy = (y + 0.5f) * cell - ly;
                    const float t = 1.0f - std::fmin(std::sqrt(cx * cx + cy * cy) / (r + halfDiagonal), 1.0f);
                    entries.push_back(Entry{ static_cast<uint32_t>(y * cells + x), s_strength[i] * t * t, i });
                }
            }
        }

        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
            return a.cell != b.cell ? a.cell < b.cell : (a.score != b.score ? a.score > b.score : a.light < b.light);
        });

        for (size_t e = 0; e < entries.size();) {
            const uint32_t c = entries[e].cell;
            uint32_t n = 0;

            for (; e < entries.size() && entries[e].cell == c; e++, n++) {
                if (n < perCell) {
                    s_cellLights[static_cast<size_t>(c) * perCell + n] = entries[e].light;
                }
            }

            s_stats.busiest_cell = std::max(s_stats.busiest_cell, n);
            s_stats.full_cells += n > perCell ? 1 : 0;
        }
    }

    // --- the service -------------------------------------------------------------------------------
    wxl_bool WXL_CALL Available() { return s_available ? WXL_TRUE : WXL_FALSE; }

    void WXL_CALL SetSettings(const sxc_lights_settings* settings) {
        if (!settings || settings->struct_size < sizeof(sxc_lights_settings)) {
            s_haveSettings = false;
            return;
        }

        s_settings = *settings;
        s_haveSettings = true;
    }

    void WXL_CALL Stats(sxc_lights_stats* out) {
        if (!out || out->struct_size < sizeof(sxc_lights_stats)) {
            return;
        }

        const uint32_t size = out->struct_size;
        *out = s_stats;
        out->struct_size = size;
    }

    const char* WXL_CALL Status() {
        if (!s_available) {
            return "off: this renderer doesn't draw our lights (Vulkan with our terrain shader only)";
        }

        if (!Settled()) {
            return "off: no mod turned them on";
        }

        std::snprintf(s_status, sizeof(s_status),
                      "%u models; their own point lights %u (%u placed by the client this frame; left out on %u models by the table); "
                      "%u table lights (%u at the model's origin: attachment missing)\n"
                      "%u in range, %u merged, %u drawn%s; grid %u x %u cells of %.0f yd, busiest cell %u lights, %u cells over their limit; "
                      "stock terrain lights %s",
                      s_stats.models, s_stats.model_lights, s_stats.placed, s_stats.suppressed, s_stats.table_lights, s_stats.attach_missing,
                      s_stats.in_range, s_stats.merged, s_stats.active, s_stats.dropped ? " (the farthest left out: more than 512)" : "", s_stats.cells,
                      s_stats.cells, s_frame.cellSize, s_stats.busiest_cell, s_stats.full_cells, StockTerrainOff() ? "off" : "on");
        return s_status;
    }

    void WXL_CALL SetTable(const sxc_lights_kind* kinds, uint32_t kindCount, const sxc_lights_assignment* rows, uint32_t rowCount) {
        s_kinds.assign(kinds && kindCount ? kinds : nullptr, kinds && kindCount ? kinds + kindCount : nullptr);
        s_rows.clear();
        s_byPath.clear();
        std::unordered_map<uint32_t, int> kindIndex;

        for (size_t k = 0; k < s_kinds.size(); k++) {
            kindIndex[s_kinds[k].id] = static_cast<int>(k);
        }

        for (uint32_t r = 0; rows && r < rowCount; r++) {
            Row row;
            row.a = rows[r];
            row.path = Normalize(rows[r].model_path);
            row.a.model_path = nullptr; // not kept: the caller's
            const auto it = kindIndex.find(rows[r].light_id);
            row.kind = rows[r].light_id && it != kindIndex.end() ? it->second : -1;

            if (row.path.empty()) {
                continue;
            }

            s_byPath[row.path].push_back(static_cast<uint32_t>(s_rows.size()));
            s_rows.push_back(std::move(row));
        }

        s_tableGeneration++;
    }

    const sxc_lights_api s_api = { sizeof(sxc_lights_api), Available, SetSettings, Stats, Status, SetTable };
}

namespace SXC::Lights {
    const sxc_lights_api* Api() {
        return &s_api;
    }

    void SetDevice(bool available, StatusFn status) {
        s_available = available;
        s_deviceStatus = status;
    }

    bool StockTerrainOff() {
        return Settled() && s_settings.stock_off;
    }

    const Frame& Current() {
        const uint32_t size = s_stats.struct_size;
        s_stats = sxc_lights_stats{};
        s_stats.struct_size = size;
        s_frame = Frame{};
        s_found.clear();
        s_lights.clear();
        s_strength.clear();
        const CM2Scene* scene = World::s_m2Scene;

        if (!Settled() || !scene) {
            return s_frame;
        }

        const sxc_lights_settings& set = s_settings;
        const float range = std::fmax(set.range, 8.0f);
        const float radius = std::fmax(set.radius, 1.0f);
        const float cell = std::fmax(set.cell_size, 4.0f);
        const uint32_t perCell = static_cast<uint32_t>(std::min(std::max(set.max_per_cell, 1), static_cast<int32_t>(kMaxPerCell)));
        // The camera: the scene's view origin in the world (row vectors: world = view x the view's inverse).
        const C3Vector camera(scene->m_viewInv.d0, scene->m_viewInv.d1, scene->m_viewInv.d2);

        for (CM2Model* model = scene->m_modelList; model; model = model->m_sceneNext) {
            Visit(scene, model, camera, range, 0);
        }

        s_stats.in_range = static_cast<uint32_t>(s_found.size());
        Merge(set.merge_distance);

        // The nearest kMaxPool, kept in the scan's order (stable between frames: the grid doesn't reshuffle).
        static std::vector<std::pair<float, uint32_t>> order;
        order.clear();

        for (uint32_t k = 0; k < s_found.size(); k++) {
            if (!s_found[k].merged) {
                order.emplace_back(s_found[k].distance, k);
            }
        }

        if (order.size() > kMaxPool) {
            s_stats.dropped = static_cast<uint32_t>(order.size() - kMaxPool);
            std::nth_element(order.begin(), order.begin() + kMaxPool, order.end());
            order.resize(kMaxPool);
            std::sort(order.begin(), order.end(), [](const std::pair<float, uint32_t>& a, const std::pair<float, uint32_t>& b) { return a.second < b.second; });
        }

        const double now = static_cast<double>(OsGetAsyncTimeMs()) * 0.001;
        const float fadeStart = range * (2.0f / 3.0f);

        for (const std::pair<float, uint32_t>& o : order) {
            const Found& f = s_found[o.second];
            const float fade = f.distance <= fadeStart ? 1.0f : std::fmax(0.0f, 1.0f - (f.distance - fadeStart) / (range - fadeStart));

            if (fade <= 0.0f) {
                continue;
            }

            const int mode = f.flickerMode == SXC_LIGHTS_FLICKER_DEFAULT ? SXC_LIGHTS_FLICKER_SMOOTH : f.flickerMode;
            const float amount = std::fmin(f.flickerAmount >= 0.0f ? f.flickerAmount : std::fmax(set.flicker, 0.0f), 1.0f);
            const float speed = (f.flickerSpeed >= 0.0f ? f.flickerSpeed : 1.0f) * (set.flicker_speed > 0.0f ? set.flicker_speed : 1.0f);
            const float scale = set.brightness * fade * (1.0f - amount * Flicker(mode, f.pos, now * speed));
            GpuLight l = {};
            l.pos[0] = f.pos.x;
            l.pos[1] = f.pos.y;
            l.pos[2] = f.pos.z;
            l.invRadius = 1.0f / std::fmax(f.radius > 0.0f ? f.radius : radius, 0.5f);
            l.color[0] = f.color.x * scale;
            l.color[1] = f.color.y * scale;
            l.color[2] = f.color.z * scale;
            l.falloff = f.falloff;
            l.spotDir[0] = f.spotDir.x;
            l.spotDir[1] = f.spotDir.y;
            l.spotDir[2] = f.spotDir.z;
            l.cosOuter = f.cosOuter;
            l.spotScale = f.spotScale;
            s_lights.push_back(l);
            s_strength.push_back((f.color.x + f.color.y + f.color.z) * set.brightness);
        }

        s_stats.active = static_cast<uint32_t>(s_lights.size());
        BuildGrid(camera, range, cell, perCell);

        s_frame.on = true;
        s_frame.debug = set.debug;
        s_frame.lightCount = static_cast<uint32_t>(s_lights.size());
        s_frame.lights = s_lights.empty() ? nullptr : s_lights.data();
        s_frame.cellLights = s_cellLights.data();
        return s_frame;
    }
}
