# Injected into Ghostship's top-level project() through CMAKE_PROJECT_Ghostship_INCLUDE (see build.sh).
set(PLM_MODULE_DIR ${CMAKE_CURRENT_LIST_DIR})

function(plm_make_module)
    target_sources(Ghostship PRIVATE ${PLM_MODULE_DIR}/../common/lus/lus_shim.cpp ${PLM_MODULE_DIR}/plm_options.cpp
        ${PLM_MODULE_DIR}/../common/savestate/plm_savestate.cpp ${PLM_MODULE_DIR}/../common/android/execinfo_compat.c)
    target_compile_options(Ghostship PRIVATE -include ${PLM_MODULE_DIR}/../common/android/execinfo_compat.h)
    target_compile_definitions(Ghostship PRIVATE PLM_LOG_TAG="plm-ghostship" PLM_LUS_CONFIG="ghostship.cfg.json" PLM_PORT_ARCHIVE="ghostship.o2r"
        PLM_GAME_MAIN="SDL_main")
    # nlohmann/json comes from the port (its libultraship fetches it); ours would clash with its version.
    target_include_directories(Ghostship PRIVATE ${PLM_MODULE_DIR}/../../core/include)
    # Upstream names the Android library "main" (its in-game C scripting links scripts against it).
    set_target_properties(Ghostship PROPERTIES OUTPUT_NAME plm_ghostship)
    # Torch (the ROM extractor, linked into the game library) and Ghostship's resource importer both define classes
    # named SM64::Painting, SM64::AnimationData, ... In one shared library their identically named template code
    # (shared_ptr control blocks, vtables) collides at link time, so Torch's cleanup ran Ghostship's destructors: the
    # first-start extraction crashed in Companion::Process (ROADMAP open issue). Renaming Torch's copies, in Torch's
    # own sources only, keeps the two apart.
    target_compile_definitions(torch PRIVATE Painting=TorchSM64Painting PaintingData=TorchSM64PaintingData
        AnimationData=TorchSM64AnimationData MovtexQuadData=TorchSM64MovtexQuadData
        TrajectoryData=TorchSM64TrajectoryData)
    # Save states (docs/planning/SAVE_STATES.md): the decomp's own sources put their globals in one section; the port
    # layer (src/port), libultraship and Torch stay out of it. The main pool is added by wrapping main_pool_init.
    get_target_property(plm_sources Ghostship SOURCES)
    set(plm_game_sources "")
    foreach(source IN LISTS plm_sources)
        if(source MATCHES "^(src|lib/ultralib)/.*\\.c$" AND NOT source MATCHES "^src/port/")
            list(APPEND plm_game_sources ${source})
        endif()
    endforeach()
    set_property(SOURCE ${plm_game_sources} APPEND PROPERTY COMPILE_OPTIONS
        -include ${PLM_MODULE_DIR}/../common/savestate/plm_game_section.h)
    list(LENGTH plm_game_sources plm_game_count)
    message(STATUS "port-launcher: ${plm_game_count} game sources in the save-state section")
    target_link_options(Ghostship PRIVATE -Wl,-z,max-page-size=16384 -Wl,--wrap=SDL_GL_SwapWindow
        -Wl,--wrap=SDL_OpenAudioDevice -Wl,--wrap=main_pool_init)
endfunction()

cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL plm_make_module)
