#pragma once

#include <cstddef>

namespace reshade { namespace api { struct effect_runtime; } }

// self-contained undo/redo for reshade shader-effect edits
//
// hooks reshade's addon events to record uniform-value changes and technique
// enable/disable, then exposes ctrl+z / ctrl+y (plus ctrl+shift+z) as undo/redo
// while the reshade overlay is open. only depends on the reshade addon api and
// the shared keybinds module, so it can get dropped into any addon or its own
// dll:
//
//   DllMain(DLL_PROCESS_ATTACH) { register_addon(hModule); undo_history::init(); }
//   DllMain(DLL_PROCESS_DETACH) { undo_history::shutdown(); unregister_addon(hModule); }
//
// history survives effect reloads, recorded changes remember the effect + uniform /
// technique names and re-resolve their handles after each reload
namespace undo_history {

// registers every reshade event the module needs. call this once after the addon
// itself is registered. keybind defaults get (re)loaded whenever an effect runtime shows up
void init();

// unregisters everything init() registered
void shutdown();

size_t undo_count(); // steps that can be undone right now
size_t redo_count(); // steps that can be redone right now

void undo(reshade::api::effect_runtime *runtime);
void redo(reshade::api::effect_runtime *runtime);

size_t history_size();

// fills a short human-readable label for the change at index (0 = most recent).
// returns false if index is out of range
bool history_label(size_t index, char *buf, size_t size);

// true if the change at index is currently sitting in the undone (redo) branch
bool history_undone(size_t index);

// applies undo/redo until the current state lands on the given history index
// (0 = most recent). no-op if we're already there. pass in the active effect
// runtime, available from any reshade callback
void jump_to(reshade::api::effect_runtime *runtime, size_t index);

// drops the whole history, used on runtime teardown
void clear();

// drops only the erase-step entries (used when the erase history is reset
// for a new image / project so stale undo entries can't restore old stages)
void clear_erase_entries();

// records an erase operation in the undo/redo history
void record_erase_step(uint32_t step);

} // namespace undo_history
