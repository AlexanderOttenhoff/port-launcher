# Injected into MegaMan64Recompiled's top-level project() through CMAKE_PROJECT_MegaMan64Recompiled_INCLUDE (build.sh).
# Turns the Android shared-library build (patches/MegaMan64Recompiled) into a port-launcher module.
#
# Why the RecompFrontend shim although this is a "B2" project (own Zelda64Recomp-dev style UI, no RecompFrontend):
# everything the shim calls or wraps exists here with the same signatures: main() (SDL_MAIN_HANDLED), the
# --rom/--skip-launcher/--game-mode arguments (our game patch), ultramodern::quit, osViSwapBuffer_recomp,
# ultramodern::error_handling::message_box, recomp::overlays::register_base_export, and the UI's own
# recompui::show_context(ContextId, std::string_view) (src/ui/ui_state.cpp, same mangled name as RecompFrontend's).
# So no per-game shim like JFG's plm_shim.cpp is needed. PLM_C_STICK_MODIFIER stays off (needs RecompFrontend input).
set(PLM_MODULE_DIR ${CMAKE_CURRENT_LIST_DIR})

function(plm_make_module)
    target_sources(MegaMan64Recompiled PRIVATE ${PLM_MODULE_DIR}/../common/n64recomp/recompfrontend_shim.cpp)
    target_compile_definitions(MegaMan64Recompiled PRIVATE PLM_LOG_TAG="plm-mm64"
        # The port's own settings (settings_schema.json, tools/schema/oldui_schema.py): written into the old UI's json
        # files before zelda64::load_config(), live through its load_<file>_config() (recompfrontend_shim.cpp,
        # "old-UI recomps"). No wrap needed.
        PLM_OLDUI_SCHEMA)
    target_include_directories(MegaMan64Recompiled PRIVATE ${PLM_MODULE_DIR}/../../core/include
        ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/librecomp/include ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib)
    set_target_properties(MegaMan64Recompiled PROPERTIES OUTPUT_NAME plm_mm64-recomp)
    target_link_options(MegaMan64Recompiled PRIVATE -Wl,-z,max-page-size=16384 -Wl,--wrap=osViSwapBuffer_recomp
        -Wl,--wrap=_ZN11ultramodern14error_handling11message_boxEPKc
        -Wl,--wrap=_ZN8recompui12show_contextENS_9ContextIdENSt6__ndk117basic_string_viewIcNS1_11char_traitsIcEEEE)
endfunction()

# The game target is defined later in the project's CMakeLists, so finish the module at the end of that directory.
cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL plm_make_module)
