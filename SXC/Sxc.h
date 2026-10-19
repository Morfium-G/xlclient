#pragma once

// SXC's entry points from the client.
namespace SXC {
    // Once, from InitializeClient after the client's DBCs are loaded (SFile works, the window may not
    // exist yet): registers the modules' tables and SXC's per-frame work (the idle event).
    void Startup();
}
