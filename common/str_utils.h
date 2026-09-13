#pragma once
#include <string>
#include <windows.h>

namespace str_utils {

inline std::string wide_to_utf8(const std::wstring &wstr) {
    if (wstr.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string out(size - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, out.data(), size, nullptr, nullptr);
    return out;
}

inline std::string wide_to_utf8(const wchar_t *wstr) {
    if (!wstr || !*wstr) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string out(size - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, out.data(), size, nullptr, nullptr);
    return out;
}

inline std::wstring utf8_to_wide(const std::string &utf8_str) {
    if (utf8_str.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, utf8_str.c_str(), -1, nullptr, 0);
    if (size <= 1) return {};
    std::wstring out(size - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8_str.c_str(), -1, out.data(), size);
    return out;
}

inline std::wstring utf8_to_wide(const char *utf8_str) {
    if (!utf8_str || !*utf8_str) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, utf8_str, -1, nullptr, 0);
    if (size <= 1) return {};
    std::wstring out(size - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8_str, -1, out.data(), size);
    return out;
}

} // namespace str_utils
