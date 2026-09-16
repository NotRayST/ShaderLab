cmake_minimum_required(VERSION 3.20)
project(ShaderLabCapture LANGUAGES CXX)

set(CAPTURE_SOURCES
    src/dllmain.cpp
    src/capture_manager.h
    src/capture_manager.cpp
    ../common/depth_chunk.cpp
    ../common/depth_file.cpp
    ../third_party/miniz/miniz.cpp
    resource.rc
)

add_library(ShaderLabCapture SHARED ${CAPTURE_SOURCES})

target_compile_definitions(ShaderLabCapture PRIVATE
    _CRT_SECURE_NO_WARNINGS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_include_directories(ShaderLabCapture PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/miniz
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/reshade/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/stb
)

target_link_libraries(ShaderLabCapture PRIVATE
    user32
    shell32
    winmm
    psapi
)

set_target_properties(ShaderLabCapture PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME "ShaderLabCapture"
    SUFFIX ".addon"
    PREFIX ""
    VERSION "1.2.1"
    SOVERSION "1.2.1"
)
