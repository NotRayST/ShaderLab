#pragma once
#include <vector>
#include <memory>
#include <algorithm>
#include <cstddef>
#include "ICommand.hpp"

enum class PushResult { Failed, Pushed, Merged };

struct PushOutcome {
    PushResult result = PushResult::Failed;
    int merged_index = -1;
};

class CommandQueue {
public:
    explicit CommandQueue(size_t limit = kHistoryLimit) : limit_(limit) {}

    PushOutcome push(std::unique_ptr<ICommand> command) {
        if (!command) return { PushResult::Failed, -1 };
        truncate_redo();

        if (command->merge_id() != -1) {
            size_t check_count = (std::min)(entries_.size(), size_t(4));
            for (size_t i = 0; i < check_count; ++i) {
                if (entries_[i]->merge_id() == command->merge_id() &&
                    entries_[i]->merge_with(command.get()))
                {
                    return { PushResult::Merged, static_cast<int>(i) };
                }
            }
        }

        if (entries_.size() >= limit_) entries_.pop_back();
        entries_.insert(entries_.begin(), std::move(command));
        position_ = 0;
        return { PushResult::Pushed, 0 };
    }

    bool undo(reshade::api::effect_runtime *runtime = nullptr) {
        if (position_ >= entries_.size()) return false;
        entries_[position_++]->undo(runtime);
        return true;
    }

    bool redo(reshade::api::effect_runtime *runtime = nullptr) {
        if (position_ == 0) return false;
        entries_[--position_]->execute(runtime);
        return true;
    }

    void jump_to(size_t target, reshade::api::effect_runtime *runtime = nullptr) {
        target = (std::min)(target, entries_.size());
        bool changed = (position_ != target);
        while (position_ < target) { if (!undo(runtime)) break; }
        while (position_ > target) { if (!redo(runtime)) break; }
        if (changed && runtime) runtime->save_current_preset();
    }

    void truncate_redo() {
        if (position_ > 0) {
            entries_.erase(entries_.begin(), entries_.begin() + (std::min)(position_, entries_.size()));
            position_ = 0;
        }
    }

    void clear() { entries_.clear(); position_ = 0; }

    size_t position() const noexcept { return position_; }
    size_t size() const noexcept { return entries_.size(); }
    size_t undo_count() const noexcept { return entries_.size() - position_; }
    size_t redo_count() const noexcept { return position_; }
    bool is_undone(size_t index) const noexcept { return index < position_; }
    const std::vector<std::unique_ptr<ICommand>> &entries() const { return entries_; }

private:
    size_t limit_ = kHistoryLimit;
    size_t position_ = 0;
    std::vector<std::unique_ptr<ICommand>> entries_;
};
