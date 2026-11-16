# Injected into the Perfect Dark port's top-level project() through CMAKE_PROJECT_pd_INCLUDE (see build.sh).
# Turns the Android libpd.so build (patches/perfect_dark) into a port-launcher module.
set(PLM_MODULE_DIR ${CMAKE_CURRENT_LIST_DIR})

function(plm_make_module)
    target_sources(pd PRIVATE ${PLM_MODULE_DIR}/plm_shim.cpp)
    target_include_directories(pd PRIVATE ${PLM_MODULE_DIR}/../../core/include ${PLM_MODULE_DIR}/../../core/third_party)
    set_target_properties(pd PROPERTIES OUTPUT_NAME plm_perfect-dark)
    target_link_options(pd PRIVATE -Wl,-z,max-page-size=16384 -Wl,--wrap=SDL_GL_SwapWindow -Wl,--wrap=SDL_OpenAudioDevice
        # The port's own settings at launch, right after its config is registered and loaded (plm_shim.cpp).
        -Wl,--wrap=configInit)
endfunction()

cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL plm_make_module)
