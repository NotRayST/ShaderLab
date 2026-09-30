#pragma once
#include <stdint.h>
#include <stdbool.h>

#if defined(UNDOREDO_EXPORTS)
#  define UNDOREDO_API __declspec(dllexport)
#elif defined(UNDOREDO_STATIC)
#  define UNDOREDO_API
#else
#  define UNDOREDO_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ReShadeUndoRedo_ActionHandler)(
    const char *action_tag,
    const uint8_t *payload,
    uint32_t payload_size,
    bool is_undo,
    void *user_data
);

// register or unregister addon handler
UNDOREDO_API bool ReShadeUndoRedo_RegisterAddon(
    const char *addon_id,
    ReShadeUndoRedo_ActionHandler handler,
    void *user_data
);

UNDOREDO_API void ReShadeUndoRedo_UnregisterAddon(
    const char *addon_id
);

// push custom action to history
UNDOREDO_API bool ReShadeUndoRedo_PushCustomAction(
    const char *addon_id,
    const char *action_tag,
    const char *display_label,
    const uint8_t *before_state,
    const uint8_t *after_state,
    uint32_t payload_size
);

// helper for window geometry
UNDOREDO_API bool ReShadeUndoRedo_PushWindowTransform(
    const char *window_name,
    const float before_pos[2],
    const float before_size[2],
    const float after_pos[2],
    const float after_size[2]
);

#ifdef __cplusplus
}
#endif


