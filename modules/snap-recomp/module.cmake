# Injected into Snap64Recomp's top-level project() through CMAKE_PROJECT_Snap64Recomp_INCLUDE (build.sh). Turns
# upstream's own Android library build (libmain.so for SDL's activity) into a port-launcher module.
#
# Why our own small shim (plm_shim.cpp) instead of modules/common/n64recomp/recompfrontend_shim.cpp: Snap64 Recomp is
# neither RecompFrontend nor a Zelda64Recomp-dev UI copy. It has its own frontend (menus drawn in the game's own
# font, settings in snapsettings.json, no recompui), so the shared shim's wraps of recompui::show_context and
# recompui::config::finalize would name symbols that don't exist here, and its entry point is SDL_main (SDL_main.h
# renames main() on Android), not main with SDL_MAIN_HANDLED. What it does need is the same: plm_* exports, first
# frame and pause at the VI swap, the launcher's mods, and --rom.
set(PLM_MODULE_DIR ${CMAKE_CURRENT_LIST_DIR})

function(plm_make_module)
    target_sources(Snap64Recomp PRIVATE ${PLM_MODULE_DIR}/plm_shim.cpp)
    target_compile_definitions(Snap64Recomp PRIVATE PLM_LOG_TAG="plm-snap")
    target_include_directories(Snap64Recomp PRIVATE ${PLM_MODULE_DIR}/../../core/include
        ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib)
    # libplm_snap-recomp.so instead of upstream's libmain.so (the host loads modules by this name).
    set_target_properties(Snap64Recomp PROPERTIES OUTPUT_NAME plm_snap-recomp)
    # 16 KB pages; the VI swap (first frame, pause) and the runtime's error box (take the loading screen down so the
    # box isn't hidden behind it) are wrapped, as in the shared shim.
    target_link_options(Snap64Recomp PRIVATE -Wl,-z,max-page-size=16384 -Wl,--wrap=osViSwapBuffer_recomp
        -Wl,--wrap=_ZN11ultramodern14error_handling11message_boxEPKc)
endfunction()

# The game target is defined later in the project's CMakeLists, so finish the module at the end of that directory.
cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL plm_make_module)
