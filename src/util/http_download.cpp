#include "util/http_download.h"

// Windows 实现：WinHTTP + CNG。Linux/macOS 见 http_download_posix.cpp。
#ifdef _WIN32

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include "util/utf8_io.h"

namespace util {
namespace {

struct UrlParts {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    bool secure = false;
};

bool CrackUrl(const std::string& url, UrlParts* out, std::string* err) {
    const std::wstring w = Utf8ToWide(url);
    if (w.empty()) {
        if (err) *err = "下载地址为空";
        return false;
    }
    std::vector<wchar_t> host(256);
    std::vector<wchar_t> path(8192);
    URL_COMPONENTS uc;
    std::memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host.data();
    uc.dwHostNameLength = static_cast<DWORD>(host.size());
    uc.lpszUrlPath = path.data();
    uc.dwUrlPathLength = static_cast<DWORD>(path.size());
    if (!WinHttpCrackUrl(w.c_str(), static_cast<DWORD>(w.size()), 0, &uc)) {
        if (err) *err = "无法解析下载地址: " + url;
        return false;
    }
    out->host.assign(host.data(), uc.dwHostNameLength);
    out->path.assign(path.data(), uc.dwUrlPathLength);
    if (uc.lpszExtraInfo && uc.dwExtraInfoLength > 0)
        out->path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    out->port = uc.nPort;
    out->secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    return true;
}

struct WinHttpHandles {
    HINTERNET session = nullptr;
    HINTERNET connect = nullptr;
    HINTERNET request = nullptr;
    ~WinHttpHandles() {
        if (request) WinHttpCloseHandle(request);
        if (connect) WinHttpCloseHandle(connect);
        if (session) WinHttpCloseHandle(session);
    }
};

std::int64_t FileSizeW(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    std::memset(&fad, 0, sizeof(fad));
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return -1;
    return (static_cast<std::int64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
}

void RemoveFileQuiet(const std::string& path) {
    const std::wstring w = Utf8ToWide(path);
    if (!w.empty()) DeleteFileW(w.c_str());
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

    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objLen = 0, resultLen = 0, cb = 0;
    std::vector<unsigned char> obj, digest(32);
    std::string out;

    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        std::fclose(f);
        return std::string();
    }
    bool ok = BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objLen),
                                sizeof(objLen), &cb, 0) == 0 &&
              BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&resultLen),
                                sizeof(resultLen), &cb, 0) == 0 &&
              resultLen == digest.size();
    if (ok) {
        obj.resize(objLen);
        ok = BCryptCreateHash(alg, &hash, obj.data(), objLen, nullptr, 0, 0) == 0;
    }
    if (ok) {
        std::vector<unsigned char> buf(256 * 1024);
        size_t n = 0;
        while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) {
            if (BCryptHashData(hash, buf.data(), static_cast<ULONG>(n), 0) != 0) {
                ok = false;
                break;
            }
        }
        ok = ok && std::ferror(f) == 0 &&
             BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0;
    }
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    std::fclose(f);
    if (!ok) return std::string();

    static const char* kHex = "0123456789abcdef";
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

    const std::wstring outW = Utf8ToWide(outPath);
    if (outW.empty()) return fail("目标路径无效: " + outPath);
    if (FileSizeW(outW) > 0) {
        if (handle) {
            handle->success = true;
            handle->finished = true;
        }
        return true;  // 已经有了
    }

    UrlParts parts;
    std::string parseErr;
    if (!CrackUrl(url, &parts, &parseErr)) return fail(parseErr);

    const std::string partPath = outPath + ".part";
    const std::wstring partW = Utf8ToWide(partPath);
    std::int64_t resumeFrom = FileSizeW(partW);
    if (resumeFrom < 0) resumeFrom = 0;

    if (handle) {
        handle->received = resumeFrom;
        handle->total = 0;
        handle->cancel = false;
        handle->finished = false;
        handle->success = false;
        handle->SetMessage("连接服务器…");
    }

    WinHttpHandles h;
    h.session = WinHttpOpen(L"PureOCR/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h.session)
        h.session = WinHttpOpen(L"PureOCR/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h.session) return fail("无法初始化网络会话");
    WinHttpSetTimeouts(h.session, 15000, 15000, 30000, 60000);

    h.connect = WinHttpConnect(h.session, parts.host.c_str(), parts.port, 0);
    if (!h.connect) return fail("无法连接 " + std::string(url));
    h.request = WinHttpOpenRequest(h.connect, L"GET", parts.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                   WINHTTP_DEFAULT_ACCEPT_TYPES,
                                   parts.secure ? WINHTTP_FLAG_SECURE : 0);
    if (!h.request) return fail("无法创建 HTTP 请求");

    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(h.request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    WinHttpAddRequestHeaders(h.request, L"User-Agent: PureOCR\r\n", -1L,
                             WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    if (resumeFrom > 0) {
        wchar_t range[80];
        swprintf_s(range, L"Range: bytes=%lld-", static_cast<long long>(resumeFrom));
        WinHttpAddRequestHeaders(h.request, range, -1L,
                                 WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }

    if (!WinHttpSendRequest(h.request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
        return fail("网络请求失败（无法连接或超时）: " + url);
    if (!WinHttpReceiveResponse(h.request, nullptr))
        return fail("没有收到服务器响应: " + url);

    DWORD status = 0, statusLen = sizeof(status);
    if (!WinHttpQueryHeaders(h.request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusLen,
                             WINHTTP_NO_HEADER_INDEX)) {
        return fail("无法读取 HTTP 状态码");
    }

    bool append = false;
    if (status == 206 && resumeFrom > 0) {
        append = true;
    } else if (status == 200) {
        append = false;
        resumeFrom = 0;
    } else if (status == 404) {
        return fail("服务器上没有这个文件（HTTP 404）");
    } else {
        return fail("服务器返回 HTTP " + std::to_string(status));
    }

    std::int64_t contentLen = 0;
    {
        wchar_t buf[64] = {};
        DWORD bufLen = sizeof(buf);
        if (WinHttpQueryHeaders(h.request, WINHTTP_QUERY_CONTENT_LENGTH,
                                WINHTTP_HEADER_NAME_BY_INDEX, buf, &bufLen,
                                WINHTTP_NO_HEADER_INDEX)) {
            contentLen = _wtoi64(buf);
        }
    }

    std::int64_t received = append ? resumeFrom : 0;
    if (handle) {
        handle->received = received;
        handle->total = contentLen > 0 ? received + contentLen : 0;
    }

    FILE* f = OpenUtf8(partPath, append ? L"ab" : L"wb");
    if (!f) return fail("无法写入临时文件: " + partPath);

    bool ok = true;
    std::string ioErr;
    std::vector<char> buf(128 * 1024);
    for (;;) {
        if (handle && handle->cancel.load()) {
            ok = false;
            ioErr = "已取消下载";
            break;
        }
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(h.request, &avail)) {
            ok = false;
            ioErr = "接收数据失败（网络中断）";
            break;
        }
        if (avail == 0) break;
        const DWORD want = static_cast<DWORD>(std::min<size_t>(avail, buf.size()));
        DWORD got = 0;
        if (!WinHttpReadData(h.request, buf.data(), want, &got)) {
            ok = false;
            ioErr = "读取数据失败（网络中断）";
            break;
        }
        if (got == 0) break;
        if (std::fwrite(buf.data(), 1, got, f) != got) {
            ok = false;
            ioErr = "写入磁盘失败（磁盘空间不足？）";
            break;
        }
        received += got;
        if (handle) handle->received = received;
    }
    std::fclose(f);
    if (!ok) return fail(ioErr);

    if (expectSize > 0 && received != expectSize) {
        RemoveFileQuiet(partPath);
        return fail("文件大小不符（期望 " + std::to_string(expectSize) + "，实际 " +
                    std::to_string(received) + "）");
    }

    if (handle) handle->SetMessage("校验 SHA256…");
    if (!expectSha256.empty()) {
        const std::string got = Sha256File(partPath);
        if (got.empty() || _stricmp(got.c_str(), expectSha256.c_str()) != 0) {
            RemoveFileQuiet(partPath);
            return fail("SHA256 校验失败，文件可能已损坏或被替换");
        }
    }

    std::error_code ec;
    std::filesystem::rename(std::filesystem::path(partW), std::filesystem::path(outW), ec);
    if (ec) {
        return fail("无法保存到 " + outPath + "（" + ec.message() + "）");
    }

    if (handle) {
        handle->SetMessage("完成");
        handle->success = true;
        handle->finished = true;
    }
    return true;
}

}  // namespace util

#endif  // _WIN32
