#pragma once
// UTF-8 路径的文件读写封装。
//
// 坑：MSVC 的 fopen / fopen_s 把 char* 路径按当前 ANSI 代码页（中文系统是 GBK）解释，
// 传 UTF-8 路径时中文会变成乱码（导出 CSV 的文件名乱码就是这么来的）。
// 这里统一按 UTF-8 -> UTF-16 转换后用 _wfopen_s 打开。
#include <cstdio>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace util {

#ifdef _WIN32
inline std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int len = static_cast<int>(s.size());
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), len, nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), len, &w[0], n);
    return w;
}

inline std::string WideToUtf8(const wchar_t* s) {
    if (!s) return std::string();
    const int len = static_cast<int>(wcslen(s));
    if (len <= 0) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, len, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, len, &out[0], n, nullptr, nullptr);
    return out;
}

/// 以 UTF-8 路径打开文件；mode 用 L"rb" / L"wb" / L"ab" 这种宽字符形式。失败返回 nullptr。
inline FILE* OpenUtf8(const std::string& path, const wchar_t* mode) {
    const std::wstring w = Utf8ToWide(path);
    if (w.empty()) return nullptr;
    FILE* f = nullptr;
    if (_wfopen_s(&f, w.c_str(), mode) != 0) return nullptr;
    return f;
}
#else
// POSIX：路径本来就是 UTF-8 字节串，直接 fopen。
inline FILE* OpenUtf8(const std::string& path, const wchar_t* mode) {
    std::string m;
    for (const wchar_t* p = mode; p && *p; ++p) m.push_back(static_cast<char>(*p));
    if (path.empty() || m.empty()) return nullptr;
    return std::fopen(path.c_str(), m.c_str());
}
#endif

}  // namespace util
