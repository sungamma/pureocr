// Linux / macOS 下载器：libcurl + OpenSSL。Windows 版见 http_download.cpp。
#ifndef _WIN32

#include "util/http_download.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <curl/curl.h>
#include <openssl/sha.h>

#include "util/platform.h"
#include "util/utf8_io.h"

namespace util {
namespace {

std::int64_t FileSize(const std::string& path) {
    std::error_code ec;
    const auto sz = std::filesystem::file_size(std::filesystem::u8path(path), ec);
    return ec ? -1 : static_cast<std::int64_t>(sz);
}

void RemoveFileQuiet(const std::string& path) { util::RemoveFile(path); }

struct WriteCtx {
    FILE* file = nullptr;
    DownloadHandle* handle = nullptr;
    std::int64_t received = 0;
    bool writeFailed = false;
    bool cancelled = false;
};

size_t OnWrite(char* data, size_t size, size_t nmemb, void* userp) {
    WriteCtx* ctx = static_cast<WriteCtx*>(userp);
    const size_t n = size * nmemb;
    if (!ctx->file || std::fwrite(data, 1, n, ctx->file) != n) {
        ctx->writeFailed = true;
        return 0;
    }
    ctx->received += static_cast<std::int64_t>(n);
    if (ctx->handle) ctx->handle->received = ctx->received;
    return n;
}

int OnProgress(void* userp, curl_off_t, curl_off_t dlnow, curl_off_t, curl_off_t) {
    WriteCtx* ctx = static_cast<WriteCtx*>(userp);
    if (ctx->handle) {
        if (ctx->handle->cancel.load()) {
            ctx->cancelled = true;
            return 1;
        }
        if (dlnow > 0) ctx->handle->total = dlnow;
    }
    return 0;
}

}  // namespace

void DownloadHandle::SetMessage(const std::string& text) {
    std::lock_guard<std::mutex> lock(m_);
    message_ = text;
}

std::string DownloadHandle::Message() {
    std::lock_guard<std::mutex> lock(m_);
    return message_;
}

std::string Sha256File(const std::string& path) {
    FILE* f = OpenUtf8(path, L"rb");
    if (!f) return std::string();

    SHA256_CTX sha;
    SHA256_Init(&sha);
    std::vector<unsigned char> buf(256 * 1024);
    size_t n = 0;
    while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) SHA256_Update(&sha, buf.data(), n);
    const bool bad = std::ferror(f) != 0;
    std::fclose(f);
    if (bad) return std::string();

    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256_Final(digest, &sha);
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (unsigned char c : digest) {
        out.push_back(kHex[c >> 4]);
        out.push_back(kHex[c & 0x0F]);
    }
    return out;
}

std::string MirrorUrl(const std::string& url) {
    static const std::string kFrom = "https://huggingface.co/";
    static const std::string kTo = "https://hf-mirror.com/";
    if (url.compare(0, kFrom.size(), kFrom) == 0) return kTo + url.substr(kFrom.size());
    return url;
}

bool HttpDownload(const std::string& url, const std::string& outPath,
                  const std::string& expectSha256, std::int64_t expectSize,
                  DownloadHandle* handle, std::string* err) {
    auto fail = [&](const std::string& msg) {
        if (err) *err = msg;
        if (handle) {
            handle->SetMessage(msg);
            handle->success = false;
            handle->finished = true;
        }
        return false;
    };

    if (outPath.empty()) return fail("目标路径无效");
    if (FileSize(outPath) > 0) {
        if (handle) {
            handle->success = true;
            handle->finished = true;
        }
        return true;
    }

    const std::string partPath = outPath + ".part";
    std::int64_t resumeFrom = FileSize(partPath);
    if (resumeFrom < 0) resumeFrom = 0;

    if (handle) {
        handle->received = resumeFrom;
        handle->total = 0;
        handle->cancel = false;
        handle->finished = false;
        handle->success = false;
        handle->SetMessage("连接服务器…");
    }

    FILE* file = OpenUtf8(partPath, resumeFrom > 0 ? L"ab" : L"wb");
    if (!file) return fail("无法写入临时文件: " + partPath);

    CURL* curl = curl_easy_init();
    if (!curl) {
        std::fclose(file);
        return fail("无法初始化网络会话");
    }

    WriteCtx ctx;
    ctx.file = file;
    ctx.handle = handle;
    ctx.received = resumeFrom;

    char errBuf[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "ocr-gui/1.0");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errBuf);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, OnWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, OnProgress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    if (resumeFrom > 0) curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, static_cast<curl_off_t>(resumeFrom));

    const CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    std::fclose(file);

    if (ctx.cancelled) return fail("已取消下载");
    if (ctx.writeFailed) return fail("写入磁盘失败（磁盘空间不足？）");
    if (rc != CURLE_OK) {
        const char* rcs = errBuf[0] ? errBuf : curl_easy_strerror(rc);
        return fail(std::string("网络请求失败: ") + (rcs ? rcs : "unknown"));
    }
    if (status == 404) return fail("服务器上没有这个文件（HTTP 404）");
    if (status != 200 && status != 206) return fail("服务器返回 HTTP " + std::to_string(status));

    if (expectSize > 0 && ctx.received != expectSize) {
        RemoveFileQuiet(partPath);
        return fail("文件大小不符（期望 " + std::to_string(expectSize) + "，实际 " +
                    std::to_string(ctx.received) + "）");
    }

    if (handle) handle->SetMessage("校验 SHA256…");
    if (!expectSha256.empty()) {
        const std::string got = Sha256File(partPath);
        bool same = got.size() == expectSha256.size();
        for (size_t i = 0; same && i < got.size(); ++i) {
            const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(got[i])));
            const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(expectSha256[i])));
            same = a == b;
        }
        if (!same) {
            RemoveFileQuiet(partPath);
            return fail("SHA256 校验失败，文件可能已损坏或被替换");
        }
    }

    std::error_code ec;
    std::filesystem::rename(std::filesystem::u8path(partPath), std::filesystem::u8path(outPath), ec);
    if (ec) return fail("无法保存到 " + outPath + "（" + ec.message() + "）");

    if (handle) {
        handle->SetMessage("完成");
        handle->success = true;
        handle->finished = true;
    }
    return true;
}

}  // namespace util

#endif  // !_WIN32