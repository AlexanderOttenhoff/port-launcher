# Injected into JFG's top-level project() through CMAKE_PROJECT_JFGRecomp_INCLUDE (see build.sh). It turns JFG's
# Android libmain.so build into a port-launcher module without patching the JFG source tree.
set(PLM_MODULE_DIR ${CMAKE_CURRENT_LIST_DIR})

function(plm_make_module)
    target_sources(JetForceGeminiRecompiled PRIVATE ${PLM_MODULE_DIR}/plm_shim.cpp)
    target_include_directories(JetForceGeminiRecompiled PRIVATE ${PLM_MODULE_DIR}/../../core/include)
    # The game's first osViSwapBuffer tells the host to drop its loading screen (plm_shim.cpp).
    target_link_options(JetForceGeminiRecompiled PRIVATE -Wl,--wrap=osViSwapBuffer_recomp)
    set_target_properties(JetForceGeminiRecompiled PROPERTIES OUTPUT_NAME plm_jfg-recomp)
    # 16 KB page alignment for 16K-page devices and emulator images.
    target_link_options(JetForceGeminiRecompiled PRIVATE -Wl,-z,max-page-size=16384)
endfunction()

# The game target is defined later in JFG's CMakeLists, so finish the module at the end of that directory.
cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL plm_make_module)
