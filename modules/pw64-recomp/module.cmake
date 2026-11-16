# Injected into pilotwings-64-recomp's top-level project() through CMAKE_PROJECT_Pilotwings64Recomp_INCLUDE (see
# build.sh). Turns the Android shared-library build (patches/pilotwings-64-recomp) into a port-launcher module.
# Why the shared RecompFrontend shim fits a port with its own launcher: Pilotwings 64 still links recompui/recompinput
# and librecomp, and its patched main() accepts `--rom <path> --skip-launcher` and reads APP_FOLDER_PATH as its
# settings dir, which is all the shim's plm_main assumes (same as modules/bh-recomp: same author, same structure;
# Body Harvest's main.cpp was copied from this one).
set(PLM_MODULE_DIR ${CMAKE_CURRENT_LIST_DIR})

function(plm_make_module)
    target_sources(Pilotwings64Recomp PRIVATE ${PLM_MODULE_DIR}/../common/n64recomp/recompfrontend_shim.cpp)
    target_compile_definitions(Pilotwings64Recomp PRIVATE PLM_LOG_TAG="plm-pilotwings64")
    target_include_directories(Pilotwings64Recomp PRIVATE ${PLM_MODULE_DIR}/../../core/include
        # The shim includes json/json.hpp; in this tree it lives in N64ModernRuntime's thirdparty.
        ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/thirdparty)
    set_target_properties(Pilotwings64Recomp PROPERTIES OUTPUT_NAME plm_pw64-recomp)
    target_link_options(Pilotwings64Recomp PRIVATE -Wl,-z,max-page-size=16384 -Wl,--wrap=osViSwapBuffer_recomp
        -Wl,--wrap=_ZN11ultramodern14error_handling11message_boxEPKc
        -Wl,--wrap=_ZN8recompui12show_contextENS_9ContextIdENSt6__ndk117basic_string_viewIcNS1_11char_traitsIcEEEE)
endfunction()

# Deferred: the target is declared further down the top-level CMakeLists than project(), where this file runs.
cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL plm_make_module)
