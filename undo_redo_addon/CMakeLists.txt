cmake_minimum_required(VERSION 3.20)
project(ReShadeUndoRedo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /EHsc /Zc:preprocessor)
endif()

# allow pointing to external reshade sdk if built outside shaderlab
if(NOT DEFINED RESHADE_SDK_DIR OR RESHADE_SDK_DIR STREQUAL "")
    if(DEFINED CMAKE_SOURCE_DIR AND EXISTS "${CMAKE_SOURCE_DIR}/third_party/reshade/include/reshade.hpp")
        set(RESHADE_SDK_DIR "${CMAKE_SOURCE_DIR}/third_party/reshade" CACHE PATH "Path to ReShade SDK")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/reshade/include/reshade.hpp")
        set(RESHADE_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/reshade" CACHE PATH "Path to ReShade SDK")
    else()
        set(RESHADE_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/reshade" CACHE PATH "Path to ReShade SDK")
    endif()
endif()

if(EXISTS "${RESHADE_SDK_DIR}/include/reshade.hpp")
    set(RESHADE_INCLUDE_DIR "${RESHADE_SDK_DIR}/include")
elseif(EXISTS "${RESHADE_SDK_DIR}/reshade.hpp")
    set(RESHADE_INCLUDE_DIR "${RESHADE_SDK_DIR}")
else()
    message(FATAL_ERROR "Could not find reshade.hpp. Set RESHADE_SDK_DIR to your ReShade SDK path.")
endif()

set(ADDON_SOURCES
    include/ReShadeUndoRedo_API.h
    src/DynamicPayload.hpp
    src/ICommand.hpp
    src/Commands.hpp
    src/CommandQueue.hpp
    src/ContextManager.hpp
    src/AuditionStateMachine.hpp
    src/TransactionCoalescer.hpp
    src/AddonRegistry.hpp
    src/main.cpp
    resource.rc
)

add_library(UndoRedo SHARED ${ADDON_SOURCES})

target_compile_definitions(UndoRedo PRIVATE
    _CRT_SECURE_NO_WARNINGS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNDOREDO_EXPORTS
)

target_include_directories(UndoRedo PRIVATE
    ${RESHADE_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(UndoRedo PRIVATE
    user32
    shell32
)

set_target_properties(UndoRedo PROPERTIES
    OUTPUT_NAME "UndoRedo"
    SUFFIX ".addon"
    PREFIX ""
    VERSION "1.1.0"
    SOVERSION "1.1.0"
)

add_executable(UndoRedoTests
    tests/test_main.cpp
)

target_compile_definitions(UndoRedoTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNDOREDO_STATIC
)

target_include_directories(UndoRedoTests PRIVATE
    ${RESHADE_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(UndoRedoTests PRIVATE
    user32
    shell32
)

