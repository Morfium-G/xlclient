#pragma once

#include <cstddef>
#include <cstdint>
#include "Engine/Source/Tempest/C3Vector.h"

#if defined(WOWDECOMP_ARCH_64)

#include "wxl/seams/seams.h"

#else

// Win32 carries no runtime: every seam call compiles to nothing. Before and After are function
// objects that take anything, so a call site reads the same on both architectures.
#include "wxl/abi.h"

namespace wxl::seam {
    namespace detail {
        struct Nop {
            template <class... T>
            void operator()(T&&...) const {}
        };
    }

#define WXL_SEAM(domain, name, symbol, ret, conv, params) \
    namespace domain::name {                              \
        inline constexpr ::wxl::seam::detail::Nop Before{}; \
        inline constexpr ::wxl::seam::detail::Nop After{};  \
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
