# Injected into snowboardkids2-recomp's top-level project() through CMAKE_PROJECT_SnowboardKids2Recompiled_INCLUDE (see build.sh).
# Turns the Android libmain.so build (patches/snowboardkids2-recomp) into a port-launcher module.
set(PLM_MODULE_DIR ${CMAKE_CURRENT_LIST_DIR})

function(plm_make_module)
    target_sources(SnowboardKids2Recompiled PRIVATE ${PLM_MODULE_DIR}/../common/n64recomp/recompfrontend_shim.cpp)
    target_compile_definitions(SnowboardKids2Recompiled PRIVATE PLM_LOG_TAG="plm-sk2"
        # The port's own settings (settings_schema.json): applied through the RecompFrontend config API, with the
        # config-load wrap below (recompfrontend_shim.cpp, "The port's own settings").
        PLM_PORT_SCHEMA)
    target_include_directories(SnowboardKids2Recompiled PRIVATE ${PLM_MODULE_DIR}/../../core/include)
    set_target_properties(SnowboardKids2Recompiled PROPERTIES OUTPUT_NAME plm_sk2-recomp)
    target_link_options(SnowboardKids2Recompiled PRIVATE -Wl,-z,max-page-size=16384 -Wl,--wrap=osViSwapBuffer_recomp
        -Wl,--wrap=_ZN11ultramodern14error_handling11message_boxEPKc
        -Wl,--wrap=_ZN8recompui12show_contextENS_9ContextIdENSt6__ndk117basic_string_viewIcNS1_11char_traitsIcEEEE
        -Wl,--wrap=_ZN8recompui6config8finalizeEv)
endfunction()

cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL plm_make_module)
