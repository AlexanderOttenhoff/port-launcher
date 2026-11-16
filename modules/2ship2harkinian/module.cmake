# Injected into 2Ship's top-level project() (named "2s2h") through CMAKE_PROJECT_2s2h_INCLUDE (see build.sh). The game
# target is "2ship", created in the mm/ subdirectory.
set(PLM_MODULE_DIR ${CMAKE_CURRENT_LIST_DIR})

function(plm_make_module)
    target_sources(2ship PRIVATE ${PLM_MODULE_DIR}/../common/lus/lus_shim.cpp ${PLM_MODULE_DIR}/plm_options.cpp
        ${PLM_MODULE_DIR}/../common/savestate/plm_savestate.cpp ${PLM_MODULE_DIR}/../common/android/execinfo_compat.c)
    target_compile_options(2ship PRIVATE -include ${PLM_MODULE_DIR}/../common/android/execinfo_compat.h)
    # ZAPD's crash handler (linked into the game as ZAPDLib) also uses backtrace(); the implementation is the
    # execinfo_compat.c compiled into the game library above.
    target_compile_options(ZAPDLib PRIVATE -include ${PLM_MODULE_DIR}/../common/android/execinfo_compat.h)
    # No PLM_PORT_ARCHIVE: 2ship.o2r is generated on the device into the config dir (plm_options.cpp), not shipped.
    # PLM_ROM_ARCHIVE: the shim passes the ROM as the game's argument while mm.o2r is missing, which takes 2Ship's
    # extractor down its prompt-free "ROMs given on the command line" path.
    target_compile_definitions(2ship PRIVATE PLM_LOG_TAG="plm-2ship" PLM_LUS_CONFIG="2ship2harkinian.json" PLM_ROM_ARCHIVE="mm.o2r" PLM_GAME_MAIN="main")
    target_include_directories(2ship PRIVATE ${PLM_MODULE_DIR}/../../core/include)
    # NDK libc++ doesn't pull headers in transitively as libstdc++ does; same as Ship of Harkinian.
    target_compile_options(2ship PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include sstream>")
    set_target_properties(2ship PROPERTIES OUTPUT_NAME plm_2ship2harkinian)
    # Save states (docs/planning/SAVE_STATES.md): 2Ship carries SoH's SaveStateMgr, but its creation is commented out
    # upstream, so the generic snapshot is used: the decomp's sources (mm/src) put their globals in one section. The
    # system and audio heaps are malloc'd/mmap'd by Heaps_Alloc (src/buffers/heaps.c); a link-time wrap registers
    # them as extra regions (plm_options.cpp). The port layer (mm/2s2h), ZAPD and libultraship stay out.
    get_target_property(plm_sources 2ship SOURCES)
    set(plm_game_sources "")
    foreach(source IN LISTS plm_sources)
        # mm/CMakeLists.txt lists sources relative to mm/; made absolute so the property finds them from here.
        if(source MATCHES "^src/.*\\.c$")
            list(APPEND plm_game_sources ${CMAKE_SOURCE_DIR}/mm/${source})
        endif()
    endforeach()
    set_property(SOURCE ${plm_game_sources} TARGET_DIRECTORY 2ship APPEND PROPERTY COMPILE_OPTIONS
        -include ${PLM_MODULE_DIR}/../common/savestate/plm_game_section.h)
    list(LENGTH plm_game_sources plm_game_count)
    message(STATUS "port-launcher: ${plm_game_count} game sources in the save-state section")
    target_link_options(2ship PRIVATE -Wl,-z,max-page-size=16384 -Wl,--wrap=SDL_GL_SwapWindow
        -Wl,--wrap=SDL_OpenAudioDevice -Wl,--wrap=Heaps_Alloc)
endfunction()

cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL plm_make_module)
