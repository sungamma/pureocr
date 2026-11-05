#pragma once
// 跨平台小工具：UTF-8 路径的文件/目录操作、环境变量、可执行文件目录、打开文件管理器。
// Windows 走宽字符 API；Linux/macOS 路径本身就是 UTF-8 字节串，直接用窄字符串。

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

namespace util {

inline bool PathExists(const std::string& path) {
    if (path.empty()) return false;
#ifdef _WIN32
    const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (n <= 0) return false;
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), n);
    return GetFileAttributesW(w.c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
#endif
}

inline bool IsDirectory(const std::string& path) {
    if (path.empty()) return false;
#ifdef _WIN32
    const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (n <= 0) return false;
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), n);
    const DWORD attr = GetFileAttributesW(w.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

inline bool MakeDirs(const std::string& path) {
    if (path.empty()) return false;
#ifdef _WIN32
    const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (n <= 0) return false;
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), n);
    if (CreateDirectoryW(w.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
#else
    // 逐级创建：mkdir 只建最后一级
    std::string acc;
    for (size_t i = 0; i < path.size(); ++i) {
        acc.push_back(path[i]);
        const bool sep = path[i] == '/';
        if (!sep && i + 1 != path.size()) continue;
        if (acc.size() > 1 && ::mkdir(acc.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
    return IsDirectory(path);
#endif
}

inline bool RemoveFile(const std::string& path) {
    if (path.empty()) return false;
#ifdef _WIN32
    const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (n <= 0) return false;
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), n);
    return DeleteFileW(w.c_str()) != 0;
#else
    return std::remove(path.c_str()) == 0;
#endif
}

inline std::string GetEnv(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

/// 可执行文件所在目录（UTF-8），失败返回 "."。
inline std::string ExeDir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH] = L"";
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return ".";
    const int len = WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(n), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return ".";
    std::string s(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(n), s.data(), len, nullptr, nullptr);
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0) return ".";
    std::string s(buf);
#else
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return ".";
    buf[n] = 0;
    std::string s(buf);
#endif
    const size_t p = s.find_last_of("\\/");
    return p == std::string::npos ? std::string(".") : s.substr(0, p);
}

}  // namespace util
