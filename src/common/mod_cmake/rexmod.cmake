# rexmod.cmake - shared CMake helper for mods_src/* code mods.
#
# Include this (after project()) from a mod's own CMakeLists.txt, then call
# rexmod_add_plugin() instead of add_library(). Keeps every mod's build
# config (C++ standard, rex::runtime link, toolchain expectations) consistent
# with scripts/build.py's own settings, since a mod DLL must be ABI-compatible
# with the exe it loads into.
cmake_minimum_required(VERSION 3.25)

if(NOT TARGET rex::runtime)
    find_package(rexglue CONFIG REQUIRED)
endif()

function(rexmod_add_plugin target_name)
    add_library(${target_name} SHARED ${ARGN})
    set_target_properties(${target_name} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
    )
    # The game (EternalSonataReprise) ships only the matching-variant runtime
    # DLLs next to its exe and the mod loader resolves <stem><postfix>.dll
    # for the host config first (see rex::system::LoadModPlugin), so the
    # output binary must carry the build config's postfix -- RelWithDebInfo
    # (the game's default) produces <name>rd.dll linking rexruntimerd.dll.
    # Release keeps a bare name; Debug gets "d" (CMake's default anyway).
    #
    # Set as per-target properties, NOT the CMAKE_*_POSTFIX variables: those
    # are snapshotted onto a target when add_library() runs, so setting them
    # afterwards (as rexmod_add_plugin() does, since the target is created
    # above) would have no effect on the output name.
    set_target_properties(${target_name} PROPERTIES
        RELWITHDEBINFO_POSTFIX "rd"
        DEBUG_POSTFIX "d"
        RELEASE_POSTFIX ""
    )
    # PRIVATE: a mod plugin has no consumers of its own. Linking rex::runtime
    # (never the rexcore/rexui OBJECT libraries) reuses the same in-process
    # ImGui drawer, keybind registry, and kernel state as the host exe --
    # mirrors how the rexgpu-xenos GPU plugin links against rexruntime.
    target_link_libraries(${target_name} PRIVATE rex::runtime)
    # Header-only helpers shared across mods (e.g. <rexmod/text_patch.h>)
    # live under mods_src/common/include -- every mod gets this for free
    # rather than each needing its own relative include path.
    # CMAKE_CURRENT_FUNCTION_LIST_DIR (not CMAKE_CURRENT_LIST_DIR, which
    # inside a function resolves against the *caller's* CMakeLists.txt)
    # so this points at mods_src/common/mod_cmake regardless of which
    # mod's CMakeLists.txt calls rexmod_add_plugin().
    target_include_directories(${target_name} PRIVATE
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../include
    )
endfunction()
