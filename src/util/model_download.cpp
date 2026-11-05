#include "util/model_download.h"

#include <algorithm>
#include <cstdio>

namespace util {

std::string FormatBytes(std::int64_t bytes) {
    char buf[48];
    if (bytes < 0) {
        std::snprintf(buf, sizeof(buf), "?");
    } else if (bytes < 1024) {
        std::snprintf(buf, sizeof(buf), "%lld B", static_cast<long long>(bytes));
    } else if (bytes < 1024LL * 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else if (bytes < 1024LL * 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else {
        std::snprintf(buf, sizeof(buf), "%.2f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    }
    return std::string(buf);
}

std::vector<ModelFile> MissingForNames(const std::string& modelDir, const std::vector<std::string>& names) {
    std::vector<ModelFile> missing;
    const std::vector<ModelFile> registry = LoadModelRegistry(modelDir);
    for (const auto& name : names) {
        if (name.empty()) continue;
        if (FileReady(JoinPath(modelDir, name))) continue;
        if (const ModelFile* info = FindModelFile(registry, name)) {
            missing.push_back(*info);
        } else {
            ModelFile m;
            m.file = name;
            missing.push_back(m);
        }
    }
    return missing;
}

bool FetchModels(const std::string& modelDir, const std::vector<ModelFile>& files, bool preferMirror,
                 DownloadHandle* handle, std::string* err) {
    for (const auto& m : files) {
        if (handle && handle->cancel.load()) {
            if (err) *err = "已取消";
            return false;
        }
        const std::string out = JoinPath(modelDir, m.file);
        if (FileReady(out)) continue;

        std::vector<std::string> urls;
        const std::string mirror = MirrorUrl(m.url);
        const bool hasAlt = !m.altUrl.empty() && m.altUrl != m.url;
        if (preferMirror) {
            if (hasAlt) urls.push_back(m.altUrl);
            if (mirror != m.url) urls.push_back(mirror);
            if (!m.url.empty()) urls.push_back(m.url);
        } else {
            if (!m.url.empty()) urls.push_back(m.url);
            if (hasAlt) urls.push_back(m.altUrl);
            if (mirror != m.url) urls.push_back(mirror);
        }
        urls.erase(std::unique(urls.begin(), urls.end()), urls.end());

        if (urls.empty()) {
            if (err) *err = m.file + " 没有在线下载源，请手动放到 " + modelDir;
            return false;
        }

        if (handle) handle->SetMessage("下载 " + m.file);
        bool ok = false;
        std::string lastErr;
        for (const auto& url : urls) {
            if (handle && handle->cancel.load()) break;
            if (handle) {
                handle->received = 0;
                handle->total = 0;
            }
            std::string one;
            ok = HttpDownload(url, out, m.sha256, m.size, handle, &one);
            if (ok) break;
            lastErr = one;
        }
        if (!ok) {
            if (handle && handle->cancel.load()) {
                if (err) *err = "已取消";
            } else if (err) {
                *err = m.file + " 下载失败：" + lastErr;
            }
            return false;
        }
    }
    if (handle) handle->SetMessage("完成");
    return true;
}

}  // namespace util