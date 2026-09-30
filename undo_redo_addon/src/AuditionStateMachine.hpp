#pragma once
#include <string>
#include <cstdint>
#include <windows.h>
#include "Commands.hpp"
#include "ContextManager.hpp"

class AuditionStateMachine {
public:
    static AuditionStateMachine &instance() { static AuditionStateMachine s; return s; }

    void process_technique_toggle(const std::string &effect, const std::string &technique, bool new_state) {
        uint64_t now = GetTickCount64();

        if (active_) {
            if (effect_ == effect && technique_ == technique) {
                current_ = new_state;
                last_ms_ = now;
                return;
            }
            flush_session();
        }

        active_ = true;
        effect_ = effect;
        technique_ = technique;
        initial_ = !new_state;
        current_ = new_state;
        last_ms_ = now;
    }

    void update_frame() {
        if (active_ && GetTickCount64() - last_ms_ >= kTimeoutMs)
            flush_session();
    }

    void flush_session() {
        if (!active_) return;
        if (initial_ != current_) {
            ContextManager::instance().push_command(
                std::make_unique<TechniqueToggleCommand>(effect_, technique_, initial_, current_));
        }
        active_ = false;
    }

    void reset() {
        active_ = false;
        effect_.clear();
        technique_.clear();
        initial_ = current_ = false;
        last_ms_ = 0;
    }

    bool is_active() const noexcept { return active_; }
    std::string target_label() const { return active_ ? technique_ : ""; }

    float time_remaining_sec() const {
        if (!active_) return 0.0f;
        uint64_t elapsed = GetTickCount64() - last_ms_;
        return elapsed >= kTimeoutMs ? 0.0f : static_cast<float>(kTimeoutMs - elapsed) / 1000.0f;
    }

private:
    AuditionStateMachine() = default;

    static constexpr uint64_t kTimeoutMs = 1500;
    bool active_ = false;
    std::string effect_, technique_;
    bool initial_ = false, current_ = false;
    uint64_t last_ms_ = 0;
};
