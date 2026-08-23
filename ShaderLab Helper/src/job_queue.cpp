#include "job_queue.h"
#include <chrono>
#include <iomanip>
#include <sstream>

namespace fs = std::filesystem;

JobQueueManager &JobQueueManager::get() {
    static JobQueueManager instance;
    return instance;
}

JobQueueManager::JobQueueManager() = default;

JobQueueManager::~JobQueueManager() {
    shutdown();
}

void JobQueueManager::update() {
    // try to attach to the app's shared block. dllmain was a fucking trap, never do the attach there
    // (dxgi loads before the app creates the mapping), so just poll every frame
    if (!m_is_connected) {
        wchar_t shm_name[kMaxPathW] = {};
        make_shared_mem_name(shm_name, _countof(shm_name));

        m_hMap = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, shm_name);
        if (m_hMap) {
            m_block = static_cast<SharedControlBlock *>(
                MapViewOfFile(m_hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedControlBlock))
            );
            if (m_block && m_block->magic == kIpcMagic && m_block->version == kIpcVersion) {
                m_is_connected = true;
                if (wcslen(m_block->preview_path) > 0) {
                    m_active_image_path = m_block->preview_path;
                }
            } else {
                if (m_block) {
                    UnmapViewOfFile(m_block);
                    m_block = nullptr;
                }
                CloseHandle(m_hMap);
                m_hMap = nullptr;
            }
        }
    } else {
        // host went away (closed/crashed), release everything
        // HACK: host_flags bit0 is our ghetto alive flag, set to 0 on shutdown
        if (!m_block || (m_block->host_flags & 1) == 0) {

            shutdown();
        } else {
            // keep our active image in sync with whatever host is showing (drag-drop lands there first)
            if (wcslen(m_block->preview_path) > 0 && m_active_image_path != m_block->preview_path) {
                m_active_image_path = m_block->preview_path;
            }
        }
    }
}

void JobQueueManager::shutdown() {
    if (m_block) {
        UnmapViewOfFile(m_block);
        m_block = nullptr;
    }
    if (m_hMap) {
        CloseHandle(m_hMap);
        m_hMap = nullptr;
    }
    m_is_connected = false;
}

void JobQueueManager::set_preview_image(const std::wstring &path) {
    if (path.empty()) return;
    m_active_image_path = path;

    if (m_block) {
        wcsncpy_s(m_block->preview_path, kMaxPathW, path.c_str(), _TRUNCATE);
        m_block->preview_counter++;
        add_log(L"Loaded image: " + path);
    }
}

bool JobQueueManager::trigger_export(const std::wstring &input_path, const std::wstring &output_folder) {
    // queue a job into the shared block, host picks it up on its next frame
    if (!m_block || !m_is_connected) {
        add_log(L"Export failed: host not connected");
        return false;
    }

    if (input_path.empty() || !fs::exists(input_path)) {
        add_log(L"Export failed: input image does not exist: " + input_path);
        return false;
    }

    fs::path in_p(input_path);
    fs::path out_dir(output_folder.empty() ? L"out" : output_folder);
    std::error_code ec;
    fs::create_directories(out_dir, ec);

    fs::path out_file = out_dir / (in_p.stem().wstring() + L".png");

    wcsncpy_s(m_block->export_input_path, kMaxPathW, in_p.wstring().c_str(), _TRUNCATE);
    wcsncpy_s(m_block->export_output_path, kMaxPathW, out_file.wstring().c_str(), _TRUNCATE);
    m_block->export_state = ExportState::Requested;
    m_block->export_counter++;

    add_log(L"Exporting: " + in_p.filename().wstring() + L" -> " + out_file.wstring());
    return true;
}

ExportState JobQueueManager::get_export_state() const {
    if (!m_block) return ExportState::Idle;
    return m_block->export_state;
}

void JobQueueManager::add_log(const std::wstring &msg) {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    struct tm tm_buf {};
    localtime_s(&tm_buf, &time_t_now);

    std::wstringstream ss;
    ss << L"[" << std::setfill(L'0')
       << std::setw(2) << tm_buf.tm_hour << L":"
       << std::setw(2) << tm_buf.tm_min << L":"
       << std::setw(2) << tm_buf.tm_sec << L"] "
       << msg;

    m_logs.push_back(ss.str());
    if (m_logs.size() > kMaxLogs) {
        m_logs.pop_front();
    }
}
