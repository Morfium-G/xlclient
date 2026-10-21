#pragma once

#include <cstdint>
#include <vector>
#include "Engine/Source/Tempest/C3Vector.h"

class CM2Model;

// SXC's trails in the covers (step 5d): where units' feet press the cover down. Two world-aligned
// toroidal layers around the player (cell (i, j) at [mod(j, size) * size + mod(i, size)], 4 floats):
// a fine one (0.125 yd) that fills back in by the covers' relax time, a coarse one (0.5 yd) that keeps
// trails longer (the look's persist time). Per cell: the ceiling the cover was pressed down to (world z;
// 1e6 untouched) and when, the rim's strength (0..1) and when. The terrain's tessellation reads them
// and lets the ceiling rise back over time (Shaders/TerrainCover.glsl), so nothing here relaxes cells.
//
// Feet are found from the units' own footstep events (the client's footprints use them): an event names
// a bone and a contact point on it (CM2Model::FindEvent), which is followed every frame from then on;
// the foot carves wherever it passes, so a foot dragged through deep snow leaves a furrow.
namespace SXC::Cover::Trails {
    constexpr float kUntouched = 1.0e6f;

    struct Rect {
        int32_t x, y, w, h; // texels
    };

    struct Layer {
        uint32_t size = 0;
        float cell = 1.0f;
        int32_t firstI = 0, firstJ = 0;
        std::vector<float> cells;
        std::vector<Rect> dirty; // changed since the device last took them
        bool full = true;        // everything changed (a new window): upload all of it
        uint32_t version = 1;
    };

    struct Settings {
        bool on = false;
        float reach = 45.0f;     // yd: units this close to the player carve
    };

    // The footstep seam: CGUnit_C::AnimEventCallback, for every event (non-footsteps are ignored).
    void Event(CM2Model* model, uint32_t eventId, const C3Vector& position, uint64_t guid);

    // Once per frame (SXC::Cover::Current): follow the known feet, carve, move the windows with the player.
    void Update(const Settings& settings);

    const Layer& Fine();
    const Layer& Coarse();
    // The device took the changes: forget them.
    void TookChanges();

    // Seconds the trails' clock has run (the shader's "now").
    float Now();

    struct Stats {
        uint32_t feet = 0;       // feet followed now
        uint32_t stamps = 0;     // carvings last frame
        uint64_t footsteps = 0;  // footstep events seen
    };
    Stats GetStats();

    // Forget everything (the world left, trails off).
    void Reset();
}
