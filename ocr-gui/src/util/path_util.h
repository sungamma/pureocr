#pragma once
#include <string>

namespace util {

/// 在文件名的扩展名前插入后缀。
/// 全部按 UTF-8 字节处理：不经 std::filesystem::path 的窄字符转换，
/// 避免中文路径在 ACP(GBK) 与 UTF-8 之间转换失败抛 filesystem_error。
inline std::string InsertBeforeExtension(const std::string& path, const std::string& suffix) {
    const size_t slash = path.find_last_of("\\/");
    const size_t dot = path.find_last_of('.');
    const bool hasExt = dot != std::string::npos && (slash == std::string::npos || dot > slash);
    const size_t stemEnd = hasExt ? dot : path.size();
    return path.substr(0, stemEnd) + suffix + path.substr(stemEnd);
}

/// 所在目录（UTF-8），不含末尾分隔符；无分隔符时返回空串
inline std::string ParentDir(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    if (slash == std::string::npos) return std::string();
    return path.substr(0, slash);
}

}  // namespace util
