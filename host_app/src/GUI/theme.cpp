#include "theme.h"

namespace gui {

void apply_photoshop_gray_theme() {
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
    colors[ImGuiCol_WindowBg]              = ImVec4(0.18f, 0.18f, 0.19f, 1.00f);
    colors[ImGuiCol_ChildBg]               = ImVec4(0.14f, 0.14f, 0.15f, 1.00f);
    colors[ImGuiCol_PopupBg]               = ImVec4(0.20f, 0.20f, 0.22f, 1.00f);
    colors[ImGuiCol_Border]                = ImVec4(0.26f, 0.26f, 0.28f, 1.00f);
    colors[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg]               = ImVec4(0.16f, 0.16f, 0.18f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.24f, 0.24f, 0.27f, 1.00f);
    colors[ImGuiCol_FrameBgActive]         = ImVec4(0.28f, 0.28f, 0.32f, 1.00f);
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

void add_sample_effects(std::vector<ShaderLabEffect> &effects) {
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
    effects.push_back(fx1);

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
    effects.push_back(fx2);

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
    effects.push_back(fx3);
}

} // namespace gui
