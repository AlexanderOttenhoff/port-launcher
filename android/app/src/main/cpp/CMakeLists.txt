cmake_minimum_required(VERSION 3.22)
project(plm_launcher LANGUAGES CXX)

# JNI bridge between the Android shell and the portable core. Android specifics stay in this layer.

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../../../core ${CMAKE_BINARY_DIR}/core)

add_library(plm_launcher SHARED jni_bridge.cpp)
target_link_libraries(plm_launcher PRIVATE plm_core log)
set_target_properties(plm_launcher PROPERTIES CXX_VISIBILITY_PRESET hidden)
# 16 KB page alignment: required by 16K-page devices and images (NDK 26 defaults to 4 KB).
target_link_options(plm_launcher PRIVATE "-Wl,-z,max-page-size=16384")
