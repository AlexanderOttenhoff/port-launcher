# Injected into BanjoRecomp's top-level project() through CMAKE_PROJECT_BanjoRecompiled_INCLUDE (see build.sh).
# Turns the Android libmain.so build (patches/BanjoRecomp) into a port-launcher module.
set(PLM_MODULE_DIR ${CMAKE_CURRENT_LIST_DIR})

function(plm_make_module)
    target_sources(BanjoRecompiled PRIVATE ${PLM_MODULE_DIR}/../common/n64recomp/recompfrontend_shim.cpp)
    target_compile_definitions(BanjoRecompiled PRIVATE PLM_LOG_TAG="plm-banjo")
    target_include_directories(BanjoRecompiled PRIVATE ${PLM_MODULE_DIR}/../../core/include)
    set_target_properties(BanjoRecompiled PROPERTIES OUTPUT_NAME plm_banjo-recomp)
    target_link_options(BanjoRecompiled PRIVATE -Wl,-z,max-page-size=16384 -Wl,--wrap=osViSwapBuffer_recomp
        -Wl,--wrap=_ZN11ultramodern14error_handling11message_boxEPKc
        -Wl,--wrap=_ZN8recompui12show_contextENS_9ContextIdENSt6__ndk117basic_string_viewIcNS1_11char_traitsIcEEEE)
endfunction()

cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL plm_make_module)
