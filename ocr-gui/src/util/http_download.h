#pragma once
// WinHTTP 下载器：断点续传 + SHA256 校验 + 进度/取消。
//
// 不引第三方依赖（只用 Windows 自带的 WinHTTP / CNG）。
// HttpDownload 是阻塞调用，必须在后台线程里跑。

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace util {

/// 下载进度句柄：后台线程写，UI 线程只读。
struct DownloadHandle {
    std::atomic<std::int64_t> received{0};  // 已下载字节
    std::atomic<std::int64_t> total{0};     // 总字节，0 = 服务器没给长度
    std::atomic<bool> cancel{false};        // 置 true 请求取消
    std::atomic<bool> finished{false};      // 全部结束（成功或失败）
    std::atomic<bool> success{false};

    void SetMessage(const std::string& text);
    std::string Message();

private:
    std::mutex m_;
    std::string message_;
};

/// 文件的 SHA256（小写十六进制）；失败返回空串。
std::string Sha256File(const std::string& path);

/// 同步下载 url 到 outPath。
/// - 已存在 outPath 时直接返回 true（不重复下载）
/// - 支持 .part 断点续传：上次中断后接着下
/// - expectSha256 非空则校验，失败会删掉临时文件
/// - expectSize > 0 时校验字节数
/// handle 可为空（不需要进度/取消时）。
bool HttpDownload(const std::string& url, const std::string& outPath,
                  const std::string& expectSha256, std::int64_t expectSize,
                  DownloadHandle* handle, std::string* err);

/// https://huggingface.co/... -> https://hf-mirror.com/...；其他域名原样返回。
std::string MirrorUrl(const std::string& url);

}  // namespace util