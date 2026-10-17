// The seams, for the client functions that call them.
//
// A seam is a Before and an After the runtime defines for one client function, declared once in
// core's seam table (wxl/seams/seams.def) and called from inside that function:
//
//     void World::Preload(const C3Vector& position, int32_t showLoading) {
//         wxl::seam::World::Preload::Before(WarcraftXL::Vec3(position), showLoading);
//         ...
//         wxl::seam::World::Preload::After(WarcraftXL::Vec3(position), showLoading);
//     }
//
// The parameters cross a C boundary: a reference is a pointer, a client class is its SDK struct.
// The adapters below make that conversion at the call.
#pragma once

#include <cstddef>
#include <cstdint>
#include "Engine/Source/Tempest/C3Vector.h"

#if defined(WOWDECOMP_ARCH_64)

#include "wxl/seams/seams.h"

#else

// Win32 carries no runtime: the same table, expanded to functions that do nothing, so the calls
// in the client's source compile unchanged.
#include "wxl/abi.h"

namespace wxl::seam {
#define WXL_SEAM(domain, name, symbol, params)  \
    namespace domain::name {                    \
        inline void Before params {}            \
        inline void After params {}             \
    }
#include "wxl/seams/seams.def"
#undef WXL_SEAM
}

#endif

namespace WarcraftXL {
    static_assert(sizeof(C3Vector) == sizeof(wxl_vec3) && offsetof(C3Vector, x) == offsetof(wxl_vec3, x)
                      && offsetof(C3Vector, y) == offsetof(wxl_vec3, y) && offsetof(C3Vector, z) == offsetof(wxl_vec3, z),
                  "C3Vector and wxl_vec3 are the same three floats, or the seams cannot pass one as the other");

    // A client position as the seams take it.
    inline const wxl_vec3* Vec3(const C3Vector& v) {
        return reinterpret_cast<const wxl_vec3*>(&v);
    }
}
