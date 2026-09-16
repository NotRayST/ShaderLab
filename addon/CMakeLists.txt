cmake_minimum_required(VERSION 3.20)
project(capture_addon LANGUAGES CXX)

set(ADDON_SOURCES
    src/dllmain.cpp
    src/overlay_ui.h
    src/overlay_ui.cpp
    src/depth_manager.h
    src/depth_manager.cpp
    src/job_queue.h
    src/job_queue.cpp
    src/capture.h
    src/capture.cpp
    src/hud_composite.h
    src/hud_composite.cpp
    src/before_after.h
    src/before_after.cpp
    src/undo_history.h
    src/undo_history.cpp
    src/keybinds.h
    src/keybinds.cpp
    ../common/project_file.h
    ../common/project_file.cpp
    ../common/depth_chunk.cpp
    ../common/depth_file.cpp
    ../third_party/miniz/miniz.cpp
    resource.rc
)

add_library(capture SHARED ${ADDON_SOURCES})

target_compile_definitions(capture PRIVATE
    _CRT_SECURE_NO_WARNINGS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_include_directories(capture PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/miniz
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/reshade/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/stb
)

target_link_libraries(capture PRIVATE
    d3d11
    d3dcompiler
    user32
    shell32
    comdlg32
    ole32
    wininet
    urlmon
)

set_target_properties(capture PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME "ShaderLab"
    SUFFIX ".addon"
    PREFIX ""
)

# if you pass -DRESHADE_DIR we also drop a fresh dxgi.dll next to the addon for quick testing
if(RESHADE_DIR)
    add_custom_command(TARGET capture POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:capture>
            ${CMAKE_BINARY_DIR}/host_app/$<CONFIG>/capture.addon
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${RESHADE_DIR}/dxgi.dll
            ${CMAKE_BINARY_DIR}/host_app/$<CONFIG>/dxgi.dll
        COMMENT "Deploying capture.addon and dxgi.dll to host_app binary directory"
    )
endif()
