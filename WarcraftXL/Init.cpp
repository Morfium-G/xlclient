// Bringing the WarcraftXL runtime up: the accessors the client hands it, and the call.
#include "WarcraftXL/Init.h"

#include "WoW/Source/WorldClient/World.h"
#include "wxl/core/init.h"

namespace {
    // The runtime is a DLL and reads the client only through what it is handed here.
    int32_t WXL_CALL MapId() {
        return World::QueryMapId();
    }
}

namespace WarcraftXL {
    void Init() {
        wxl_core_init init = {};
        init.struct_size = sizeof(init);
        init.map_id = &MapId;
        WarcraftXLInit(&init);
    }
}
