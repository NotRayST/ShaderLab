#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <deque>
#include <filesystem>
#include "../../common/ipc_protocol.h"

class JobQueueManager {
public:
    static JobQueueManager &get();

    void update();
    void shutdown();

    bool is_connected() const { return m_is_connected; }
    SharedControlBlock *get_control_block() const { return m_block; }

    void set_preview_image(const std::wstring &path);
    bool trigger_export(const std::wstring &input_path, const std::wstring &output_folder);

    ExportState get_export_state() const;
    const std::wstring &get_active_image_path() const { return m_active_image_path; }
    void set_active_image_path(const std::wstring &path) { m_active_image_path = path; }

    const std::deque<std::wstring> &get_logs() const { return m_logs; }
    void add_log(const std::wstring &msg);

private:
    JobQueueManager();
    ~JobQueueManager();

    SharedControlBlock *m_block = nullptr;
    HANDLE m_hMap = nullptr;
    bool m_is_connected = false;
    std::wstring m_active_image_path;

    std::deque<std::wstring> m_logs;
    static constexpr size_t kMaxLogs = 100;
};
