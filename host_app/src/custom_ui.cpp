#include "custom_ui.h"
#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx11.h>
#include <algorithm>
#include <cmath>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

CustomUI::CustomUI() {
    add_sample_effects();
}

CustomUI::~CustomUI() {
    shutdown();
}

bool CustomUI::initialize(HWND hwnd, ID3D11Device *device, ID3D11DeviceContext *context) {
    if (m_initialized) return true;
    m_hwnd = hwnd;
    m_device = device;
    m_context = context;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    apply_photoshop_gray_theme();

    if (!ImGui_ImplWin32_Init(hwnd)) {
        ImGui::DestroyContext();
        return false;
    }

    if (!ImGui_ImplDX11_Init(device, context)) {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return false;
    }

    m_initialized = true;
    return true;
}

void CustomUI::shutdown() {
    if (!m_initialized) return;
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    m_initialized = false;
    m_hwnd = nullptr;
    m_device = nullptr;
    m_context = nullptr;
}

bool CustomUI::input_hook(void *user_data, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto *self = static_cast<CustomUI *>(user_data);
    if (!self || !self->m_initialized) return false;

    if (ImGui_ImplWin32_WndProcHandler(self->m_hwnd, msg, wParam, lParam)) {
        return true;
    }

    const ImGuiIO &io = ImGui::GetIO();
    if (io.WantCaptureMouse) {
        if ((msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL) {
            return true;
        }
    }

    if (io.WantTextInput || io.WantCaptureKeyboard) {
        if ((msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || msg == WM_CHAR) {
            return true;
        }
    }

    return false;
}

bool CustomUI::want_capture_mouse() const {
    if (!m_initialized) return false;
    return ImGui::GetIO().WantCaptureMouse;
}

bool CustomUI::want_capture_keyboard() const {
    if (!m_initialized) return false;
    return ImGui::GetIO().WantCaptureKeyboard;
}

float CustomUI::get_left_panel_width(uint32_t screen_w) const {
    float total_w = static_cast<float>(screen_w);
    if (total_w < 800.0f) return total_w * 0.30f;
    return 280.0f;
}

float CustomUI::get_right_panel_width(uint32_t screen_w) const {
    float total_w = static_cast<float>(screen_w);
    if (total_w < 800.0f) return total_w * 0.35f;
    return 320.0f;
}

float CustomUI::get_menu_bar_height() const {
    if (!m_initialized) return 24.0f;
    return ImGui::GetFrameHeight();
}

void CustomUI::apply_photoshop_gray_theme() {
    ImGuiStyle &style = ImGui::GetStyle();

    style.WindowRounding = 0.0f;
    style.ChildRounding = 2.0f;
    style.FrameRounding = 2.0f;
    style.PopupRounding = 2.0f;
    style.ScrollbarRounding = 2.0f;
    style.GrabRounding = 2.0f;
    style.TabRounding = 2.0f;

    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.TabBorderSize = 1.0f;

    style.WindowPadding = ImVec2(8.0f, 8.0f);
    style.FramePadding = ImVec2(6.0f, 4.0f);
    style.ItemSpacing = ImVec2(8.0f, 6.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.ScrollbarSize = 12.0f;

    ImVec4 *colors = style.Colors;
    colors[ImGuiCol_Text]                  = ImVec4(0.88f, 0.88f, 0.90f, 1.00f);
    colors[ImGuiCol_TextDisabled]          = ImVec4(0.50f, 0.50f, 0.52f, 1.00f);
    colors[ImGuiCol_WindowBg]              = ImVec4(0.18f, 0.18f, 0.19f, 0.98f);
    colors[ImGuiCol_ChildBg]               = ImVec4(0.14f, 0.14f, 0.15f, 1.00f);
    colors[ImGuiCol_PopupBg]               = ImVec4(0.20f, 0.20f, 0.22f, 0.98f);
    colors[ImGuiCol_Border]                = ImVec4(0.26f, 0.26f, 0.28f, 1.00f);
    colors[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg]               = ImVec4(0.12f, 0.12f, 0.13f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.22f, 0.22f, 0.24f, 1.00f);
    colors[ImGuiCol_FrameBgActive]         = ImVec4(0.26f, 0.26f, 0.28f, 1.00f);
    colors[ImGuiCol_TitleBg]               = ImVec4(0.14f, 0.14f, 0.15f, 1.00f);
    colors[ImGuiCol_TitleBgActive]         = ImVec4(0.16f, 0.16f, 0.17f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.12f, 0.12f, 0.13f, 1.00f);
    colors[ImGuiCol_MenuBarBg]             = ImVec4(0.14f, 0.14f, 0.15f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.14f, 0.14f, 0.15f, 0.60f);
    colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.30f, 0.30f, 0.32f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.38f, 0.38f, 0.40f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.44f, 0.44f, 0.48f, 1.00f);
    colors[ImGuiCol_CheckMark]             = ImVec4(0.35f, 0.65f, 0.95f, 1.00f);
    colors[ImGuiCol_SliderGrab]            = ImVec4(0.35f, 0.55f, 0.85f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]      = ImVec4(0.45f, 0.68f, 0.98f, 1.00f);
    colors[ImGuiCol_Button]                = ImVec4(0.24f, 0.24f, 0.26f, 1.00f);
    colors[ImGuiCol_ButtonHovered]         = ImVec4(0.30f, 0.30f, 0.33f, 1.00f);
    colors[ImGuiCol_ButtonActive]          = ImVec4(0.36f, 0.36f, 0.40f, 1.00f);
    colors[ImGuiCol_Header]                = ImVec4(0.18f, 0.40f, 0.70f, 1.00f);
    colors[ImGuiCol_HeaderHovered]         = ImVec4(0.24f, 0.48f, 0.80f, 1.00f);
    colors[ImGuiCol_HeaderActive]          = ImVec4(0.28f, 0.54f, 0.88f, 1.00f);
    colors[ImGuiCol_Separator]             = ImVec4(0.26f, 0.26f, 0.28f, 1.00f);
    colors[ImGuiCol_SeparatorHovered]      = ImVec4(0.36f, 0.36f, 0.40f, 1.00f);
    colors[ImGuiCol_SeparatorActive]       = ImVec4(0.46f, 0.46f, 0.52f, 1.00f);
    colors[ImGuiCol_Tab]                   = ImVec4(0.18f, 0.18f, 0.19f, 1.00f);
    colors[ImGuiCol_TabHovered]            = ImVec4(0.28f, 0.28f, 0.30f, 1.00f);
    colors[ImGuiCol_TabActive]             = ImVec4(0.23f, 0.23f, 0.25f, 1.00f);
}

void CustomUI::add_sample_effects() {
    ShaderLabEffect fx1;
    fx1.name = "Adaptive Sharpen";
    fx1.file_path = "shaders/AdaptiveSharpen.fx";
    fx1.engine_type = "ReShade FX";
    fx1.enabled = true;
    {
        ShaderLabUniform u;
        u.name = "curve_height";
        u.label = "Sharpening Strength";
        u.tooltip = "Controls main edge sharpening curve height";
        u.category = "Tuning";
        u.type = ShaderLabUniform::Type::Float;
        u.min_val = 0.0f;
        u.max_val = 2.5f;
        u.step = 0.05f;
        u.float_val = 1.0f;
        u.default_float = 1.0f;
        fx1.uniforms.push_back(u);
    }
    {
        ShaderLabUniform u;
        u.name = "video_level_out";
        u.label = "Video Levels Output";
        u.tooltip = "Compress output to standard video ranges";
        u.category = "Output";
        u.type = ShaderLabUniform::Type::Bool;
        u.bool_val = false;
        u.default_bool = false;
        fx1.uniforms.push_back(u);
    }
    m_effects.push_back(fx1);

    ShaderLabEffect fx2;
    fx2.name = "Vignette & Chromatic";
    fx2.file_path = "shaders/UnityVignette.shader";
    fx2.engine_type = "Unity Shader";
    fx2.enabled = false;
    {
        ShaderLabUniform u;
        u.name = "_VignetteIntensity";
        u.label = "Vignette Falloff";
        u.tooltip = "Outer border darkening intensity";
        u.category = "Lens Optics";
        u.type = ShaderLabUniform::Type::Float;
        u.min_val = 0.0f;
        u.max_val = 1.0f;
        u.step = 0.02f;
        u.float_val = 0.5f;
        u.default_float = 0.5f;
        fx2.uniforms.push_back(u);
    }
    {
        ShaderLabUniform u;
        u.name = "_VignetteColor";
        u.label = "Tint Color";
        u.tooltip = "Vignette gradient multiplier color";
        u.category = "Lens Optics";
        u.type = ShaderLabUniform::Type::Color4;
        u.color_val[0] = 0.05f;
        u.color_val[1] = 0.05f;
        u.color_val[2] = 0.08f;
        u.color_val[3] = 1.0f;
        u.default_color[0] = 0.05f;
        u.default_color[1] = 0.05f;
        u.default_color[2] = 0.08f;
        u.default_color[3] = 1.0f;
        fx2.uniforms.push_back(u);
    }
    m_effects.push_back(fx2);

    ShaderLabEffect fx3;
    fx3.name = "Film Grain & Color Grade";
    fx3.file_path = "shaders/GodotFilmGrain.gdshader";
    fx3.engine_type = "Godot GDShader";
    fx3.enabled = false;
    {
        ShaderLabUniform u;
        u.name = "grain_amount";
        u.label = "Film Grain Size";
        u.tooltip = "Simulated noise amount";
        u.category = "Film Stock";
        u.type = ShaderLabUniform::Type::Float;
        u.min_val = 0.0f;
        u.max_val = 0.5f;
        u.step = 0.01f;
        u.float_val = 0.12f;
        u.default_float = 0.12f;
        fx3.uniforms.push_back(u);
    }
    {
        ShaderLabUniform u;
        u.name = "color_temperature";
        u.label = "Color Profile";
        u.tooltip = "Color look";
        u.category = "Color";
        u.type = ShaderLabUniform::Type::Enum;
        u.enum_items = { "Neutral", "Warm", "Cool", "Monochrome" };
        u.enum_selected = 0;
        u.default_enum = 0;
        fx3.uniforms.push_back(u);
    }
    m_effects.push_back(fx3);
}

void CustomUI::begin_frame() {
    if (!m_initialized) return;
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void CustomUI::render_menu_bar() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open Image...", "Ctrl+O")) {}
            if (ImGui::MenuItem("Save Snapshot...", "Ctrl+S")) {}
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4")) {
                PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Reset View", "Home")) {}
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Shaders")) {
            if (ImGui::MenuItem("Add ReShade Shader (.fx)...")) {}
            if (ImGui::MenuItem("Import Unity Shader (.shader)...")) {}
            if (ImGui::MenuItem("Import Godot Shader (.gdshader)...")) {}
            if (ImGui::MenuItem("Import Raw HLSL (.hlsl)...")) {}
            ImGui::Separator();
            if (ImGui::MenuItem("Reload All Shaders", "F5")) {}
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Fit Canvas to Screen", "Ctrl+0")) {}
            if (ImGui::MenuItem("Actual Size (100%)", "Ctrl+1")) {}
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("About ShaderLab")) {
                m_show_about_dialog = true;
            }
            ImGui::EndMenu();
        }

        ImGui::EndMainMenuBar();
    }
}

void CustomUI::render_left_panel(float left_width, float top_offset, float panel_height) {
    ImGui::SetNextWindowPos(ImVec2(0.0f, top_offset));
    ImGui::SetNextWindowSize(ImVec2(left_width, panel_height));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar;

    ImGui::Begin("##LeftShadersPanel", nullptr, flags);

    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted("SHADERS & EFFECTS");
    ImGui::PopTextWrapPos();
    ImGui::Separator();

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##Filter", "Filter shaders...", m_search_buffer, sizeof(m_search_buffer));
    ImGui::Spacing();

    ImGui::BeginChild("##EffectListRegion", ImVec2(-1.0f, -64.0f), true);

    std::string search_query = m_search_buffer;
    std::transform(search_query.begin(), search_query.end(), search_query.begin(), [](unsigned char c) { return static_cast<char>(::tolower(c)); });

    if (ImGui::BeginTable("##EffectTable", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("##check", ImGuiTableColumnFlags_WidthFixed, 22.0f);
        ImGui::TableSetupColumn("##item", ImGuiTableColumnFlags_WidthStretch);

        for (size_t i = 0; i < m_effects.size(); ++i) {
            auto &fx = m_effects[i];

            if (!search_query.empty()) {
                std::string lower_name = fx.name;
                std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), [](unsigned char c) { return static_cast<char>(::tolower(c)); });
                if (lower_name.find(search_query) == std::string::npos) continue;
            }

            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(static_cast<int>(i * 2));
            ImGui::Checkbox("##enabled", &fx.enabled);
            ImGui::PopID();

            ImGui::TableSetColumnIndex(1);
            bool is_selected = (m_selected_effect_idx == static_cast<int>(i));
            std::string label = fx.name + " [" + fx.engine_type + "]";

            if (is_selected) {
                ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.18f, 0.40f, 0.70f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.24f, 0.48f, 0.80f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.00f));
            } else {
                ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.0f, 0.0f, 0.0f, 0.00f));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.24f, 0.24f, 0.26f, 0.50f));
                ImGui::PushStyleColor(ImGuiCol_Text, fx.enabled ? ImVec4(0.88f, 0.88f, 0.90f, 1.00f) : ImVec4(0.55f, 0.55f, 0.58f, 1.00f));
            }

            ImGui::PushID(static_cast<int>(i * 2 + 1));
            if (ImGui::Selectable(label.c_str(), is_selected)) {
                m_selected_effect_idx = static_cast<int>(i);
            }
            ImGui::PopID();

            ImGui::PopStyleColor(3);
        }

        ImGui::EndTable();
    }
    ImGui::EndChild();

    if (ImGui::Button("+ Add Shader", ImVec2(-1.0f, 24.0f))) {}

    float btn_w = (ImGui::GetContentRegionAvail().x - 6.0f) * 0.5f;
    if (ImGui::Button("Move Up", ImVec2(btn_w, 24.0f))) {
        if (m_selected_effect_idx > 0 && m_selected_effect_idx < static_cast<int>(m_effects.size())) {
            std::swap(m_effects[m_selected_effect_idx], m_effects[m_selected_effect_idx - 1]);
            m_selected_effect_idx--;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Move Down", ImVec2(btn_w, 24.0f))) {
        if (m_selected_effect_idx >= 0 && m_selected_effect_idx + 1 < static_cast<int>(m_effects.size())) {
            std::swap(m_effects[m_selected_effect_idx], m_effects[m_selected_effect_idx + 1]);
            m_selected_effect_idx++;
        }
    }

    ImGui::End();
}

void CustomUI::render_center_canvas(float left_offset, float top_offset, float canvas_width, float canvas_height,
                                    uint32_t img_w, uint32_t img_h,
                                    float zoom_level, float pan_x, float pan_y) {
    ImGui::SetNextWindowPos(ImVec2(left_offset, top_offset));
    ImGui::SetNextWindowSize(ImVec2(canvas_width, canvas_height));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                            ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus |
                            ImGuiWindowFlags_NoMouseInputs;

    ImGui::Begin("##CenterCanvasPanel", nullptr, flags);

    ImGui::PushTextWrapPos(0.0f);
    if (img_w > 0 && img_h > 0) {
        ImGui::Text("Canvas: %ux%u | Zoom: %.1f%% | Pan: (%.0f, %.0f)",
                    img_w, img_h, zoom_level * 100.0f, pan_x, pan_y);
    } else {
        ImGui::TextUnformatted("Drop an image or project file to begin editing");
    }
    ImGui::PopTextWrapPos();

    ImGui::End();
}

void CustomUI::render_right_panel(float right_offset, float top_offset, float panel_width, float panel_height) {
    ImGui::SetNextWindowPos(ImVec2(right_offset, top_offset));
    ImGui::SetNextWindowSize(ImVec2(panel_width, panel_height));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar;

    ImGui::Begin("##RightSettingsPanel", nullptr, flags);

    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted("PARAMETERS & SETTINGS");
    ImGui::PopTextWrapPos();
    ImGui::Separator();

    if (m_selected_effect_idx >= 0 && m_selected_effect_idx < static_cast<int>(m_effects.size())) {
        auto &fx = m_effects[m_selected_effect_idx];

        ImGui::PushTextWrapPos(0.0f);
        ImGui::Text("Effect: %s", fx.name.c_str());
        ImGui::TextDisabled("Source: %s (%s)", fx.file_path.c_str(), fx.engine_type.c_str());
        ImGui::PopTextWrapPos();

        ImGui::Checkbox("Enable Effect", &fx.enabled);
        ImGui::Separator();

        ImGui::BeginChild("##UniformListRegion", ImVec2(-1.0f, -38.0f), false);

        std::string current_category = "";
        for (auto &u : fx.uniforms) {
            if (!u.category.empty() && u.category != current_category) {
                current_category = u.category;
                ImGui::Spacing();
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "[ %s ]", current_category.c_str());
                ImGui::PopTextWrapPos();
                ImGui::Separator();
            }

            ImGui::PushID(u.name.c_str());

            ImGui::PushTextWrapPos(0.0f);
            ImGui::Text("%s", u.label.c_str());
            ImGui::PopTextWrapPos();
            if (ImGui::IsItemHovered() && !u.tooltip.empty()) {
                ImGui::SetTooltip("%s", u.tooltip.c_str());
            }

            switch (u.type) {
            case ShaderLabUniform::Type::Float: {
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::SliderFloat("##val", &u.float_val, u.min_val, u.max_val, "%.3f");
                break;
            }
            case ShaderLabUniform::Type::Int: {
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::SliderInt("##val", &u.int_val, static_cast<int>(u.min_val), static_cast<int>(u.max_val));
                break;
            }
            case ShaderLabUniform::Type::Bool: {
                ImGui::Checkbox("##val", &u.bool_val);
                break;
            }
            case ShaderLabUniform::Type::Color3: {
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::ColorEdit3("##val", u.color_val);
                break;
            }
            case ShaderLabUniform::Type::Color4: {
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::ColorEdit4("##val", u.color_val);
                break;
            }
            case ShaderLabUniform::Type::Enum: {
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::BeginCombo("##val", u.enum_selected < static_cast<int>(u.enum_items.size()) ?
                                                u.enum_items[u.enum_selected].c_str() : "")) {
                    for (int opt = 0; opt < static_cast<int>(u.enum_items.size()); ++opt) {
                        bool sel = (u.enum_selected == opt);
                        if (ImGui::Selectable(u.enum_items[opt].c_str(), sel)) {
                            u.enum_selected = opt;
                        }
                    }
                    ImGui::EndCombo();
                }
                break;
            }
            }

            ImGui::PopID();
            ImGui::Spacing();
        }

        ImGui::EndChild();

        if (ImGui::Button("Reset Parameters to Default", ImVec2(-1.0f, 26.0f))) {
            fx.reset_uniforms();
        }

    } else {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Select an effect on the left to configure its parameters.");
        ImGui::PopTextWrapPos();
    }

    ImGui::End();
}

void CustomUI::render(uint32_t screen_w, uint32_t screen_h,
                      uint32_t img_w, uint32_t img_h,
                      float zoom_level, float pan_x, float pan_y) {
    if (!m_initialized) return;

    render_menu_bar();

    float menu_height = get_menu_bar_height();
    float body_height = static_cast<float>(screen_h) - menu_height;
    if (body_height < 100.0f) body_height = 100.0f;

    float left_w = get_left_panel_width(screen_w);
    float right_w = get_right_panel_width(screen_w);
    float total_w = static_cast<float>(screen_w);

    float center_x = left_w;
    float center_w = total_w - left_w - right_w;
    if (center_w < 100.0f) center_w = 100.0f;

    float right_x = total_w - right_w;

    render_left_panel(left_w, menu_height, body_height);
    render_center_canvas(center_x, menu_height, center_w, body_height, img_w, img_h, zoom_level, pan_x, pan_y);
    render_right_panel(right_x, menu_height, right_w, body_height);

    if (m_show_about_dialog) {
        ImGui::OpenPopup("About ShaderLab");
        m_show_about_dialog = false;
    }

    if (ImGui::BeginPopupModal("About ShaderLab", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(260.0f);
        ImGui::TextUnformatted("ShaderLab");
        ImGui::Separator();
        ImGui::TextUnformatted("Version 1.3.0");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(-1.0f, 26.0f))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void CustomUI::end_frame() {
    if (!m_initialized) return;
    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}
