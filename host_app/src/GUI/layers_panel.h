#pragma once
#include <windows.h>
#include <d3d11.h>
#include <functional>
#include "layer_system.h"

namespace gui {

class GuiManager;

struct LayerPanelCallbacks {
    HWND owner_hwnd = nullptr;
    std::function<void()> on_layers_changed;
    std::function<void(int layer_index)> on_select_text_layer;
    std::function<void()> on_import_clicked;
};

// Renders the permanent bottom-right Photoshop-style Layers Panel (##LayersPanel).
// Returns true if any layer property, ordering, or stack membership changed this frame.
bool render_layers_panel(
    GuiManager &gui,
    LayerStack &stack,
    LayerCompositor &compositor,
    ID3D11Device *device,
    ID3D11DeviceContext *context,
    const LayerPanelCallbacks *callbacks = nullptr
);

} // namespace gui
