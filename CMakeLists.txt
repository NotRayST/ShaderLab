cmake_minimum_required(VERSION 3.20)
project(ShaderLabV LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BUILD_HOST "Build host application" ON)
option(BUILD_ADDON "Build ReShade addon" ON)
set(RESHADE_DIR "" CACHE PATH "Path to directory containing full-addon-support dxgi.dll")

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /EHsc /Zc:preprocessor)
endif()

if(BUILD_HOST)
    add_subdirectory(host_app)
endif()

if(BUILD_ADDON)
    add_subdirectory(addon)
endif()
