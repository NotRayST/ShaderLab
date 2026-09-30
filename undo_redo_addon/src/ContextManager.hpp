#pragma once
#include <vector>
#include <memory>
#include <mutex>
#include <string>
#include <cstdint>
#include <algorithm>
#include "ICommand.hpp"
#include "CommandQueue.hpp"

struct Suppress {
    uint32_t &n;
    explicit Suppress(uint32_t &c) : n(c) { ++n; }
    ~Suppress() { --n; }
};

class ContextManager {
public:
    struct GlobalRecord {
        CommandDomain domain = CommandDomain::ShaderState;
        std::string label;
        uint64_t timestamp_ms = 0;
    };

    static ContextManager &instance() { static ContextManager s; return s; }

    CommandQueue &get_domain(CommandDomain d) {
        std::lock_guard<std::recursive_mutex> lk(mutex_);
        return domains_[static_cast<size_t>(d)];
    }

    CommandDomain active_domain() const noexcept { return active_domain_; }
    void set_active_domain(CommandDomain d) noexcept { active_domain_ = d; }

    bool push_command(std::unique_ptr<ICommand> command) {
        if (!command) return false;
        std::lock_guard<std::recursive_mutex> lk(mutex_);

        if (global_position_ > 0) {
            for (auto &q : domains_) q.truncate_redo();
            global_timeline_.erase(global_timeline_.begin(),
                                   global_timeline_.begin() + (std::min)(global_position_, global_timeline_.size()));
            global_position_ = 0;
        }

        CommandDomain d = command->domain();
        std::string label = command->get_label();
        PushOutcome res = domains_[static_cast<size_t>(d)].push(std::move(command));

        if (res.result == PushResult::Pushed) {
            if (global_timeline_.size() >= kHistoryLimit) global_timeline_.pop_back();
            global_timeline_.insert(global_timeline_.begin(), GlobalRecord{ d, std::move(label), GetTickCount64() });
            global_position_ = 0;
            return true;
        }
        if (res.result == PushResult::Merged) {
            int domain_seen = 0;
            for (auto &rec : global_timeline_) {
                if (rec.domain == d) {
                    if (domain_seen == res.merged_index) {
                        rec.label = std::move(label);
                        rec.timestamp_ms = GetTickCount64();
                        break;
                    }
                    ++domain_seen;
                }
            }
            return true;
        }
        return false;
    }

    bool undo_active(reshade::api::effect_runtime *rt = nullptr) {
        std::lock_guard<std::recursive_mutex> lk(mutex_);
        Suppress s(reentrancy_);
        bool r = domains_[static_cast<size_t>(active_domain_)].undo(rt);
        sync_global_timeline();
        return r;
    }

    bool redo_active(reshade::api::effect_runtime *rt = nullptr) {
        std::lock_guard<std::recursive_mutex> lk(mutex_);
        Suppress s(reentrancy_);
        bool r = domains_[static_cast<size_t>(active_domain_)].redo(rt);
        sync_global_timeline();
        return r;
    }

    bool undo_global(reshade::api::effect_runtime *rt = nullptr) {
        std::lock_guard<std::recursive_mutex> lk(mutex_);
        if (global_position_ >= global_timeline_.size()) return false;
        Suppress s(reentrancy_);
        CommandDomain d = global_timeline_[global_position_].domain;
        bool r = domains_[static_cast<size_t>(d)].undo(rt);
        ++global_position_;
        return r;
    }

    bool redo_global(reshade::api::effect_runtime *rt = nullptr) {
        std::lock_guard<std::recursive_mutex> lk(mutex_);
        if (global_position_ == 0) return false;
        Suppress s(reentrancy_);
        --global_position_;
        CommandDomain d = global_timeline_[global_position_].domain;
        return domains_[static_cast<size_t>(d)].redo(rt);
    }

    void jump_global(size_t target, reshade::api::effect_runtime *rt = nullptr) {
        std::lock_guard<std::recursive_mutex> lk(mutex_);
        Suppress s(reentrancy_);
        target = (std::min)(target, global_timeline_.size());
        bool changed = (global_position_ != target);
        while (global_position_ < target) { if (!undo_global(rt)) break; }
        while (global_position_ > target) { if (!redo_global(rt)) break; }
        if (changed && rt) rt->save_current_preset();
    }

    void jump_domain(CommandDomain domain, size_t target, reshade::api::effect_runtime *rt = nullptr) {
        std::lock_guard<std::recursive_mutex> lk(mutex_);
        size_t d = static_cast<size_t>(domain);
        if (d >= static_cast<size_t>(CommandDomain::Count)) return;
        Suppress s(reentrancy_);
        domains_[d].jump_to(target, rt);
        sync_global_timeline();
    }

    void clear_all() {
        std::lock_guard<std::recursive_mutex> lk(mutex_);
        for (auto &q : domains_) q.clear();
        global_timeline_.clear();
        global_position_ = 0;
    }

    bool is_suppressing() const noexcept { return reentrancy_ > 0; }

    size_t global_undo_count() const noexcept { return global_timeline_.size() - global_position_; }
    size_t global_redo_count() const noexcept { return global_position_; }
    size_t global_size() const noexcept { return global_timeline_.size(); }
    size_t global_position() const noexcept { return global_position_; }
    bool global_undone(size_t i) const noexcept { return i < global_position_; }
    const std::vector<GlobalRecord> &global_timeline() const { return global_timeline_; }

    void sync_global_timeline() {
        std::lock_guard<std::recursive_mutex> lk(mutex_);
        if (global_timeline_.empty()) { global_position_ = 0; return; }

        constexpr size_t num_domains = static_cast<size_t>(CommandDomain::Count);
        bool all_end = true, all_start = true;
        for (size_t d = 0; d < num_domains; ++d) {
            if (domains_[d].position() != domains_[d].size()) all_end = false;
            if (domains_[d].position() != 0) all_start = false;
        }
        if (all_end) { global_position_ = global_timeline_.size(); return; }
        if (all_start) { global_position_ = 0; return; }

        size_t d_seen[num_domains] = {}, matched = 0;
        for (size_t i = 0; i < global_timeline_.size(); ++i) {
            size_t d = static_cast<size_t>(global_timeline_[i].domain);
            if (d < num_domains && d_seen[d]++ < domains_[d].position()) matched = i + 1;
        }
        global_position_ = (std::min)(matched, global_timeline_.size());
    }

private:
    ContextManager() = default;

    std::recursive_mutex mutex_;
    uint32_t reentrancy_ = 0;
    CommandDomain active_domain_ = CommandDomain::ShaderState;
    CommandQueue domains_[static_cast<size_t>(CommandDomain::Count)];
    std::vector<GlobalRecord> global_timeline_;
    size_t global_position_ = 0;
};
