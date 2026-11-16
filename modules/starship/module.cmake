# Injected into Starship's top-level project() through CMAKE_PROJECT_Starship_INCLUDE (see build.sh).
set(PLM_MODULE_DIR ${CMAKE_CURRENT_LIST_DIR})

function(plm_make_module)
    target_sources(Starship PRIVATE ${PLM_MODULE_DIR}/../common/lus/lus_shim.cpp ${PLM_MODULE_DIR}/plm_options.cpp
        ${PLM_MODULE_DIR}/../common/savestate/plm_savestate.cpp)
    target_compile_definitions(Starship PRIVATE PLM_LOG_TAG="plm-starship" PLM_LUS_CONFIG="starship.cfg.json" PLM_PORT_ARCHIVE="starship.o2r" PLM_GAME_MAIN="SDL_main")
    target_include_directories(Starship PRIVATE ${PLM_MODULE_DIR}/../../core/include ${PLM_MODULE_DIR}/../../core/third_party)
    set_target_properties(Starship PROPERTIES OUTPUT_NAME plm_starship)
    # Save states (docs/planning/SAVE_STATES.md): the decomp's own sources put their globals in one section (this
    # includes its heaps, sMemoryBuffer and gAudioHeap); the port layer (src/port) and libultraship stay out of it.
    get_target_property(plm_sources Starship SOURCES)
    set(plm_game_sources "")
    foreach(source IN LISTS plm_sources)
        if(source MATCHES "^src/.*\\.c$" AND NOT source MATCHES "^src/port/")
            list(APPEND plm_game_sources ${source})
        endif()
    endforeach()
    set_property(SOURCE ${plm_game_sources} APPEND PROPERTY COMPILE_OPTIONS
        -include ${PLM_MODULE_DIR}/../common/savestate/plm_game_section.h)
    list(LENGTH plm_game_sources plm_game_count)
    message(STATUS "port-launcher: ${plm_game_count} game sources in the save-state section")
    target_link_options(Starship PRIVATE -Wl,-z,max-page-size=16384 -Wl,--wrap=SDL_GL_SwapWindow -Wl,--wrap=SDL_OpenAudioDevice)
endfunction()

cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL plm_make_module)
