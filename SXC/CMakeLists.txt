# SXC: our own layer in the client (orchestration/docs/decisions/0005-fork-first.md). A static library
# linked into the client executables from here, so the root CMakeLists.txt carries only the
# add_subdirectory line that pulls this folder in.

# Linking targets defined in other directories.
cmake_policy(SET CMP0079 NEW)

file(GLOB_RECURSE SXC_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_library(SXC STATIC
    ${SXC_SOURCES}
)

target_include_directories(SXC
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)

# Every executable that links the engine libraries, which call into SXC (the root's list of
# executables, including tools that exist only on some upstream branches).
foreach (target Wow WowClient MapViewer M2Viewer GxTest MpqTest BlpTest DbcTest AssetCheck)
    if (TARGET ${target})
        target_link_libraries(${target} PRIVATE SXC)
    endif ()
endforeach ()
