#pragma once

#include <cstdint>

class WeatherRec;

// sxc.world's seams in the client (SXC/Api/sxc_world.h): each called from one marked line.
namespace SXC::WorldApi {
    // SMSG_WEATHER, before the client applies it: notes the server's weather and answers true while a
    // test weather (sxc_world_api::set_weather) holds it back. It's applied when the test ends.
    bool HoldServerWeather(uint32_t type, float intensity, const WeatherRec* rec, bool immediate, float transition);
}
