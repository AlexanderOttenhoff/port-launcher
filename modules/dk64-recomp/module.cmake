# Injected into Donkey-Kong-64-Recompiled's top-level project() through CMAKE_PROJECT_DK64Recompiled_INCLUDE (see build.sh).
# Turns the Android libmain.so build (patches/Donkey-Kong-64-Recompiled) into a port-launcher module.
set(PLM_MODULE_DIR ${CMAKE_CURRENT_LIST_DIR})

function(plm_make_module)
    target_sources(DK64Recompiled PRIVATE ${PLM_MODULE_DIR}/../common/n64recomp/recompfrontend_shim.cpp)
    target_compile_definitions(DK64Recompiled PRIVATE PLM_LOG_TAG="plm-dk64"
        # The port's own settings (settings_schema.json): applied through the RecompFrontend config API, with the
        # config-load wrap below (recompfrontend_shim.cpp, "The port's own settings").
        PLM_PORT_SCHEMA)
    target_include_directories(DK64Recompiled PRIVATE ${PLM_MODULE_DIR}/../../core/include)
    set_target_properties(DK64Recompiled PROPERTIES OUTPUT_NAME plm_dk64-recomp)
    target_link_options(DK64Recompiled PRIVATE -Wl,-z,max-page-size=16384 -Wl,--wrap=osViSwapBuffer_recomp
        -Wl,--wrap=_ZN11ultramodern14error_handling11message_boxEPKc
        -Wl,--wrap=_ZN8recompui12show_contextENS_9ContextIdENSt6__ndk117basic_string_viewIcNS1_11char_traitsIcEEEE
        -Wl,--wrap=_ZN8recompui6config8finalizeEv)
    if (PLM_SPLIT_GAME)
        plm_split_game()
    endif()
endfunction()

# Recomp split (docs/planning/ON_DEVICE_RECOMP.md, spike 2; -DPLM_SPLIT_GAME=ON, build.sh --split): the module
# keeps no ROM-derived code. Everything that holds or names game code moves to a second library, plm_game
# (libgame.so): the generated functions (RecompiledFuncs), the RSP microcode (rsp/), the overlay table that lists
# every generated function (register_overlays.cpp) and the patches, whose code calls game functions by name
# (PatchesLib, register_patches.cpp). game_loader.cpp stands in for the five symbols the module used from them.
# For the spike plm_game is built here, on the desktop; the on-device toolkit will do the same link on the phone.
function(plm_split_game)
    set(GAME_SOURCES
        ${CMAKE_SOURCE_DIR}/src/main/register_overlays.cpp
        ${CMAKE_SOURCE_DIR}/src/main/register_patches.cpp
        ${CMAKE_SOURCE_DIR}/rsp/n_aspMain.cpp)
    get_target_property(MODULE_SOURCES DK64Recompiled SOURCES)
    list(REMOVE_ITEM MODULE_SOURCES ${GAME_SOURCES})
    list(APPEND MODULE_SOURCES ${PLM_MODULE_DIR}/game_loader.cpp)
    set_property(TARGET DK64Recompiled PROPERTY SOURCES ${MODULE_SOURCES})
    get_target_property(MODULE_LIBS DK64Recompiled LINK_LIBRARIES)
    list(REMOVE_ITEM MODULE_LIBS RecompiledFuncs PatchesLib)
    # librecomp whole: its os*_recomp/__ll_*_recomp functions were linked in only because generated code called
    # them; without the game code in this link the linker would drop them, and libgame.so needs them from here.
    list(TRANSFORM MODULE_LIBS REPLACE "^librecomp$" "$<LINK_LIBRARY:WHOLE_ARCHIVE,librecomp>")
    set_property(TARGET DK64Recompiled PROPERTY LINK_LIBRARIES ${MODULE_LIBS})
    target_include_directories(DK64Recompiled PRIVATE ${PLM_MODULE_DIR})
    # libgame.so resolves ~180 runtime functions (os*_recomp, cop0_*, get_function, dmem...) against the module.
    # They are already exported (default visibility); export-dynamic keeps that true if a dependency hides them.
    target_link_options(DK64Recompiled PRIVATE -Wl,--export-dynamic)

    add_library(plm_game SHARED ${GAME_SOURCES} ${PLM_MODULE_DIR}/game_entry.cpp)
    set_target_properties(plm_game PROPERTIES OUTPUT_NAME game)
    target_include_directories(plm_game PRIVATE $<TARGET_PROPERTY:DK64Recompiled,INCLUDE_DIRECTORIES> ${PLM_MODULE_DIR})
    target_compile_definitions(plm_game PRIVATE $<TARGET_PROPERTY:DK64Recompiled,COMPILE_DEFINITIONS>)
    target_compile_options(plm_game PRIVATE $<TARGET_PROPERTY:DK64Recompiled,COMPILE_OPTIONS>)
    # Whole archives: nothing in libgame.so references most generated functions directly (the overlay table and
    # the patches do, but only the table's own object pulls them), and all of them must be present.
    # ORDER MATTERS: a patched function is defined twice, both weak (RECOMP_FUNC): the patch in PatchesLib and the
    # original in RecompiledFuncs. With two weak definitions the linker keeps the first it sees, so PatchesLib must
    # come first, as in upstream's own link. Found on the RP6 (2026-11-13): with RecompiledFuncs first, DK64 ran the
    # unpatched func_dk64_boot_800009D0, which reads an uncached (KSEG1) address and crashed at boot.
    # --wrap: the module wraps osViSwapBuffer_recomp (the shim's frame hook); the game's calls must reach the wrapper,
    # which the module exports, not the real function.
    target_link_libraries(plm_game PRIVATE -Wl,--whole-archive PatchesLib RecompiledFuncs -Wl,--no-whole-archive
        DK64Recompiled)
    target_link_options(plm_game PRIVATE -Wl,-Bsymbolic -Wl,-z,max-page-size=16384 -Wl,--wrap=osViSwapBuffer_recomp
        -Wl,--no-undefined)
endfunction()

cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL plm_make_module)
