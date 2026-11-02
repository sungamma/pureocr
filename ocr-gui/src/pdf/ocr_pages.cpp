#include "pdf/ocr_pages.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include "ocr/engine.h"
#include "pdf/doc.h"
#include "pdf/textlayer.h"

namespace pdf {
namespace {

std::wstring ToWide(const std::string& s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

struct PageResult {
    std::vector<ocr::TextLine> lines;
    int width = 0;
    int height = 0;
    bool done = false;
};

}  // namespace

bool ParsePageSpec(const std::string& spec, std::vector<int>* pages) {
    pages->clear();
    size_t i = 0;
    while (i < spec.size()) {
        while (i < spec.size() && (spec[i] == ',' || spec[i] == ' ')) ++i;
        if (i >= spec.size()) break;
        const size_t start = i;
        while (i < spec.size() && spec[i] != ',') ++i;
        const std::string token = spec.substr(start, i - start);
        const size_t dash = token.find('-');
        try {
            if (dash == std::string::npos) {
                pages->push_back(std::stoi(token) - 1);
            } else {
                const int lo = std::stoi(token.substr(0, dash));
                const int hi = std::stoi(token.substr(dash + 1));
                for (int p = lo; p <= hi; ++p) pages->push_back(p - 1);
            }
        } catch (...) {
            return false;
        }
    }
    return true;
}

bool RunPdfOcr(const OcrRunOptions& opt, const OcrProgress& progress, OcrRunStats* stats,
               std::string* err) {
    const auto t0 = std::chrono::steady_clock::now();
    std::string message;
    // 降低进程优先级，避免 OCR 抢占前台响应
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);

    Document doc;
    if (!doc.Open(opt.inputPath, &message)) {
        if (err) *err = "打开 PDF 失败: " + message;
        return false;
    }
    const int pageCount = doc.PageCount();
    if (pageCount <= 0) {
        if (err) *err = "PDF 没有可处理的页面";
        return false;
    }

    std::vector<int> pages;
    if (opt.pages.empty()) {
        for (int i = 0; i < pageCount; ++i) pages.push_back(i);
    } else {
        for (int p : opt.pages) {
            if (p >= 0 && p < pageCount) pages.push_back(p);
        }
    }
    const int n = static_cast<int>(pages.size());
    if (n == 0) {
        if (err) *err = "指定的页面范围为空";
        return false;
    }

    TextLayer layer;
    TextLayerOptions topts;
    topts.fontPath = opt.fontPath;
    topts.dpi = opt.dpi;
    if (!layer.Init(doc, topts, &message)) {
        if (err) *err = message;
        return false;
    }
    double maskSeconds = 0.0;
    if (opt.maskOriginal) {
        const auto tm0 = std::chrono::steady_clock::now();
        const bool ok = layer.MaskOriginalText(&message);
        maskSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - tm0).count();
        if (!ok) {
            if (err) *err = "屏蔽原文字层失败: " + message;
            return false;
        }
    }

    std::vector<PageResult> results(static_cast<size_t>(n));
    std::atomic<int> next{0};
    std::atomic<bool> failed{false};
    std::mutex mtx;
    std::condition_variable cv;
    std::string workerError;

    int threadCount = opt.threads;
    if (threadCount < 1) threadCount = 1;
    if (threadCount > n) threadCount = n;
    if (threadCount > 16) threadCount = 16;

    auto fail = [&](const std::string& msg) {
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (workerError.empty()) workerError = msg;
        }
        failed = true;
        cv.notify_all();
    };

    auto worker = [&]() {
        Document wdoc;
        std::string localErr;
        if (!wdoc.Open(opt.inputPath, &localErr)) {
            fail("渲染线程打开 PDF 失败: " + localErr);
            return;
        }
        ocr::EngineOptions eopt;
        eopt.modelDir = ToWide(opt.modelDir);
        eopt.minScore = opt.minScore;
        // 每个 worker 各自一个 ORT session：限制单 session 的线程数，
        // 避免「线程数 × ORT 默认线程」把 CPU 过度订阅（实测 8 线程反而更慢）
        {
            const unsigned hw = std::thread::hardware_concurrency();
            int perWorker = static_cast<int>(hw) / (threadCount > 0 ? threadCount : 1);
            if (perWorker < 1) perWorker = 1;
            if (perWorker > 8) perWorker = 8;
            eopt.threads = perWorker;
        }
        ocr::Engine engine;
        if (!engine.Init(eopt, &localErr)) {
            fail("模型加载失败: " + localErr);
            return;
        }
        for (;;) {
            const int i = next.fetch_add(1);
            if (i >= n || failed.load()) return;
            ocr::Image img;
            std::string e1;
            if (!wdoc.RenderPage(pages[i], opt.dpi, &img, &e1)) {
                fail("渲染第 " + std::to_string(pages[i] + 1) + " 页失败: " + e1);
                return;
            }
            std::string e2;
            std::vector<ocr::TextLine> lines = engine.Recognize(img, &e2);
            {
                std::lock_guard<std::mutex> lk(mtx);
                results[i].lines = std::move(lines);
                results[i].width = img.w;
                results[i].height = img.h;
                results[i].done = true;
            }
            cv.notify_all();
        }
    };

    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(threadCount));
    for (int i = 0; i < threadCount; ++i) pool.emplace_back(worker);

    bool ok = true;
    int donePages = 0;
    int linesTotal = 0;
    for (int i = 0; i < n && ok; ++i) {
        {
            std::unique_lock<std::mutex> lk(mtx);
            cv.wait(lk, [&] { return results[i].done || failed.load(); });
        }
        if (failed.load()) {
            ok = false;
            break;
        }
        if (!layer.AddPage(pages[i], results[i].lines, &message)) {
            if (err) *err = "写入第 " + std::to_string(pages[i] + 1) + " 页文字层失败: " + message;
            ok = false;
            break;
        }
        if (stats) {
            OcrPageLines pl;
            pl.page = pages[i];
            pl.width = results[i].width;
            pl.height = results[i].height;
            pl.lines = results[i].lines;
            stats->pageLines.push_back(std::move(pl));
        }
        ++donePages;
        linesTotal += static_cast<int>(results[i].lines.size());
        if (progress) progress(donePages, n, pages[i], static_cast<int>(results[i].lines.size()));
        std::vector<ocr::TextLine>().swap(results[i].lines);
    }

    failed = true;
    cv.notify_all();
    for (auto& t : pool) t.join();

    if (!ok) {
        if (err && err->empty()) *err = workerError.empty() ? "OCR 处理中断" : workerError;
        return false;
    }
    const auto ts0 = std::chrono::steady_clock::now();
    const bool saved = layer.Save(opt.outputPath, &message);
    const double saveSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - ts0).count();
    if (!saved) {
        if (err) *err = "保存 PDF 失败: " + message;
        return false;
    }

    if (stats) {
        stats->pageCount = donePages;
        stats->lineCount = linesTotal;
        stats->maskedFonts = layer.MaskedFonts();
        stats->maskedCodes = layer.MaskedCodes();
        stats->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        stats->maskSeconds = maskSeconds;
        stats->saveSeconds = saveSeconds;
    }
    return true;
}

}  // namespace pdf
