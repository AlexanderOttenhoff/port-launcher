# Injected into body-harvest-recomp's top-level project() through CMAKE_PROJECT_BodyHarvestRecomp_INCLUDE (see build.sh).
# Turns the Android shared-library build (patches/body-harvest-recomp) into a port-launcher module.
# Why the shim fits a non-RecompFrontend-launcher port: Body Harvest still links recompui/recompinput and librecomp, and
# its patched main() accepts `--rom <path> --skip-launcher` and reads APP_FOLDER_PATH as its settings dir, which is all
# the shim's plm_main assumes.
set(PLM_MODULE_DIR ${CMAKE_CURRENT_LIST_DIR})

function(plm_make_module)
    target_sources(body-harvest-recomp PRIVATE ${PLM_MODULE_DIR}/../common/n64recomp/recompfrontend_shim.cpp)
    target_compile_definitions(body-harvest-recomp PRIVATE PLM_LOG_TAG="plm-bodyharvest")
    target_include_directories(body-harvest-recomp PRIVATE ${PLM_MODULE_DIR}/../../core/include
        # The shim includes json/json.hpp; in this tree it lives in N64ModernRuntime's thirdparty (the BH sources
        # include it the same way).
        ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/thirdparty)
    set_target_properties(body-harvest-recomp PROPERTIES OUTPUT_NAME plm_bh-recomp)
    target_link_options(body-harvest-recomp PRIVATE -Wl,-z,max-page-size=16384 -Wl,--wrap=osViSwapBuffer_recomp
        -Wl,--wrap=_ZN11ultramodern14error_handling11message_boxEPKc
        -Wl,--wrap=_ZN8recompui12show_contextENS_9ContextIdENSt6__ndk117basic_string_viewIcNS1_11char_traitsIcEEEE)
endfunction()

cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL plm_make_module)
