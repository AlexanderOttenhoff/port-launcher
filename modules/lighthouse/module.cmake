# Injected into Lighthouse's top-level project() through CMAKE_PROJECT_Lighthouse_INCLUDE (see build.sh).
set(PLM_MODULE_DIR ${CMAKE_CURRENT_LIST_DIR})

function(plm_make_module)
    target_sources(Lighthouse PRIVATE ${PLM_MODULE_DIR}/../common/lus/lus_shim.cpp ${PLM_MODULE_DIR}/plm_options.cpp
        ${PLM_MODULE_DIR}/../common/savestate/plm_savestate.cpp
        # The game heap (bk_malloc & co., patch 0011) lives in this arena so save states capture it.
        ${PLM_MODULE_DIR}/../common/savestate/plm_arena.c
        ${PLM_MODULE_DIR}/../common/android/execinfo_compat.c)
    # backtrace() for Android below API 33 (the port uses it for traces), without patching the port.
    target_compile_options(Lighthouse PRIVATE -include ${PLM_MODULE_DIR}/../common/android/execinfo_compat.h)
    target_compile_definitions(Lighthouse PRIVATE PLM_LOG_TAG="plm-lighthouse" PLM_LUS_CONFIG="lighthouse.cfg.json" PLM_PORT_ARCHIVE="lighthouse.o2r"
        PLM_ROM_ARCHIVE="bk.o2r" PLM_GAME_MAIN="SDL_main")
    target_include_directories(Lighthouse PRIVATE ${PLM_MODULE_DIR}/../../core/include ${PLM_MODULE_DIR}/../../core/third_party)
    set_target_properties(Lighthouse PROPERTIES OUTPUT_NAME plm_lighthouse)
    # Save states (docs/planning/SAVE_STATES.md): the game's own sources (the decomp and its libultra) put their
    # globals in one section; the port layer (src/port) and libultraship stay out of it.
    get_target_property(plm_sources Lighthouse SOURCES)
    set(plm_game_sources "")
    foreach(source IN LISTS plm_sources)
        if(source MATCHES "^(src|lib/ultralib)/.*\\.c$" AND NOT source MATCHES "^src/port/")
            list(APPEND plm_game_sources ${source})
        endif()
    endforeach()
    # The arena allocator's heap and free list must be in the state too, so it goes in the section with the game.
    list(APPEND plm_game_sources ${PLM_MODULE_DIR}/../common/savestate/plm_arena.c)
    set_property(SOURCE ${plm_game_sources} APPEND PROPERTY COMPILE_OPTIONS
        -include ${PLM_MODULE_DIR}/../common/savestate/plm_game_section.h)
    list(LENGTH plm_game_sources plm_game_count)
    message(STATUS "port-launcher: ${plm_game_count} game sources in the save-state section")
    target_link_options(Lighthouse PRIVATE -Wl,-z,max-page-size=16384 -Wl,--wrap=SDL_GL_SwapWindow -Wl,--wrap=SDL_OpenAudioDevice)
endfunction()

cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL plm_make_module)
