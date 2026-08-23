cmake_minimum_required(VERSION 3.20)
project(host_app LANGUAGES CXX)

set(HOST_SOURCES
    src/main.cpp
    src/gfx_device.h
    src/gfx_device.cpp
    src/image_loader.h
    src/image_loader.cpp
    src/blit_renderer.h
    src/blit_renderer.cpp
    src/backbuffer_dump.h
    src/backbuffer_dump.cpp
    src/warning_overlay.h
    src/warning_overlay.cpp
    src/shaders/blit.hlsl
    resource.rc
)

set_source_files_properties(src/shaders/blit.hlsl PROPERTIES HEADER_FILE_ONLY TRUE)

add_executable(host_app WIN32 ${HOST_SOURCES})

target_compile_definitions(host_app PRIVATE
    _CRT_SECURE_NO_WARNINGS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_include_directories(host_app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/stb
)

target_link_libraries(host_app PRIVATE
    d3d11
    dxgi
    d3dcompiler
    user32
    shell32
    gdiplus
    winmm
)

set_target_properties(host_app PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME "ShaderLab"
)
