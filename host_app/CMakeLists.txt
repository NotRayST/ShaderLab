cmake_minimum_required(VERSION 3.20)
project(host_app LANGUAGES CXX)

set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/reshade-src/deps/imgui")

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
    src/settle_detector.h
    src/settle_detector.cpp
    src/hud_renderer.h
    src/hud_renderer.cpp
    src/viewport_controller.h
    src/viewport_controller.cpp
    src/warning_overlay.h
    src/warning_overlay.cpp
    src/GUI/types.h
    src/GUI/tool_requirements.h
    src/GUI/tool_requirements.cpp
    src/GUI/theme.h
    src/GUI/theme.cpp
    src/GUI/menu_bar.h
    src/GUI/menu_bar.cpp
    src/GUI/tool_strip.h
    src/GUI/tool_strip.cpp
    src/GUI/tab_bar.h
    src/GUI/tab_bar.cpp
    src/GUI/left_panel.h
    src/GUI/left_panel.cpp
    src/GUI/right_panel.h
    src/GUI/right_panel.cpp
    src/GUI/canvas.h
    src/GUI/canvas.cpp
    src/GUI/status_bar.h
    src/GUI/status_bar.cpp
    src/GUI/gui_manager.h
    src/GUI/gui_manager.cpp
    src/effect_pipeline.h
    src/effect_pipeline.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
    src/cli/cli_parser.h
    src/cli/cli_parser.cpp
    src/cli/cli_info.h
    src/cli/cli_info.cpp
    src/cli/cli_preset.h
    src/cli/cli_preset.cpp
    src/cli/cli_depth.h
    src/cli/cli_depth.cpp
    src/cli/cli_test_shader.h
    src/cli/cli_test_shader.cpp
    src/cli/cli_render.h
    src/cli/cli_render.cpp
    src/cli/cli_dispatcher.h
    src/cli/cli_dispatcher.cpp
    src/cli/cli_ctl.h
    src/cli/cli_ctl.cpp
    src/cli/cli_reshade_utils.h
    ../common/project_file.h
    ../common/project_file.cpp
    ../common/depth_file.cpp
    ../common/depth_chunk.cpp
    ../third_party/miniz/miniz.cpp
    resource.rc
)

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
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/miniz
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

target_link_libraries(host_app PRIVATE
    d3d11
    dxgi
    d3dcompiler
    user32
    shell32
    gdiplus
    winmm
    comdlg32
    urlmon
    wininet
)

set_target_properties(host_app PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME "ShaderLab"
)
