#pragma once

#include "types.h"
#include <vector>

namespace gui {

enum class ExitModalDecision {
    None = 0,
    SaveAndExit,
    DontSaveAndExit,
    Cancel
};

// Renders the centered "Unsaved Changes" modal dialog and handles Escape to cancel.
// Set open_requested = true to open the modal; this resets open_requested once opened.
ExitModalDecision render_unsaved_exit_modal(
    bool &open_requested,
    bool &out_is_open,
    const std::vector<DocumentTab> &tabs,
    UiDomRegistry *dom
);

} // namespace gui
