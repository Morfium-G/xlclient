# WarcraftXL: the runtime this client loads mods with.
#
# The 64-bit client builds core's WarcraftXL.dll from the checkout beside this one, links it, and
# calls into it from three places in its own source: WarcraftXLInit from Win64/WinMain.cpp (see
# Init.h) and the seams' Before and After from the functions they attach to (see Seams.h). The
# DLL lands next to the executable in bin/Win/, which is also where Mods/ and Logs/ go.
#
# Win32 is the configuration whose struct layouts are checked against the original binary; it
# carries no runtime and the seam calls compile to nothing there. Only the include directories
# the seam table needs are added.
#
# Nothing conditional belongs in the root CMakeLists.txt: core/tools/layouts/measure_layouts.py
# reads it and refuses a condition it does not know. The conditions live here.

cmake_minimum_required(VERSION 3.21)

set(WXL_CORE_DIR "${CMAKE_SOURCE_DIR}/../core" CACHE PATH "Path to a checkout of WarcraftXL/core")
set(WXL_SDK_DIR "${CMAKE_SOURCE_DIR}/../sdk" CACHE PATH "Path to a checkout of WarcraftXL/sdk")

foreach (dir WXL_CORE_DIR WXL_SDK_DIR)
    if (NOT EXISTS "${${dir}}/include/wxl")
        message(FATAL_ERROR "${dir} (${${dir}}) is not a WarcraftXL checkout: include/wxl is missing. "
            "Clone WarcraftXL/core and WarcraftXL/sdk beside this repository, or pass -D${dir}=<path>.")
    endif ()
endforeach ()

if (NOT WOWDECOMP_ARCH_64)
    target_include_directories(WorldClient PRIVATE "${WXL_CORE_DIR}/include" "${WXL_SDK_DIR}/include")
    return()
endif ()

# --- The runtime: core, built here as win-x64 ------------------------------------------------------
set(WXL_CORE_ARCH "win-x64" CACHE STRING "Architecture of the core build tree" FORCE)
set(WXL_CORE_MODS OFF CACHE BOOL "The mods core ships are win-x86 only" FORCE)

# Core links the CRT statically, through CMAKE_MSVC_RUNTIME_LIBRARY. The root of this tree
# predates the policy that makes that property count (CMP0091), so the runtime library here is
# whatever the inherited flags say: /MD. Rewritten to /MT while core's directory is read, so the
# DLL built here is the one core's own build produces, and put back for everything below, the
# client's side of the boundary included: that is compiled into the client and keeps its CRT.
set(WXL_SAVED_CONFIGS "")
foreach (config ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
    string(TOUPPER ${config} config)
    list(APPEND WXL_SAVED_CONFIGS ${config})
    set(WXL_SAVED_FLAGS_${config} "${CMAKE_CXX_FLAGS_${config}}")
    string(REPLACE "/MD" "/MT" CMAKE_CXX_FLAGS_${config} "${CMAKE_CXX_FLAGS_${config}}")
endforeach ()
add_subdirectory("${WXL_CORE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/core")
foreach (config ${WXL_SAVED_CONFIGS})
    set(CMAKE_CXX_FLAGS_${config} "${WXL_SAVED_FLAGS_${config}}")
endforeach ()

# The DLL goes where the client executables go, so the loader finds it beside them.
set_target_properties(wxl-core PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin/Win")
foreach (config ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
    string(TOUPPER ${config} config)
    set_target_properties(wxl-core PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${config} "${CMAKE_SOURCE_DIR}/bin/Win")
endforeach ()

# --- The client's side of the boundary -------------------------------------------------------------
# What the client hands the runtime (the accessors of wxl_core_init) and the adapters the seam
# calls use. Linked by every executable and by WorldClient, whose World.cpp holds the seam calls.
add_library(WarcraftXLClient STATIC Init.cpp)
target_include_directories(WarcraftXLClient
    PUBLIC
        "${WXL_CORE_DIR}/include"
        "${WXL_SDK_DIR}/include"
)
# The client's own headers are external to this code: it is held to /W4 /permissive-, they are
# not (the decompile carries its warnings at every level).
target_include_directories(WarcraftXLClient SYSTEM PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(WarcraftXLClient PUBLIC wxl-core)
target_compile_options(WarcraftXLClient PRIVATE /W4 /permissive- /external:W0)

target_link_libraries(WorldClient PRIVATE WarcraftXLClient)
foreach (target Wow WowClient)
    if (TARGET ${target})
        target_link_libraries(${target} PRIVATE WarcraftXLClient)
    endif ()
endforeach ()
