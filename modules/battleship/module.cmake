# Injected into BattleShip's top-level project() (named "ssb64", also the game target) through
# CMAKE_PROJECT_ssb64_INCLUDE (see build.sh).
set(PLM_MODULE_DIR ${CMAKE_CURRENT_LIST_DIR})

function(plm_make_module)
    # port/android_torch_bridge.cpp is upstream's on-device extractor (torch_extract_o2r), which upstream builds into a
    # separate libtorch_runner.so for its own Java first-run screen. We link it and Torch into the game library so the
    # module can extract from the launcher's ROM before the game starts (plm_options.cpp).
    target_sources(ssb64 PRIVATE ${PLM_MODULE_DIR}/../common/lus/lus_shim.cpp ${PLM_MODULE_DIR}/plm_options.cpp
        ${PLM_MODULE_DIR}/../common/savestate/plm_savestate.cpp ${CMAKE_SOURCE_DIR}/port/android_torch_bridge.cpp)
    target_include_directories(ssb64 PRIVATE ${PLM_MODULE_DIR}/../../core/include ${CMAKE_SOURCE_DIR}/torch/src)
    target_link_libraries(ssb64 PRIVATE torch)
    # port/port.cpp includes SDL_main.h on Android, so the entry point is SDL_main.
    target_compile_definitions(ssb64 PRIVATE PLM_LOG_TAG="plm-battleship" PLM_LUS_CONFIG="BattleShip.cfg.json" PLM_PORT_ARCHIVE="f3d.o2r"
        PLM_GAME_MAIN="SDL_main")
    set_target_properties(ssb64 PROPERTIES OUTPUT_NAME plm_battleship)
    # Save states (docs/planning/SAVE_STATES.md): the decomp is its own OBJECT library (ssb64_game), so all of it goes
    # in the section. Outside it and registered at run time (plm_options.cpp): the scene arena (malloc'd once in
    # taskman.c, gPortSceneHeap) and every coroutine's stack and saved registers (BattleShip runs the game loop, each
    # N64 thread and each GObj thread process as a coroutine, port/coroutine_android.cpp). Coroutine create/destroy are
    # wrapped at link time to keep them in a pool (destroyed stacks stay mapped, so states stay valid), and the
    # scene-arena eviction to count scenes (a load is refused after a scene change).
    target_compile_options(ssb64_game PRIVATE -include ${PLM_MODULE_DIR}/../common/savestate/plm_game_section.h)
    target_link_options(ssb64 PRIVATE -Wl,-z,max-page-size=16384 -Wl,--wrap=SDL_GL_SwapWindow
        -Wl,--wrap=SDL_OpenAudioDevice -Wl,--wrap=port_coroutine_create -Wl,--wrap=port_coroutine_destroy
        -Wl,--wrap=port_taskman_evict_arena_caches)
endfunction()

cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL plm_make_module)
