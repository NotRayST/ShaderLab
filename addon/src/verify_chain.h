#pragma once
#define ImTextureID ImU64
#include <reshade.hpp>
#include <string>

class ChainVerifier {
public:
    static ChainVerifier &get() {
        static ChainVerifier instance;
        return instance;
    }
    void update(reshade::api::effect_runtime *) {}
    bool is_running() const { return false; }
    const std::wstring &get_status_text() const { static std::wstring s; return s; }
};
