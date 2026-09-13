#pragma once
#define ImTextureID ImU64
#include <reshade.hpp>

#include <string>

struct SharedControlBlock;
class JobQueueManager;

void on_overlay(reshade::api::effect_runtime *runtime);

bool execute_project_save(
    reshade::api::effect_runtime *runtime,
    const std::wstring &active_path,
    const std::wstring &target_project_path,
    SharedControlBlock *block,
    JobQueueManager &queue_mgr
);

void trigger_save_project_workflow(reshade::api::effect_runtime *runtime, bool force_dialog = false);
void trigger_export_workflow(reshade::api::effect_runtime *runtime);
void trigger_export_as_workflow(reshade::api::effect_runtime *runtime);
