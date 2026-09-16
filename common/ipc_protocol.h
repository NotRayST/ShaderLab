#pragma once
#include <windows.h>
#include <cstdint>

static constexpr uint32_t kIpcMagic   = 0x53484C56; // 'SHLV'
static constexpr uint32_t kIpcVersion = 17;
static constexpr uint32_t kMaxPathW   = 512;

enum class IpcAction : uint32_t {
    Undo             = 0,
    Redo             = 1,
    ResetRotation    = 2,
    ResetZoomPan     = 3,
    NudgeLeft        = 4,
    NudgeRight       = 5,
    FineTune         = 6,
    LockPan          = 7,
    LockZoom         = 8,
    LockRotate       = 9,
    LockView         = 10,
    ToggleFullscreen = 11,
    SaveProject      = 12,
    SaveProjectAs    = 13,
    ExportImage      = 14,
    ExportImageAs    = 15,
    ToggleBeforeAfter = 16,
    Count            = 17
};

enum class IpcCmd : uint32_t {
    None                 = 0,
    Snapshot             = 1,   // Host: capture live backbuffer to path (path = output file)
    SetView              = 2,   // Host: zoom=values[0], angle=values[1], pan_x=values[2], pan_y=values[3]
    TriggerAction        = 3,   // Host: action ID in values[0] (IpcAction enum)
    SetDepthPeek         = 4,   // Host: values[0] > 0.5f enables peek, <= 0.0f disables peek
    ResizeWindow         = 5,   // Host: width=values[0], height=values[1]
    ReshadeToggleEffects = 6,   // Addon: values[0] > 0.5f enable, <= 0.0f disable
    ReshadeReloadEffects = 7,   // Addon: triggers runtime->reload_effects()
    ReshadeToggleOverlay = 8,   // Addon: values[0] > 0.5f open, <= 0.0f close
    ReshadeSetTechnique  = 9,   // Addon: target=technique name, values[0] > 0.5f enable, <= 0.0f disable
    ReshadeSetUniform    = 10,  // Addon: target=uniform name, values[0..3]
    ReshadeSetPreset     = 11,  // Addon: path=preset file path
    TriggerExport        = 12,  // Host: path=output path, values[0]=settle_frames, values[1]=flags
    LoadImage            = 13,  // Host: path=input image path
};

struct IpcCommandMailbox {
    volatile uint32_t cmd_id;                  // IpcCmd
    volatile uint32_t seq_request;             // incremented by caller
    volatile uint32_t seq_handled;             // set by receiver when done
    volatile int32_t  status;                  // 0 = in-flight/pending, 1 = success, <0 = error
    char              target[64];              // technique / uniform name
    float             values[4];               // params
    wchar_t           path[kMaxPathW];         // file or preset path
    wchar_t           response_text[kMaxPathW];// error message or result info
};

// maximum number of keybinds supported in shared memory
static constexpr size_t kMaxKeybinds = 32;

struct IpcKeybind {
    uint32_t vk;      // Windows Virtual-Key code
    uint8_t  ctrl;    // 1 if Ctrl modifier required
    uint8_t  shift;   // 1 if Shift modifier required
    uint8_t  alt;     // 1 if Alt modifier required
    uint8_t  pad;
};

inline void make_shared_mem_name_for_pid(wchar_t *buf, size_t len, DWORD pid) {
    swprintf_s(buf, len, L"Local\\ShaderLabV_IPC_%u", pid);
}

inline void make_shared_mem_name(wchar_t *buf, size_t len) {
    make_shared_mem_name_for_pid(buf, len, GetCurrentProcessId());
}

inline void make_shared_mem_name_active(wchar_t *buf, size_t len) {
    swprintf_s(buf, len, L"Local\\ShaderLabV_IPC_Active");
}

// export lifecycle. app drives this, addon just reads + sets Requested
enum class ExportState : uint32_t {
    Idle      = 0,  // nothing running
    Requested = 1,  // addon bumped the counter, app should pick it up
    Loading   = 2,  // app is loading the image
    Rendering = 3,  // app presenting settle frames so reshade compiles/warms up
    Capturing = 4,  // this is the frame the addon grabs
    Done      = 5,  // written ok, host drops back to Idle next frame
    Failed    = 6,  // same but for errors
};

// error codes, stuffed into export_error on failure
static constexpr uint32_t IPC_OK                   = 0;
static constexpr uint32_t IPC_ERR_LOAD_FAILED      = 1;
static constexpr uint32_t IPC_ERR_RESIZE_FAILED    = 2;
static constexpr uint32_t IPC_ERR_STAGING_FAILED   = 3;
static constexpr uint32_t IPC_ERR_WRITE_FAILED     = 4;
static constexpr uint32_t IPC_ERR_FILE_NOT_FOUND   = 5;
static constexpr uint32_t IPC_ERR_UNSUPPORTED_FMT  = 6;
static constexpr uint32_t IPC_ERR_IMAGE_TOO_LARGE  = 7;

// export flags (bitfield)
static constexpr uint32_t EXPORT_FLAG_UNSYNCED        = (1 << 0); // bit0: present unsynced (sync_interval 0) during export
static constexpr uint32_t EXPORT_FLAG_AUTO_CONVERGE   = (1 << 1); // bit1: auto-converge enabled (default off)
static constexpr uint32_t EXPORT_FLAG_WYSIWYG         = (1 << 2); // bit2: export view transform (default on)
static constexpr uint32_t EXPORT_FLAG_EMBED_DEPTH     = (1 << 3); // bit3: embed slDp chunk into PNG
static constexpr uint32_t EXPORT_FLAG_DEPTH_SIDECAR   = (1 << 4); // bit4: export .sldepth sidecar file

// host flags (bitfield, host -> addon)
static constexpr uint32_t HOST_FLAG_ALIVE        = (1 << 0); // bit0: app process alive
static constexpr uint32_t HOST_FLAG_EXTERNAL_HUD = (1 << 1); // bit1: host publishes HUD textures for external composite

// number of HUD layers shared for external composite (compass, zoom, warning)
static constexpr uint32_t kHudLayers = 3;

// view interaction flags (bitfield)
static constexpr uint32_t VIEW_FLAG_ROTATING   = (1 << 0);
static constexpr uint32_t VIEW_FLAG_PANNING    = (1 << 1);
static constexpr uint32_t VIEW_FLAG_LOCK_ZOOM  = (1 << 2);
static constexpr uint32_t VIEW_FLAG_LOCK_ROT   = (1 << 3);
static constexpr uint32_t VIEW_FLAG_LOCK_PAN   = (1 << 4);
static constexpr uint32_t VIEW_FLAG_SNAPPED    = (1 << 5);
static constexpr uint32_t VIEW_FLAG_FINE       = (1 << 6);
static constexpr uint32_t VIEW_FLAG_PAN_LOCKED_ATTEMPT = (1 << 7);
static constexpr uint32_t VIEW_FLAG_DEPTH_PEEK        = (1 << 8);
static constexpr uint32_t VIEW_FLAG_TEXT_INPUT         = (1 << 9); // text input active in reshade overlay
static constexpr uint32_t VIEW_FLAG_BEFORE_AFTER       = (1 << 10); // before/after split comparison active
static constexpr uint32_t VIEW_FLAG_BEFORE_AFTER_DRAG  = (1 << 11); // actively dragging divider line

struct alignas(64) SharedControlBlock {
    uint32_t magic;
    uint32_t version;
    uint32_t host_flags;      // bit0 set while the app process is alive
    uint32_t heartbeat;

    wchar_t  host_status[kMaxPathW];

    // preview 
    // either side can push a path here and bump the counter, but the app is the one
    // that actually loads and shows it (addon drag-drop goes through the app window)
    wchar_t  preview_path[kMaxPathW];
    volatile uint32_t preview_counter;

    // drag and drop telemetry (host -> addon)
    wchar_t  dropped_file_path[kMaxPathW];
    int32_t  dropped_file_x;
    int32_t  dropped_file_y;
    volatile uint32_t dropped_file_counter;

    // export
    // addon fills in both paths then bumps export_counter, app watches for that and
    // walks export_state through the enum above
    wchar_t  export_input_path[kMaxPathW];
    wchar_t  export_output_path[kMaxPathW];
    volatile uint32_t export_counter;
    volatile ExportState export_state;
    uint32_t export_error;
    wchar_t  export_status[kMaxPathW];   // short message the addon can show in the overlay

    // export settings & settling (addon -> host)
    uint32_t export_settle_frames;       // cap, default 900, range 10..3600
    uint32_t export_flags;               // EXPORT_FLAG_*

    // live progress during rendering (host -> addon)
    uint32_t export_frame_index;         // frames presented since export started
    uint32_t export_converged;           // 1 if host auto-converged early, 0 otherwise
    float    export_last_delta;          // last frame-diff mean

    // external hud composite (host -> addon)
    // host publishes native ID3D11ShaderResourceView* handles for the GDI+ HUD layers.
    // same process and same device, so raw pointers are valid on both sides. 0 means
    // unused. hud_version bumps whenever a handle changes, e.g. texture realloc on window resize
    uint64_t hud_srv[kHudLayers];
    volatile uint32_t hud_version;

    // 3D Depth Canvas (host -> addon)
    // host publishes native ID3D11ShaderResourceView* handle for the re-encoded hyperbolic depth canvas
    uint64_t depth_srv_ptr;
    volatile uint32_t depth_version;
    uint32_t depth_valid;               // 1 if active image has 3D depth, 0 if 2D
    float    depth_far_plane;           // far plane used for linearization (e.g. 1000.0f)
    uint32_t depth_width;               // depth canvas width
    uint32_t depth_height;              // depth canvas height


    // addon liveness (addon -> host), increments once per finish_effects callback
    volatile uint32_t addon_heartbeat;

    // live view transform & interaction, synced both ways between host and addon
    float    view_angle;                 // degrees (snapped if snap active)
    float    view_raw_angle;             // un-snapped angle in degrees
    float    view_zoom;                  // zoom factor (0.25 .. 32.0)
    int32_t  view_pan[2];                // pan offset [x, y] in source pixels
    uint32_t view_image_width;           // active image native width
    uint32_t view_image_height;          // active image native height
    volatile uint32_t view_transform_version; // incremented whenever transform changes
    uint32_t view_interaction_flags;     // VIEW_FLAG_*
    float    before_after_angle;         // separator angle in degrees (0 = vertical L/R, 90 = horizontal T/B)
    float    before_after_split;         // separator split offset [-0.5..0.5] along normal
    uint32_t pad_ba;                     // 8-byte alignment pad for uint64_t below

    // per-gesture idle timestamps, host <-> addon, same process so GetTickCount64
    // stays consistent on both sides. each HUD fades on its own timer so a zoom
    // only lights up the zoom HUD and a rotate only lights up the compass
    uint64_t view_last_rotate_ms;        // GetTickCount64 ms of last rotate gesture
    uint64_t view_last_zoom_ms;          // GetTickCount64 ms of last zoom gesture
    uint64_t view_last_lock_ms;          // GetTickCount64 ms of last lock toggle (pan/zoom/rot/all)

    uint32_t fine_tune_vk;               // VK code of the fine-tune key (host reads this)

    uint32_t first_run_nudge;            // addon sets 1 once on first launch; host clears after resizing

    // preset switching (host -> addon)
    wchar_t  requested_preset_path[kMaxPathW];
    volatile uint32_t requested_preset_version;

    // active project file path (.shaderlab archive currently open)
    wchar_t  active_project_path[kMaxPathW];
    volatile uint32_t active_project_version;
    volatile uint32_t project_dirty;            // 1 if active project has unsaved changes, 0 if clean

    // toast notification (displayed by host ToastHud across HUD composite)
    wchar_t  toast_message[kMaxPathW];
    volatile uint32_t toast_version;

    // save project request (host/overlay -> addon)
    volatile uint32_t request_save_project;

    // save project as request (host/overlay -> addon)
    volatile uint32_t request_save_project_as;

    // export image request (host/overlay -> addon)
    volatile uint32_t request_export_image;

    // export image as request (host/overlay -> addon)
    volatile uint32_t request_export_image_as;

    // effect compilation & status (addon -> host)
    volatile uint32_t effects_ready;             // incremented when on_reshade_reloaded_effects completes
    volatile uint32_t effects_compiling;         // 1 while compiling, 0 when ready
    volatile uint32_t effects_enabled;           // 1 if effects are toggled on, 0 if disabled

    // keybinds (addon -> host, synced when modified or loaded)
    IpcKeybind keybind_table[kMaxKeybinds];
    volatile uint32_t keybind_version;

    // borderless fullscreen (addon -> host toggle request, host -> addon state)
    volatile uint32_t request_toggle_fullscreen;
    uint32_t is_fullscreen;

    // remote control & automation mailbox (CLI / MCP / external tools <-> Host & Addon)
    IpcCommandMailbox mailbox;
};
