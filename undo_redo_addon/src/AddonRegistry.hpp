#pragma once
#include <string>
#include <unordered_map>
#include <mutex>
#include <vector>
#include <cstdint>
#include <windows.h>
#include "ReShadeUndoRedo_API.h"

// isolated seh wrapper with no local c++ objects
inline bool invoke_foreign_handler_seh(
    ReShadeUndoRedo_ActionHandler handler, const char *tag,
    const uint8_t *data, uint32_t size, bool is_undo, void *user_data)
{
    __try { handler(tag, data, size, is_undo, user_data); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

class AddonRegistry {
public:
    struct HandlerEntry { ReShadeUndoRedo_ActionHandler handler = nullptr; void *user_data = nullptr; };

    static AddonRegistry &instance() { static AddonRegistry s; return s; }

    bool register_addon(const char *id, ReShadeUndoRedo_ActionHandler handler, void *user_data) {
        if (!id || !handler) return false;
        std::lock_guard<std::mutex> lk(mutex_);
        handlers_[id] = { handler, user_data };
        return true;
    }

    void unregister_addon(const char *id) {
        if (!id) return;
        std::lock_guard<std::mutex> lk(mutex_);
        handlers_.erase(id);
    }

    bool dispatch_action(const std::string &addon_id, const std::string &tag,
                         const std::vector<uint8_t> &payload, bool is_undo) {
        HandlerEntry entry;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            auto it = handlers_.find(addon_id);
            if (it == handlers_.end() || !it->second.handler) return false;
            entry = it->second;
        }
        return invoke_foreign_handler_seh(
            entry.handler, tag.c_str(),
            payload.empty() ? nullptr : payload.data(),
            static_cast<uint32_t>(payload.size()), is_undo, entry.user_data);
    }

    bool has_addon(const std::string &id) {
        std::lock_guard<std::mutex> lk(mutex_);
        return handlers_.count(id) > 0;
    }

private:
    std::mutex mutex_;
    std::unordered_map<std::string, HandlerEntry> handlers_;
};
