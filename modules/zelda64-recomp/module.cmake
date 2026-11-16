# Injected into Zelda64Recomp's top-level project() through CMAKE_PROJECT_Zelda64Recompiled_INCLUDE (see build.sh).
# Turns the Android libmain.so build (patches/Zelda64Recomp) into a port-launcher module.
set(PLM_MODULE_DIR ${CMAKE_CURRENT_LIST_DIR})

function(plm_make_module)
    target_sources(Zelda64Recompiled PRIVATE ${PLM_MODULE_DIR}/../common/n64recomp/recompfrontend_shim.cpp)
    target_compile_definitions(Zelda64Recompiled PRIVATE PLM_LOG_TAG="plm-zelda64" PLM_C_STICK_MODIFIER=1)
    target_include_directories(Zelda64Recompiled PRIVATE ${PLM_MODULE_DIR}/../../core/include)
    set_target_properties(Zelda64Recompiled PROPERTIES OUTPUT_NAME plm_zelda64-recomp)
    target_link_options(Zelda64Recompiled PRIVATE -Wl,-z,max-page-size=16384 -Wl,--wrap=osViSwapBuffer_recomp
        -Wl,--wrap=_ZN11ultramodern14error_handling11message_boxEPKc
        -Wl,--wrap=_ZN8recompui12show_contextENS_9ContextIdENSt6__ndk117basic_string_viewIcNS1_11char_traitsIcEEEE
        -Wl,--wrap=_ZN11recompinput27set_right_analog_suppressedEb
        -Wl,--wrap=_ZN11recompinput16get_right_analogEiPfS0_)
endfunction()

cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL plm_make_module)
