#pragma once
#include <windows.h>
#include <cstdint>

static constexpr uint32_t kIpcMagic   = 0x53484C56; // 'SHLV'
static constexpr uint32_t kIpcVersion = 3;
static constexpr uint32_t kMaxPathW   = 512;

inline void make_shared_mem_name(wchar_t *buf, size_t len) {
    DWORD pid = GetCurrentProcessId();
    swprintf_s(buf, len, L"Local\\ShaderLabV_IPC_%u", pid);
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

// error codes, stuffed into export_error on failure (neat)
static constexpr uint32_t IPC_OK                   = 0;
static constexpr uint32_t IPC_ERR_LOAD_FAILED      = 1;
static constexpr uint32_t IPC_ERR_RESIZE_FAILED    = 2;
static constexpr uint32_t IPC_ERR_STAGING_FAILED   = 3;
static constexpr uint32_t IPC_ERR_WRITE_FAILED     = 4;
static constexpr uint32_t IPC_ERR_FILE_NOT_FOUND   = 5;
static constexpr uint32_t IPC_ERR_UNSUPPORTED_FMT  = 6;
static constexpr uint32_t IPC_ERR_IMAGE_TOO_LARGE  = 7;

struct alignas(64) SharedControlBlock {
    uint32_t magic;
    uint32_t version;
    uint32_t host_flags;      // bit0 set while the app process is alive
    uint32_t heartbeat;

    wchar_t  host_status[kMaxPathW];

    // preview 
    // either side can push a path here + bump the counter; the app is the one that
    // actually loads it and shows it (addon drag-drop is handled via app window)
    wchar_t  preview_path[kMaxPathW];
    volatile uint32_t preview_counter;

    // export
    // addon fills both paths then bumps export_counter; the app watches that and
    // walks export_state through the enum above
    wchar_t  export_input_path[kMaxPathW];
    wchar_t  export_output_path[kMaxPathW];
    volatile uint32_t export_counter;
    volatile ExportState export_state;
    uint32_t export_error;
    wchar_t  export_status[kMaxPathW];   // short message the addon can show in the overlay
};
