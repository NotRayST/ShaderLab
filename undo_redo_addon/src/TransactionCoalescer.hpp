#pragma once
#define ImTextureID ImU64
#include <imgui.h>
#include <string>
#include <map>
#include <vector>
#include <cstdint>
#include <windows.h>
#include "DynamicPayload.hpp"
#include "Commands.hpp"
#include "ContextManager.hpp"

inline bool is_user_interacting() {
    if (ImGui::GetCurrentContext()) {
        const ImGuiIO &io = ImGui::GetIO();
        if (io.MouseDown[0] || io.MouseDown[1] || io.MouseDown[2]) return true;
        if (ImGui::IsAnyItemActive()) return true;
    }
    if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0) return true;
    if ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0) return true;
    return false;
}

class TransactionCoalescer {
public:
    static TransactionCoalescer &instance() { static TransactionCoalescer s; return s; }

    void stage_uniform(const std::string &effect, const std::string &variable,
                       DynamicPayload before, DynamicPayload after)
    {
        std::string key = effect + "::" + variable;
        if (!in_txn_) { in_txn_ = true; staged_.clear(); }

        auto it = staged_.find(key);
        if (it == staged_.end())
            staged_[key] = StagedMutation{ effect, variable, std::move(before), std::move(after) };
        else
            it->second.latest_state = std::move(after);
    }

    void update_frame() {
        if (!in_txn_) return;
        if (!is_user_interacting())
            commit_transaction();
    }

    void reset() {
        staged_.clear();
        in_txn_ = false;
    }

    void commit_transaction() {
        if (!in_txn_) return;

        std::vector<StagedMutation> deltas;
        deltas.reserve(staged_.size());
        for (auto &[k, mut] : staged_)
            if (mut.initial_state != mut.latest_state)
                deltas.push_back(std::move(mut));

        if (!deltas.empty()) {
            if (deltas.size() == 1)
                ContextManager::instance().push_command(std::make_unique<UniformCommand>(
                    deltas[0].effect_name, deltas[0].variable_name,
                    std::move(deltas[0].initial_state), std::move(deltas[0].latest_state)));
            else
                ContextManager::instance().push_command(
                    std::make_unique<CompoundUniformCommand>(std::move(deltas)));
        }

        staged_.clear();
        in_txn_ = false;
    }

    bool is_in_transaction() const noexcept { return in_txn_; }

private:
    TransactionCoalescer() = default;

    bool in_txn_ = false;
    std::map<std::string, StagedMutation> staged_;
};
