# Injected into SpaghettiKart's top-level project() (named "Spaghettify") through CMAKE_PROJECT_Spaghettify_INCLUDE
# (see build.sh).
set(PLM_MODULE_DIR ${CMAKE_CURRENT_LIST_DIR})

function(plm_make_module)
    target_sources(Spaghettify PRIVATE ${PLM_MODULE_DIR}/../common/lus/lus_shim.cpp ${PLM_MODULE_DIR}/plm_options.cpp
        ${PLM_MODULE_DIR}/../common/savestate/plm_savestate.cpp ${PLM_MODULE_DIR}/../common/android/execinfo_compat.c)
    # backtrace() for Android below API 33, in case the port or its libultraship uses it, without patching the port.
    target_compile_options(Spaghettify PRIVATE -include ${PLM_MODULE_DIR}/../common/android/execinfo_compat.h)
    target_compile_definitions(Spaghettify PRIVATE PLM_LOG_TAG="plm-spaghettikart" PLM_LUS_CONFIG="spaghettify.cfg.json" PLM_PORT_ARCHIVE="spaghetti.o2r"
        PLM_GAME_MAIN="SDL_main")
    # nlohmann/json comes from the port's libultraship deps (ours from build/deps is the same one, via CMAKE_PREFIX_PATH);
    # core/third_party isn't added so two copies can't clash.
    # compat/: <fmt/format.h> for src/port/pak.cpp (see compat/fmt/format.h).
    target_include_directories(Spaghettify PRIVATE ${PLM_MODULE_DIR}/../../core/include ${PLM_MODULE_DIR}/compat)
    set_target_properties(Spaghettify PROPERTIES OUTPUT_NAME plm_spaghettikart)

    # Upstream builds with -flto=auto (cmake/SetFlags.cmake). On Android it buys little, costs a long, memory-hungry
    # single-threaded link (the PC has no swap), and LTO plus -Wl,--wrap (our frame and audio hooks) is a known
    # source of hooks silently not applying. Strip it from the target's own options.
    foreach(prop COMPILE_OPTIONS LINK_OPTIONS)
        get_target_property(opts Spaghettify ${prop})
        if(opts)
            list(REMOVE_ITEM opts -flto=auto)
            set_target_properties(Spaghettify PROPERTIES ${prop} "${opts}")
        endif()
    endforeach()

    # Save states (docs/planning/SAVE_STATES.md): the decomp's own C sources put their globals in one section. That
    # includes MK64's memory pool (sMemoryPool, src/buffers/memory_pool.c) and audio heap (gAudioHeap), so no heap
    # registration is needed. The port layer (src/port), SpaghettiKart's C++ engine (src/engine: World, the C++
    # actors and tracks, all on the C++ heap), its mod loader (src/mods) and libultraship stay out: that C++ state is
    # NOT captured, which is the main risk for this port (see docs/planning/SAVE_STATES.md).
    get_target_property(plm_sources Spaghettify SOURCES)
    set(plm_game_sources "")
    foreach(source IN LISTS plm_sources)
        if(source MATCHES "^(src|courses|assets/code)/.*\\.c$" AND NOT source MATCHES "^src/(port|engine|mods)/")
            list(APPEND plm_game_sources ${source})
        endif()
    endforeach()
    set_property(SOURCE ${plm_game_sources} APPEND PROPERTY COMPILE_OPTIONS
        -include ${PLM_MODULE_DIR}/../common/savestate/plm_game_section.h)
    list(LENGTH plm_game_sources plm_game_count)
    message(STATUS "port-launcher: ${plm_game_count} game sources in the save-state section")
    target_link_options(Spaghettify PRIVATE -Wl,-z,max-page-size=16384 -Wl,--wrap=SDL_GL_SwapWindow
        -Wl,--wrap=SDL_OpenAudioDevice)
endfunction()

cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL plm_make_module)
