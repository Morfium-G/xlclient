#pragma once

#include "wxl/core/init.h"

// The client's side of the binding: one function per row of core's wxl/core/binding.def, with
// the accessor's own signature, defined in Binding.cpp against the client's classes. Init.cpp
// assigns each to its member of wxl_core_init, so a row without a function here fails to link
// and one with the wrong signature fails to compile.
namespace WarcraftXL::Binding {
#define WXL_BINDING(name, ret, params, args, absent) ret WXL_CALL name params;
#include "wxl/core/binding.def"
#undef WXL_BINDING
}
