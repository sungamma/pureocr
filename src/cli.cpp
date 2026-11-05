#include "cli.h"
#include "util/utf8_io.h"

#include "util/path_util.h"
#include "util/platform.h"
#include "version.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "ocr/engine.h"
#include "ocr/image.h"

#include "util/model_download.h"

#ifdef OCRGUI_HAS_MUPDF
#include "pdf/doc.h"
#include "pdf/ocr_pages.h"
#endif

namespace {

std::string DefaultModelDir() { return util::ExeDir() + "/models"; }

std::string DefaultFont() {
#ifdef _WIN32
    const char *cands[] = {u8"C:\\Windows\\Fonts\\simhei.ttf", u8"C:\\Windows\\Fonts\\simkai.ttf",
                           u8"C:\\Windows\\Fonts\\msyh.ttc", u8"C:\\Windows\\Fonts\\simsun.ttc"};
#else
    const char *cands[] = {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                           "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
                           "/usr/share/fonts/opentype/noto/NotoSerifCJK-Regular.ttc",
                           "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
                           "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"};
#endif
    for (const char *c : cands) {
        if (util::PathExists(c)) return c;
    }
    return "";
}

std::string JsonEscape(const std::string &s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    o += buf;
                } else {
                    o += static_cast<char>(c);
                }
        }
    }
    return o;
}

int RunPdfCli(const std::string &pdfPath, const std::string &outPdf, const std::string &modelDir,
              const std::string &fontPath, double dpi, int threads, float minScore,
              const std::string &pageSpec, bool maskOriginal, const std::string &csvPath,
              float reviewScore,
              bool resume, const std::string &cachePath, const std::string &detModel,
              const std::string &recModel, const std::string &clsModel, float mergeGap,
              const std::string &fixCsv, bool keepCache) {
#ifdef OCRGUI_HAS_MUPDF
    namespace fs = std::filesystem;
    if (fontPath.empty()) {
        std::fprintf(stderr, "找不到中文字体，请用 --font 指定 TTF/TTC\n");
        return 1;
    }

    fs::path inPath = fs::u8path(pdfPath);
    fs::path outPath;
    if (!outPdf.empty()) {
        outPath = fs::u8path(outPdf);
    } else {
        outPath = inPath;
        outPath = fs::u8path(util::InsertBeforeExtension(pdfPath, "_OCR"));
    }

    pdf::OcrRunOptions opt;
    opt.inputPath = pdfPath;
    opt.outputPath = outPath.u8string();
    opt.modelDir = modelDir;
    opt.fontPath = fontPath;
    opt.dpi = dpi;
    opt.threads = threads > 0 ? threads : 2;
    opt.minScore = minScore;
    opt.reviewScore = reviewScore;
    opt.csvPath = csvPath;
    opt.maskOriginal = maskOriginal;
    opt.resume = resume;
    opt.cachePath = cachePath;
    opt.detModel = detModel;
    opt.recModel = recModel;
    opt.clsModel = clsModel;
    opt.mergeGapRatio = mergeGap;
    opt.fixCsvPath = fixCsv;
    opt.keepCache = keepCache;
    if (!pageSpec.empty() && !pdf::ParsePageSpec(pageSpec, &opt.pages)) {
        std::fprintf(stderr, "页码参数无效: %s\n", pageSpec.c_str());
        return 2;
    }

    std::printf("输入: %s\n", pdfPath.c_str());
    std::printf("输出: %s\n", opt.outputPath.c_str());
    std::printf("DPI %g，线程 %d，最低置信度 %.2f\n", opt.dpi, opt.threads, opt.minScore);
    std::printf("模型: det=%s rec=%s\n", detModel.empty() ? "自动探测" : detModel.c_str(),
                recModel.empty() ? "自动探测" : recModel.c_str());
    std::fflush(stdout);

    const auto t0 = std::chrono::steady_clock::now();
    pdf::OcrRunStats stats;
    std::string err;
    const bool ok = pdf::RunPdfOcr(
        opt,
        [](int done, int total, int page, int lines) {
            std::printf("[%d/%d] 第 %d 页：%d 行\n", done, total, page + 1, lines);
            std::fflush(stdout);
        },
        &stats, &err);
    if (!ok) {
        std::fprintf(stderr, "失败: %s\n", err.c_str());
        return 1;
    }

    if (stats.stackedPages > 0) {
        std::printf("%d 页原文本来就能复制，成品里原文与 OCR 文本并存（复制会看到两份）；"
                    "想只保留 OCR 文本请加 --mask\n",
                    stats.stackedPages);
    }
    if (stats.maskSkippedPartial) {
        std::printf("部分页处理：已自动跳过 --mask（否则未处理页的文字会被一起清掉）\n");
    }
    if (!stats.verifyNote.empty()) {
        std::printf("注意：输出自检没能执行（%s）\n", stats.verifyNote.c_str());
    } else if (stats.verifiedPages > 0) {
        std::printf("输出自检：%d 页内容完整\n", stats.verifiedPages);
    }
    std::error_code ec1, ec2;
    const auto inSize = fs::file_size(inPath, ec1);
    const auto outSize = fs::file_size(outPath, ec2);
    std::printf("完成：%s\n", opt.outputPath.c_str());
    if (!ec1 && !ec2) {
        std::printf("原文件：%.2f MB　输出：%.2f MB\n", static_cast<double>(inSize) / 1048576.0,
                    static_cast<double>(outSize) / 1048576.0);
    }
    if (stats.maskedFonts > 0) {
        std::printf("共 %d 页，写入 %d 行；替换原文（屏蔽字体 %d 个、字符编码 %d 个）\n",
                    stats.pageCount, stats.lineCount, stats.maskedFonts, stats.maskedCodes);
    } else {
        std::printf("共 %d 页，写入 %d 行（原文保留，仅叠加隐形文字层）\n", stats.pageCount,
                    stats.lineCount);
    }
    if (stats.maskSeconds > 0.001) {
        std::printf("用时：%.1f 秒（其中屏蔽原文字层 %.1f 秒，保存 %.1f 秒）\n", stats.seconds,
                    stats.maskSeconds, stats.saveSeconds);
    } else {
        std::printf("用时：%.1f 秒（保存 %.1f 秒）\n", stats.seconds, stats.saveSeconds);
    }
    if (stats.cacheRemoved) {
        std::printf("断点续跑缓存已清理（下次对同一输出重跑会重新识别）\n");
    }
    std::printf("待复核：%d 行（置信度 < %.2f）\n", stats.reviewLines, opt.reviewScore);
    if (!csvPath.empty()) std::printf("复核 CSV：%s\n", csvPath.c_str());
    if (!fixCsv.empty() && stats.fixedLines > 0) {
        std::printf("复核回灌：应用了 %d 行文字修正\n", stats.fixedLines);
    }
    if (resume) {
        std::printf("断点续跑：缓存命中 %d 页 / 新识别 %d 页\n", stats.cachedPages, stats.ocrPages);
    }
    (void)t0;
    return 0;
#else
    (void)pdfPath; (void)outPdf; (void)modelDir; (void)fontPath; (void)dpi; (void)threads;
    (void)minScore; (void)pageSpec; (void)maskOriginal; (void)csvPath; (void)reviewScore;
    (void)resume; (void)cachePath; (void)detModel; (void)recModel; (void)clsModel;
    (void)mergeGap; (void)fixCsv;
    std::fprintf(stderr, "本版本未编译 MuPDF 支持，无法处理 PDF\n");
    return 1;
#endif
}

const ocr::ModelPreset *FindPreset(const std::string &id) {
    int n = 0;
    const ocr::ModelPreset *list = ocr::ModelPresetList(&n);
    for (int i = 0; i < n; ++i) {
        if (id == list[i].id) return &list[i];
    }
    return nullptr;
}

}  // namespace

// --fetch-models：把指定预设缺的模型下到 modelDir（缺省 v5 移动版）
int RunFetchModels(const std::string &modelDirIn, const std::string &presetId,
                   const std::string &source) {
    const ocr::ModelPreset *mp = FindPreset(presetId);
    if (!mp) {
        std::fprintf(stderr, "未知模型预设: %s（可选 auto / v3 / v4 / v4s / v5 / v5s）\n", presetId.c_str());
        return 2;
    }

    const std::string modelDir = util::ResolveModelDir(modelDirIn);
    if (modelDir != modelDirIn) {
        std::printf("模型目录不可写，改用 %s\n", modelDir.c_str());
    }

    std::vector<std::string> names;
    for (const char *f : {mp->det, mp->rec, mp->cls}) {
        if (f && *f) names.push_back(f);
    }
    const std::vector<util::ModelFile> missing = util::MissingForNames(modelDir, names);
    if (missing.empty()) {
        std::printf("模型已经齐全：%s\n", modelDir.c_str());
        return 0;
    }

    std::int64_t total = 0;
    for (const auto &m : missing) {
        total += m.size;
        std::printf("  缺 %-38s %9s\n", m.file.c_str(), util::FormatBytes(m.size).c_str());
    }
    std::printf("共 %zu 个文件，约 %s\n", missing.size(), util::FormatBytes(total).c_str());

    util::DownloadHandle handle;
    std::atomic<bool> stop{false};
    std::thread monitor([&handle, &stop]() {
        double lastPct = -100.0;
        while (!stop.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            const std::int64_t got = handle.received.load();
            const std::int64_t tot = handle.total.load();
            if (tot <= 0) continue;
            const double pct = 100.0 * static_cast<double>(got) / static_cast<double>(tot);
            if (pct - lastPct < 1.0) continue;
            lastPct = pct;
            std::printf("\r  %5.1f%%  %s / %s   ", pct, util::FormatBytes(got).c_str(),
                        util::FormatBytes(tot).c_str());
            std::fflush(stdout);
        }
    });

    const bool preferMirror = (source == "mirror");
    std::string err;
    const bool ok = util::FetchModels(modelDir, missing, preferMirror, &handle, &err);
    stop = true;
    monitor.join();
    std::printf("\n");

    if (!ok) {
        std::fprintf(stderr, "下载失败：%s\n", err.c_str());
        return 1;
    }
    std::printf("下载完成，模型目录：%s\n", modelDir.c_str());
    return 0;
}
/// 批量模式：把目录下所有 PDF 转成可搜索 PDF（<原名>_OCR.pdf）。
/// 跳过规则（避免反复跑）：文件名本身以 _OCR 结尾的；已有 <原名>_OCR.pdf 的原文件
/// （输出目录或源目录里存在都算）。--overwrite 可强制重做。
int RunPdfBatchCli(const std::string &folderIn, const std::string &outDirIn,
                   const std::string &modelDir, const std::string &fontPath, double dpi, int threads,
                   float minScore, const std::string &pageSpec, bool maskOriginal,
                   const std::string &csvPath, float reviewScore, bool resume,
                   const std::string &cachePath, const std::string &detModel,
                   const std::string &recModel, const std::string &clsModel, float mergeGap,
                   const std::string &fixCsv, bool recurse, bool overwrite, bool keepCache) {
#ifdef OCRGUI_HAS_MUPDF
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path folder = fs::u8path(folderIn);
    if (!fs::is_directory(folder, ec)) {
        std::fprintf(stderr, "目录不存在：%s\n", folderIn.c_str());
        return 2;
    }
    fs::path outRoot;
    if (!outDirIn.empty()) {
        outRoot = fs::u8path(outDirIn);
        fs::create_directories(outRoot, ec);
    }

    auto IsOcrOutput = [](const std::string &stem) {
        if (stem.size() < 4) return false;
        std::string tail = stem.substr(stem.size() - 4);
        for (char &c : tail) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return tail == "_ocr";
    };
    std::vector<fs::path> files;
    auto Collect = [&](const fs::path &p) {
        if (!fs::is_regular_file(p, ec)) return;
        std::string ext = p.extension().u8string();
        for (char &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext != ".pdf") return;
        if (IsOcrOutput(p.stem().u8string())) return;  // 产物自己，不处理
        files.push_back(p);
    };
    if (recurse) {
        fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end;
        while (!ec && it != end) {
            Collect(it->path());
            it.increment(ec);
        }
    } else {
        fs::directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end;
        while (!ec && it != end) {
            Collect(it->path());
            it.increment(ec);
        }
    }
    std::sort(files.begin(), files.end(),
              [](const fs::path &a, const fs::path &b) { return a.u8string() < b.u8string(); });

    std::printf("批量模式：%s（%zu 个 PDF%s）\n", folderIn.c_str(), files.size(),
                recurse ? "，已递归子目录" : "");
    std::printf("参数：%d 线程 / %g dpi / 最低置信度 %.2f / 同行合并阈值 %.2f\n", threads, dpi,
                static_cast<double>(minScore), static_cast<double>(mergeGap));
    if (files.empty()) {
        std::printf("没有可处理的 PDF（已跳过 *_OCR.pdf）。\n");
        return 0;
    }
    std::fflush(stdout);

    const auto t0 = std::chrono::steady_clock::now();
    int okCount = 0, skipCount = 0, failCount = 0;
    std::vector<std::string> failed, skipped;
    for (size_t i = 0; i < files.size(); ++i) {
        const fs::path &f = files[i];
        const std::string base = f.stem().u8string();
        const fs::path outPath = (outRoot.empty() ? f.parent_path() : outRoot) /
                                 fs::u8path(base + "_OCR.pdf");
        std::printf("\n[%zu/%zu] %s\n", i + 1, files.size(), f.filename().u8string().c_str());
        std::fflush(stdout);

        if (!overwrite) {
            std::string hit;
            if (fs::exists(outPath, ec)) {
                hit = outPath.filename().u8string();
            } else {
                const fs::path sibling = f.parent_path() / fs::u8path(base + "_OCR.pdf");
                if (sibling != outPath && fs::exists(sibling, ec)) hit = sibling.filename().u8string();
            }
            if (!hit.empty()) {
                std::printf("跳过：已有 %s（要重做加 --overwrite）\n", hit.c_str());
                ++skipCount;
                skipped.push_back(f.filename().u8string());
                continue;
            }
        }
        std::string csvForFile;
        if (!csvPath.empty()) {
            csvForFile = util::InsertBeforeExtension(outPath.u8string(), "_复核.csv");
        }
        const int rc = RunPdfCli(f.u8string(), outPath.u8string(), modelDir, fontPath, dpi, threads,
                                 minScore, pageSpec, maskOriginal, csvForFile, reviewScore, resume,
                                 cachePath, detModel, recModel, clsModel, mergeGap, fixCsv,
                                 keepCache);
        if (rc == 0) {
            ++okCount;
        } else {
            ++failCount;
            failed.push_back(f.filename().u8string());
        }
    }

    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("\n批量完成：成功 %d 个 / 跳过 %d 个 / 失败 %d 个，用时 %.1f 秒\n", okCount,
                skipCount, failCount, secs);
    if (!failed.empty()) {
        std::printf("失败的：");
        for (size_t i = 0; i < failed.size(); ++i) std::printf("%s%s", i ? "、" : "", failed[i].c_str());
        std::printf("\n");
        return 1;
    }
    return 0;
#else
    (void)folderIn; (void)outDirIn; (void)modelDir; (void)fontPath; (void)dpi; (void)threads;
    (void)minScore; (void)pageSpec; (void)maskOriginal; (void)csvPath; (void)reviewScore;
    (void)resume; (void)cachePath; (void)detModel; (void)recModel; (void)clsModel; (void)mergeGap;
    (void)fixCsv; (void)recurse; (void)overwrite; (void)keepCache;
    return 1;
#endif
}

int RunCli(int argc, char **argv) {
    std::string imagePath, pdfPath, outPdf, modelDir, outPath, dumpDir, fontPath, pageSpec, logPath,
        csvPath;
    float minScore = 0.55f;  // 默认置信度（配合 300dpi + 2 线程 + 合并 0.8 为推荐组合）
    float reviewScore = 0.75f;
    int threads = 2;  // 默认 2 线程：少占 CPU，笔记本上不拖慢别的活
    double dpi = 300.0;
    // 默认只叠加隐形文本层（不动原文）；--mask 才屏蔽原文字体
    bool maskOriginal = false;
    bool resume = false;
    std::string cachePath;
    std::string preset, detModel, recModel, clsModel;
    float mergeGap = 0.8f;
    std::string fixCsv;
    std::string dirPath;     // --dir：批量处理目录
    std::string outDirPath;  // --out-dir：批量输出目录（默认与源文件同目录）
    bool recurse = false;    // --recurse：含子目录
    bool overwrite = false;  // --overwrite：已有 _OCR 产物也重做
    bool keepCache = false;  // --keep-cache：跑完不删断点续跑缓存（复核回灌流程用）
    bool fetchModels = false;
    std::string source = "auto";

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--image" && i + 1 < argc) {
            imagePath = argv[++i];
        } else if (a == "--pdf" && i + 1 < argc) {
            pdfPath = argv[++i];
        } else if ((a == "--dir" || a == "-f") && i + 1 < argc) {
            dirPath = argv[++i];
        } else if (a == "--out-dir" && i + 1 < argc) {
            outDirPath = argv[++i];
        } else if (a == "--recurse" || a == "-r") {
            recurse = true;
        } else if (a == "--overwrite") {
            overwrite = true;
        } else if (a == "--keep-cache") {
            keepCache = true;
        } else if (a == "--out-pdf" && i + 1 < argc) {
            outPdf = argv[++i];
        } else if (a == "--dpi" && i + 1 < argc) {
            dpi = std::stod(argv[++i]);
        } else if (a == "--pages" && i + 1 < argc) {
            pageSpec = argv[++i];
        } else if (a == "--font" && i + 1 < argc) {
            fontPath = argv[++i];
        } else if (a == "--log" && i + 1 < argc) {
            logPath = argv[++i];
        } else if (a == "--mask") {
            maskOriginal = true;
        } else if (a == "--no-mask") {
            maskOriginal = false;
        } else if (a == "--model-dir" && i + 1 < argc) {
            modelDir = argv[++i];
        } else if (a == "--dump-crops" && i + 1 < argc) {
            dumpDir = argv[++i];
        } else if (a == "--out" && i + 1 < argc) {
            outPath = argv[++i];
        } else if (a == "--min-score" && i + 1 < argc) {
            minScore = std::stof(argv[++i]);
        } else if (a == "--review-score" && i + 1 < argc) {
            reviewScore = std::stof(argv[++i]);
        } else if (a == "--csv" && i + 1 < argc) {
            csvPath = argv[++i];
        } else if (a == "--resume") {
            resume = true;
        } else if (a == "--cache" && i + 1 < argc) {
            cachePath = argv[++i];
        } else if (a == "--preset" && i + 1 < argc) {
            preset = argv[++i];
        } else if (a == "--det-model" && i + 1 < argc) {
            detModel = argv[++i];
        } else if (a == "--rec-model" && i + 1 < argc) {
            recModel = argv[++i];
        } else if (a == "--cls-model" && i + 1 < argc) {
            clsModel = argv[++i];
        } else if (a == "--merge-gap" && i + 1 < argc) {
            mergeGap = std::stof(argv[++i]);
        } else if (a == "--fix-csv" && i + 1 < argc) {
            fixCsv = argv[++i];
        } else if (a == "--threads" && i + 1 < argc) {
            threads = std::stoi(argv[++i]);
        } else if (a == "--fetch-models") {
            fetchModels = true;
        } else if (a == "--source" && i + 1 < argc) {
            source = argv[++i];
        } else if (a == "--version" || a == "-V") {
            std::printf("ocrgui %s\n", OCRGUI_VERSION_STRING);
            return 0;
        } else if (a == "--help" || a == "-h") {
            std::printf("ocrgui %s\n\n", OCRGUI_VERSION_STRING);
            std::printf("用法:\n");
            std::printf("  ocrgui --pdf <输入.pdf> [--out-pdf <输出.pdf>] [--dpi 300] [--threads 2]\n");
            std::printf("         [--pages 1-10,15] [--font <ttf>] [--mask] [--min-score 0.55]\n");
            std::printf("         [--resume] 断点续跑（整篇跑完自动删缓存，--keep-cache 保留）\n");
            std::printf("  ocrgui --pdf <目录>   （也可 --dir / -f 指定目录）\n");
            std::printf("         [--out-dir <输出目录>] [--recurse|-r 含子目录] [--overwrite 重做已有结果]\n");
            std::printf("         批量：目录下所有 PDF 转成 <原名>_OCR.pdf；已有的自动跳过\n");
            std::printf("         （文件名以 _OCR 结尾的、或已经存在 <原名>_OCR.pdf 的原文件）\n");
            std::printf("         [--csv <复核.csv>] [--review-score 0.75]\n");
            std::printf("         [--model-dir <模型目录>] [--log <日志文件>]\n");
            std::printf("         [--resume] [--cache <缓存.tsv>]  断点续跑：已完成页跳过\n");
            std::printf("         [--preset auto|v3|v4|v4s|v5|v5s] [--det-model <文件名|绝对路径>]\n");
            std::printf("         [--rec-model <文件名|绝对路径>] [--cls-model <文件名>]\n");
            std::printf("         [--merge-gap 0.8]  同行碎片合并阈值（×行高，0=关闭）\n");
            std::printf("  ocrgui --fetch-models [--preset v5] [--model-dir <模型目录>]\n");
            std::printf("         [--source auto|official|mirror]  缺模型时联网下载\n");

            std::printf("  ocrgui --image <图片> [--out <json>] [--dump-crops <目录>] [--threads N]\n");
            return 0;
        } else {
            std::fprintf(stderr, "未知参数: %s\n", a.c_str());
            return 2;
        }
    }

    if (!logPath.empty()) {
        // 不能用 freopen_s：char* 路径按 ANSI 代码页解释，中文路径打不开；
        // 而且打开失败时 stdout 会被关掉，后续 printf 触发「无效参数」直接中止进程（闪退）。
        if (FILE *lf = util::OpenUtf8(logPath, L"w")) {
#ifdef _WIN32
            _dup2(_fileno(lf), _fileno(stdout));
            _dup2(_fileno(lf), _fileno(stderr));
#else
            ::dup2(::fileno(lf), ::fileno(stdout));
            ::dup2(::fileno(lf), ::fileno(stderr));
#endif
            std::fclose(lf);  // stdout/stderr 已持有复制出来的句柄
        } else {
            std::fprintf(stderr, "警告：日志文件无法写入（%s），本次只输出到控制台\n", logPath.c_str());
        }
    }
    if (modelDir.empty()) modelDir = DefaultModelDir();
    if (fontPath.empty()) fontPath = DefaultFont();
    if (!preset.empty()) {
        const ocr::ModelPreset *mp = FindPreset(preset);
        if (!mp) {
            std::fprintf(stderr, "未知模型预设: %s（可用 auto / v3 / v4 / v4s / v5 / v5s）\n", preset.c_str());
            return 2;
        }
        if (detModel.empty()) detModel = mp->det;
        if (recModel.empty()) recModel = mp->rec;
        if (clsModel.empty()) clsModel = mp->cls;
    }

    if (fetchModels) {
        return RunFetchModels(modelDir, preset.empty() ? "v5" : preset, source);
    }

    // 目录 -> 批量模式；单个文件 -> 原来的单文件流程
    std::string batchDir = dirPath;
    if (batchDir.empty() && !pdfPath.empty()) {
        std::error_code ec;
        if (std::filesystem::is_directory(std::filesystem::u8path(pdfPath), ec)) batchDir = pdfPath;
    }
    if (!batchDir.empty()) {
        if (!outPdf.empty()) {
            std::fprintf(stderr, "提示：批量模式下忽略 --out-pdf，用 --out-dir 指定输出目录\n");
        }
        return RunPdfBatchCli(batchDir, outDirPath, modelDir, fontPath, dpi, threads, minScore,
                              pageSpec, maskOriginal, csvPath, reviewScore, resume, cachePath,
                              detModel, recModel, clsModel, mergeGap, fixCsv, recurse, overwrite,
                              keepCache);
    }
    if (!pdfPath.empty()) {
        return RunPdfCli(pdfPath, outPdf, modelDir, fontPath, dpi, threads, minScore, pageSpec,
                         maskOriginal, csvPath, reviewScore, resume, cachePath, detModel,
                         recModel, clsModel, mergeGap, fixCsv, keepCache);
    }

    if (imagePath.empty()) {
        std::fprintf(stderr, "缺少 --image 或 --pdf 参数\n");
        return 2;
    }

    ocr::Image img;
    if (!ocr::LoadImageFile(imagePath, img)) {
        std::fprintf(stderr, "图片读取失败: %s\n", imagePath.c_str());
        return 1;
    }
    std::fprintf(stderr, "图片 %dx%d，模型目录 %s\n", img.w, img.h, modelDir.c_str());

    ocr::EngineOptions opt;
    opt.modelDir = modelDir;
    opt.minScore = minScore;
    opt.threads = threads;
    opt.dumpCropsDir = dumpDir;
    ocr::Engine engine;
    std::string err;
    const auto t0 = std::chrono::steady_clock::now();
    if (!engine.Init(opt, &err)) {
        std::fprintf(stderr, "模型加载失败: %s\n", err.c_str());
        return 1;
    }
    std::vector<ocr::TextLine> lines = engine.Recognize(img, &err);
    const auto t1 = std::chrono::steady_clock::now();
    const double sec = std::chrono::duration<double>(t1 - t0).count();
    if (!err.empty()) std::fprintf(stderr, "警告: %s\n", err.c_str());

    std::string json = "{\n  \"image\": \"" + JsonEscape(imagePath) + "\",\n  \"elapsed\": ";
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%.3f", sec);
    json += buf;
    json += ",\n  \"lines\": [\n";
    for (size_t i = 0; i < lines.size(); ++i) {
        const auto &l = lines[i];
        std::snprintf(buf, sizeof(buf),
                      "    {\"x0\": %.1f, \"y0\": %.1f, \"x1\": %.1f, \"y1\": %.1f, \"score\": %.4f, \"text\": \"",
                      l.x0, l.y0, l.x1, l.y1, l.score);
        json += buf;
        json += JsonEscape(l.text);
        json += "\"}";
        if (i + 1 < lines.size()) json += ",";
        json += "\n";
    }
    json += "  ]\n}\n";
    if (!outPath.empty()) {
        FILE *fp = util::OpenUtf8(outPath, L"wb");
        if (!fp) {
            std::fprintf(stderr, "无法写入 %s\n", outPath.c_str());
            return 1;
        }
        std::fwrite(json.data(), 1, json.size(), fp);
        std::fclose(fp);
    } else {
        std::fputs(json.c_str(), stdout);
        std::fflush(stdout);
    }
    std::fprintf(stderr, "识别完成：%zu 行，用时 %.2f 秒\n", lines.size(), sec);
    return 0;
}
