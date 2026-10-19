#include "SXC/Sxc.h"

#include "Engine/Source/Event/Event.h"
#include "SXC/Data/Data.h"
#include "SXC/ImmersiveAzeroth/Tables.h"
#include "SXC/Settings/Settings.h"

namespace {
    // Every frame, in the glue screens as in the world, whatever the renderer.
    int32_t Idle(const void* data, void* param) {
        SXC::Settings::Tick();
        SXC::Data::Tick();
        return 1;
    }
}

namespace SXC {
    void Startup() {
        static bool s_started;

        if (s_started) {
            return;
        }

        s_started = true;
        // Modules compiled into the client. Explicit calls: SXC is a static library, and an object
        // file nothing refers to would be left out of the link, its static registrations with it.
        ImmersiveAzeroth::RegisterTables();
        EventRegister(EVENT_ID_IDLE, Idle);
    }
}
