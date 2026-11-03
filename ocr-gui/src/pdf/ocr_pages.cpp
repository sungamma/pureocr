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
#include "pdf/lines_io.h"
#include "pdf/output_check.h"
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
    bool fromCache = false;  // 结果来自页级缓存，无需再 OCR
};

/// 页级缓存默认位置：输出 PDF 同目录、同名前缀的 *_ocrcache.tsv
std::string DefaultCachePath(const std::string& outPdf) {
    const size_t slash = outPdf.find_last_of("\\/");
    const size_t dot = outPdf.find_last_of('.');
    const bool hasExt = dot != std::string::npos && (slash == std::string::npos || dot > slash);
    const std::string stem = hasExt ? outPdf.substr(0, dot) : outPdf;
    return stem + "_ocrcache.tsv";
}

/// 任务锁：独占打开 <输出>.lock，进程退出（含崩溃）由系统自动删除，
/// 防止两个进程同时写同一个输出文件。
class OutputLock {
public:
    ~OutputLock() {
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
    }
    bool Acquire(const std::string& outputPath, std::string* err) {
        const std::wstring path = ToWide(outputPath + ".lock");
        h_ = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) {
            if (err) *err = "已有任务正在处理同一输出（锁被占用）：" + outputPath;
            return false;
        }
        return true;
    }

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
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

    // 同一输出只允许一个任务，避免并发写坏文件
    OutputLock outputLock;
    if (!outputLock.Acquire(opt.outputPath, &message)) {
        if (err) *err = message;
        return false;
    }

    // 页级缓存路径（仅断点续跑时使用）
    std::string cachePath;
    std::string cacheTag;
    if (opt.resume) {
        cachePath = opt.cachePath.empty() ? DefaultCachePath(opt.outputPath) : opt.cachePath;
        ocr::EngineOptions tagOpt;
        tagOpt.modelDir = ToWide(opt.modelDir);
        tagOpt.detModel = opt.detModel;
        tagOpt.recModel = opt.recModel;
        cacheTag = ocr::ModelTag(tagOpt);
        {
            char mbuf[32];
            std::snprintf(mbuf, sizeof(mbuf), "|m=%.2f", static_cast<double>(opt.mergeGapRatio));
            cacheTag += mbuf;  // 合并策略变了，缓存也要作废
        }  // 换模型后缓存自动失效
    }

    TextLayer layer;
    TextLayerOptions topts;
    topts.fontPath = opt.fontPath;
    topts.dpi = opt.dpi;
    if (!layer.Init(doc, topts, &message)) {
        if (err) *err = message;
        return false;
    }
    // 屏蔽原文字体改的是「跨页共享的字体对象」，是最容易误伤全局的一步。
    // 约定：只在「本次处理了全部页」时才允许；部分页一律跳过，否则未处理页的
    // 可复制文本会被一起清成零宽字符（视觉看不出来，复制却是空白）。
    double maskSeconds = 0.0;
    const bool fullDocument = static_cast<int>(pages.size()) == pageCount;
    const bool doMask = opt.maskOriginal && fullDocument;
    if (opt.maskOriginal && !fullDocument && stats) stats->maskSkippedPartial = true;
    if (doMask) {
        const auto tm0 = std::chrono::steady_clock::now();
        const bool ok = layer.MaskOriginalText(pages, &message);
        maskSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - tm0).count();
        if (!ok) {
            if (err) *err = "屏蔽原文字层失败: " + message;
            return false;
        }
    }

    // 复核 CSV（人工改过的文字），识别后按 页/行 覆盖
    std::vector<TextFix> fixes;
    if (!opt.fixCsvPath.empty()) {
        if (!LoadTextFixes(opt.fixCsvPath, &fixes, &message)) {
            if (err) *err = message;
            return false;
        }
    }

    std::vector<PageResult> results(static_cast<size_t>(n));
    std::atomic<int> next{0};

    // 断点续跑：载入页级缓存，命中页直接复用（dpi 不一致视为无缓存）
    LineCacheWriter cache;
    int pendingOcr = n;
    if (opt.resume) {
        std::vector<OcrPageLines> cached;
        if (!LoadLineCache(cachePath, opt.dpi, cacheTag, &cached)) {
            // dpi 或模型与旧缓存不符：旧缓存作废，删掉后重写文件头
            std::remove(cachePath.c_str());
            cached.clear();
        }
        for (const auto& cp : cached) {
            for (int i = 0; i < n; ++i) {
                if (pages[i] != cp.page || results[i].done) continue;
                results[i].lines = cp.lines;
                results[i].width = cp.width;
                results[i].height = cp.height;
                results[i].done = true;
                results[i].fromCache = true;
                --pendingOcr;
                break;
            }
        }
        if (!cache.Open(cachePath, opt.dpi, cacheTag, &message)) {
            if (err) *err = message;
            return false;
        }
    }
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
        eopt.detModel = opt.detModel;
        eopt.recModel = opt.recModel;
        eopt.clsModel = opt.clsModel;
        eopt.mergeGapRatio = opt.mergeGapRatio;
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
            if (results[i].fromCache) continue;  // 命中缓存，跳过识别
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
    if (pendingOcr > 0) {  // 全部命中缓存时不启动 worker（省掉建模开销）
        pool.reserve(static_cast<size_t>(threadCount));
        for (int i = 0; i < threadCount; ++i) pool.emplace_back(worker);
    }

    bool ok = true;
    int donePages = 0;
    int cachedPages = 0;
    int ocrPages = 0;
    int fixedLines = 0;
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
        for (const auto& fx : fixes) {
            if (fx.page != pages[i] + 1) continue;
            const int idx = fx.line - 1;
            if (idx < 0 || idx >= static_cast<int>(results[i].lines.size())) continue;
            if (results[i].lines[idx].text != fx.text) {
                results[i].lines[idx].text = fx.text;
                ++fixedLines;
            }
        }
        if (!layer.AddPage(pages[i], results[i].lines, &message)) {
            if (err) *err = "写入第 " + std::to_string(pages[i] + 1) + " 页文字层失败: " + message;
            ok = false;
            break;
        }
        OcrPageLines pl;
        pl.page = pages[i];
        pl.width = results[i].width;
        pl.height = results[i].height;
        pl.lines = results[i].lines;
        if (results[i].fromCache) {
            ++cachedPages;
        } else {
            ++ocrPages;
            if (cache.IsOpen()) cache.Append(pl);  // 逐页落盘，随时可断点续跑
        }
        if (stats) stats->pageLines.push_back(std::move(pl));
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

    // 成品自检：重新打开输出，逐页和原 PDF 比"墨迹"。
    // 写回内容流一旦出错（例如 Contents 结构特殊），页面会整页变白但保存照样成功，
    // 用户只会看到"OCR 完什么都没有"。这里兜底：宁可报错，也不交付空壳 PDF。
    {
        OutputCheck check;
        std::string checkErr;
        // 没做屏蔽时，成品里的文字只会比原件多（OCR 文本是叠加的），字符数可以当判据；
        // 做了屏蔽时原文本来就被清成零宽字符，这条判据不适用。
        if (CheckOutput(opt.inputPath, opt.outputPath, pages, !doMask, &check, &checkErr)) {
            if (stats) {
                stats->verifiedPages = check.checked;
                stats->stackedPages = check.stackedPages;
            }
            const bool broken = !check.lostPages.empty() || !check.textLossPages.empty();
            if (broken) {
                std::vector<int> bad = check.lostPages;
                bad.insert(bad.end(), check.textLossPages.begin(), check.textLossPages.end());
                std::sort(bad.begin(), bad.end());
                std::string list;
                for (size_t k = 0; k < bad.size() && k < 8; ++k) {
                    if (!list.empty()) list += ",";
                    list += std::to_string(bad[k]);
                }
                if (bad.size() > 8) list += ",...";
                const std::wstring deleted = ToWide(opt.outputPath);
                DeleteFileW(deleted.c_str());  // 别把坏文件留在磁盘上冒充成功
                if (err) {
                    *err = "输出校验失败：第 " + list +
                           (check.lostPages.empty() ? " 页的文字提取被打断了" : " 页内容丢失") +
                           "（这个 PDF 的结构太特殊，写回被拒绝了）。已删除无效成品，"
                           "请把这页 PDF 反馈给开发者。";
                }
                return false;
            }
        } else if (stats) {
            stats->verifyNote = checkErr;  // 自检本身跑不起来不阻断，日志里能看到
        }
    }

    cache.Close();  // 缓存只是中间产物，完成后关闭

    if (!opt.csvPath.empty()) {
        if (!stats) {
            if (err) *err = "导出 CSV 需要 stats 输出";  // 内部调用约定
            return false;
        }
        const float review = opt.reviewScore > 0.0f ? opt.reviewScore : opt.minScore;
        if (!WriteLinesCsv(opt.csvPath, stats->pageLines, opt.dpi, review, &message)) {
            if (err) *err = message;
            return false;
        }
    }

    if (stats) {
        stats->reviewLines =
            CountLowScoreLines(stats->pageLines, opt.reviewScore > 0.0f ? opt.reviewScore : opt.minScore);
        stats->pageCount = donePages;
        stats->lineCount = linesTotal;
        stats->cachedPages = cachedPages;
        stats->ocrPages = ocrPages;
        stats->fixedLines = fixedLines;
        stats->maskedFonts = layer.MaskedFonts();
        stats->maskedCodes = layer.MaskedCodes();
        stats->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        stats->maskSeconds = maskSeconds;
        stats->saveSeconds = saveSeconds;
    }
    return true;
}

}  // namespace pdf
