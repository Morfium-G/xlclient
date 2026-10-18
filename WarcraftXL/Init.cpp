#include "WarcraftXL/Init.h"

#include "WarcraftXL/Binding.h"
#include "wxl/core/init.h"

namespace WarcraftXL {
    void Init() {
        // The runtime is a DLL and reads the client only through what it is handed here: every
        // row of the binding table, in table order, each the function Binding.cpp defines for it,
        // and the rows themselves, which the runtime checks against its own.
        wxl_core_init init = {};
        init.struct_size = sizeof(init);
        init.row_count = WXL_CORE_INIT_ROW_COUNT;
        init.rows = wxl_core_init_rows;
#define WXL_BINDING(name, ret, params, args, absent) init.name = &WarcraftXL::Binding::name;
#include "wxl/core/binding.def"
#undef WXL_BINDING
        WarcraftXLInit(&init);
    }
}
