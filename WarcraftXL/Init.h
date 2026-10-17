// Bringing the WarcraftXL runtime up from the client's main.
#pragma once

namespace WarcraftXL {
    // Hands the runtime the client's accessors and starts it: the mod host, every mod under Mods\
    // next to the executable, then WXL_STAGE_EARLY. Call once, on the main thread, after the
    // process is set up and before CommonMain reads anything a mod may want to change. The outcome
    // is in Logs\WarcraftXL.log next to the executable; the client runs with or without mods.
    void Init();
}
