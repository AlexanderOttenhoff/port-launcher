# Injected into Shipwright's top-level project() (named "Ship") through CMAKE_PROJECT_Ship_INCLUDE (see build.sh).
# The game target is "soh", created in the soh/ subdirectory.
set(PLM_MODULE_DIR ${CMAKE_CURRENT_LIST_DIR})

# SoH links SDL2_net::SDL2_net; our static SDL2_net (build.sh) exports SDL2_net::SDL2_net-static only. Find it here,
# at the top level before soh/ is configured, and give it the name SoH expects (parent-directory targets are visible
# in soh/).
find_package(SDL2_net REQUIRED)
if(NOT TARGET SDL2_net::SDL2_net)
    add_library(SDL2_net::SDL2_net INTERFACE IMPORTED)
    set_target_properties(SDL2_net::SDL2_net PROPERTIES INTERFACE_LINK_LIBRARIES SDL2_net::SDL2_net-static)
endif()

function(plm_make_module)
    target_sources(soh PRIVATE ${PLM_MODULE_DIR}/../common/lus/lus_shim.cpp ${PLM_MODULE_DIR}/plm_options.cpp
        ${PLM_MODULE_DIR}/../common/android/execinfo_compat.c)
    target_compile_options(soh PRIVATE -include ${PLM_MODULE_DIR}/../common/android/execinfo_compat.h)
    # PLM_ROM_ARCHIVE: the shim passes the ROM as the game's argument while oot.o2r is missing, which takes SoH's
    # extractor down its "ROMs given on the command line" path (patch 0001 removes the prompt at its end).
    target_compile_definitions(soh PRIVATE PLM_LOG_TAG="plm-shipwright" PLM_LUS_CONFIG="shipofharkinian.json" PLM_PORT_ARCHIVE="soh.o2r"
        PLM_ROM_ARCHIVE="oot.o2r" PLM_GAME_MAIN="main")
    # SoH includes <SDL2/SDL_net.h>; the static SDL2_net target only exports include/SDL2, so add the prefix's
    # include/ itself (build/modules/shipwright/deps, three levels above its CMake config dir).
    get_filename_component(plm_soh_deps_include "${SDL2_net_DIR}/../../../include" ABSOLUTE)
    target_include_directories(soh PRIVATE ${PLM_MODULE_DIR}/../../core/include ${plm_soh_deps_include})
    # NDK libc++ doesn't pull <sstream> in transitively as libstdc++ does (SohMenuRandomizer.cpp uses
    # std::stringstream without including it); include it in every C++ file rather than patching each.
    target_compile_options(soh PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include sstream>")
    set_target_properties(soh PROPERTIES OUTPUT_NAME plm_shipwright)
    # Save states: SoH has its own (soh/soh/Enhancements/savestates.cpp: the system and audio heaps, gSaveContext and
    # each game file's registered statics, processed between frames in graph.c). The module drives that instead of
    # the generic game-section snapshot (docs/planning/SAVE_STATES.md), so no section, no plm_savestate.cpp here.
    target_link_options(soh PRIVATE -Wl,-z,max-page-size=16384 -Wl,--wrap=SDL_GL_SwapWindow
        -Wl,--wrap=SDL_OpenAudioDevice)
endfunction()

cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL plm_make_module)
