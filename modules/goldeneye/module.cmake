# Injected into the GoldenEye port's top-level project() through CMAKE_PROJECT_ge007_INCLUDE (see build.sh).
# Turns the Android libge007.so build (patches/goldeneye-pc-port) into a port-launcher module.
set(PLM_MODULE_DIR ${CMAKE_CURRENT_LIST_DIR})

function(plm_make_module)
    target_sources(ge007 PRIVATE ${PLM_MODULE_DIR}/plm_shim.cpp ${PLM_MODULE_DIR}/ge_convert.cpp ${PLM_MODULE_DIR}/../common/android/execinfo_compat.c)
    target_include_directories(ge007 PRIVATE ${PLM_MODULE_DIR}/../../core/include ${PLM_MODULE_DIR}/../../core/third_party)
    # backtrace() below API 33 (crash handler, watchdog)
    target_compile_options(ge007 PRIVATE "SHELL:-include ${PLM_MODULE_DIR}/../common/android/execinfo_compat.h")
    set_target_properties(ge007 PROPERTIES OUTPUT_NAME plm_goldeneye)
    target_link_options(ge007 PRIVATE -Wl,-z,max-page-size=16384 -Wl,--wrap=SDL_GL_SwapWindow -Wl,--wrap=SDL_OpenAudioDevice
        # The port's own settings at launch, right after its config is loaded (plm_shim.cpp).
        -Wl,--wrap=configLoad)
endfunction()

cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL plm_make_module)
