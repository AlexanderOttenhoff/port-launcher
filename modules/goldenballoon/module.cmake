# Injected into Golden Balloon's top-level project() (mdkr64) through CMAKE_PROJECT_mdkr64_INCLUDE (see build.sh).
# Turns the Android shared-library build (patches/goldenballoon) into a port-launcher module.
set(PLM_MODULE_DIR ${CMAKE_CURRENT_LIST_DIR})
# The project is C only; the shim is C++.
enable_language(CXX)

function(plm_make_module)
    target_sources(mdkr64 PRIVATE ${PLM_MODULE_DIR}/plm_shim.cpp)
    # PLM_MODULE gates our game-side changes in the patches (cutscene and intro skips, patch 0004), so the
    # upstream desktop build of the same tree stays retail.
    target_compile_definitions(mdkr64 PRIVATE PLM_MODULE=1)
    target_include_directories(mdkr64 PRIVATE ${PLM_MODULE_DIR}/../../core/include ${PLM_MODULE_DIR}/../../core/third_party)
    set_target_properties(mdkr64 PROPERTIES OUTPUT_NAME plm_goldenballoon)
    target_link_options(mdkr64 PRIVATE -Wl,-z,max-page-size=16384 -Wl,--wrap=SDL_GL_SwapWindow -Wl,--wrap=SDL_OpenAudioDevice)
endfunction()

cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL plm_make_module)
