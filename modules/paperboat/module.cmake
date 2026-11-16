# Injected into PaperBoat's top-level project() (named "Paperboat") through CMAKE_PROJECT_Paperboat_INCLUDE (see
# build.sh).
set(PLM_MODULE_DIR ${CMAKE_CURRENT_LIST_DIR})

function(plm_make_module)
    # Upstream's Android build links libzip::zip, expecting libultraship's FetchContent copy (a global target). We
    # provide libzip prebuilt (build/deps, found by libultraship's find_package inside its own directory), and imported
    # targets are directory-scoped, so the top level doesn't see it: import it here too.
    if(NOT TARGET libzip::zip)
        find_package(libzip REQUIRED)
    endif()
    target_sources(Paperboat PRIVATE ${PLM_MODULE_DIR}/../common/lus/lus_shim.cpp ${PLM_MODULE_DIR}/plm_options.cpp
        ${PLM_MODULE_DIR}/../common/savestate/plm_savestate.cpp ${PLM_MODULE_DIR}/bcopy_compat.c)
    # No common/android/execinfo_compat here: PaperBoat brings its own backtrace() for Android (src/port/libc_compat.c).
    # The game's headers #define PACKED (include/macros.h) before they pull in libultraship and spdlog. Upstream
    # fetches spdlog 1.16 (fmt 11); our shared prebuilt deps have spdlog 1.14, whose bundled fmt 10 names a template
    # parameter PACKED, so the macro breaks it. Parsing fmt first, in every C++ file, keeps the macro out of it (its
    # include guards make the later includes no-ops). Cheaper than a second spdlog for one port.
    target_compile_options(Paperboat PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include spdlog/fmt/fmt.h>")
    # src/port/Game.cpp includes SDL_main.h on Android, so the game's entry point is SDL_main.
    target_compile_definitions(Paperboat PRIVATE PLM_LOG_TAG="plm-paperboat" PLM_LUS_CONFIG="paperboat.cfg.json" PLM_PORT_ARCHIVE="paperboat.o2r"
        PLM_GAME_MAIN="SDL_main")
    target_include_directories(Paperboat PRIVATE ${PLM_MODULE_DIR}/../../core/include)
    # Upstream names its Android library "main" (what SDLActivity loads); ours follows the module naming.
    set_target_properties(Paperboat PROPERTIES OUTPUT_NAME plm_paperboat)
    # Save states (docs/planning/SAVE_STATES.md): the decomp's own sources (upstream's GAME_SOURCES: src/ minus
    # src/port, plus nusys from src/os/nusys) put their globals in one section. Paper Mario's heaps are static game
    # arrays (heap_generalHead, heap_battleHead, heap_collisionHead in src/heaps*.c), so they are covered; no malloc
    # replacement was found in the game code. The port layer (src/port: sprite and shape loaders, texture cache),
    # libultraship and Torch stay out.
    get_target_property(plm_sources Paperboat SOURCES)
    set(plm_game_sources "")
    # Upstream lists its sources as absolute paths; compare relative to the project so the checkout's own location
    # (which may itself contain "/src/") doesn't matter.
    foreach(source IN LISTS plm_sources)
        set(rel ${source})
        if(IS_ABSOLUTE ${source})
            file(RELATIVE_PATH rel ${CMAKE_SOURCE_DIR} ${source})
        endif()
        if(rel MATCHES "^src/.*\\.(c|cpp)$" AND NOT rel MATCHES "^src/port/")
            list(APPEND plm_game_sources ${source})
        endif()
    endforeach()
    set_property(SOURCE ${plm_game_sources} APPEND PROPERTY COMPILE_OPTIONS
        -include ${PLM_MODULE_DIR}/../common/savestate/plm_game_section.h)
    list(LENGTH plm_game_sources plm_game_count)
    message(STATUS "port-launcher: ${plm_game_count} game sources in the save-state section")
    target_link_options(Paperboat PRIVATE -Wl,-z,max-page-size=16384 -Wl,--wrap=SDL_GL_SwapWindow
        -Wl,--wrap=SDL_OpenAudioDevice)
endfunction()

cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL plm_make_module)
