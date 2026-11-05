#pragma once
// 把"某个预设缺哪些模型、怎么下"的逻辑集中在这里，CLI 和 GUI 共用。

#include <string>
#include <vector>

#include "util/http_download.h"
#include "util/model_registry.h"

namespace util {

/// 列出 modelDir 下缺失、且有清单信息的模型文件（顺序 det, rec, cls）。
std::vector<ModelFile> MissingForNames(const std::string& modelDir, const std::vector<std::string>& names);

/// 同步下载一批模型（阻塞，放后台线程）。逐个文件尝试 url → alt_url → hf-mirror。
/// preferMirror 为 true 时先试 alt_url（镜像）。
bool FetchModels(const std::string& modelDir, const std::vector<ModelFile>& files, bool preferMirror,
                 DownloadHandle* handle, std::string* err);

/// "4.6 MB" 这种人读的大小。
std::string FormatBytes(std::int64_t bytes);

}  // namespace util