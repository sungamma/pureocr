#include "cli.h"
#include "util/utf8_io.h"

#include "util/path_util.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <io.h>
#include <windows.h>

#include "ocr/engine.h"
#include "ocr/image.h"

#include "util/model_download.h"

#ifdef OCRGUI_HAS_MUPDF
#include "pdf/doc.h"
#include "pdf/ocr_pages.h"
#endif

namespace {

std::wstring ToWide(const std::string &s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string ExeDir() {
    char exe[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir(exe);
    const size_t p = dir.find_last_of("\\/");
    return p == std::string::npos ? std::string(".") : dir.substr(0, p);
}

std::string DefaultModelDir() { return ExeDir() + "\\models"; }

std::string DefaultFont() {
    const char *cands[] = {u8"C:\\Windows\\Fonts\\simhei.ttf", u8"C:\\Windows\\Fonts\\simkai.ttf",
                           u8"C:\\Windows\\Fonts\\msyh.ttc", u8"C:\\Windows\\Fonts\\simsun.ttc"};
    for (const char *c : cands) {
        if (GetFileAttributesW(ToWide(c).c_str()) != INVALID_FILE_ATTRIBUTES) return c;
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
              const std::string &fixCsv) {
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
        outPath = fs::u8path(util::InsertBeforeExtension(pdfPath, "_可复制文字"));
    }

    pdf::OcrRunOptions opt;
    opt.inputPath = pdfPath;
    opt.outputPath = outPath.u8string();
    opt.modelDir = modelDir;
    opt.fontPath = fontPath;
    opt.dpi = dpi;
    opt.threads = threads > 0 ? threads : 4;
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
    std::printf("共 %d 页，写入 %d 行；屏蔽原字体 %d 个、字符编码 %d 个\n", stats.pageCount,
                stats.lineCount, stats.maskedFonts, stats.maskedCodes);
    std::printf("用时：%.1f 秒（其中屏蔽原文字层 %.1f 秒，保存 %.1f 秒）\n", stats.seconds,
                stats.maskSeconds, stats.saveSeconds);
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
int RunCli(int argc, char **argv) {
    std::string imagePath, pdfPath, outPdf, modelDir, outPath, dumpDir, fontPath, pageSpec, logPath,
        csvPath;
    float minScore = 0.5f;
    float reviewScore = 0.75f;
    int threads = 0;
    double dpi = 300.0;
    bool maskOriginal = true;
    bool resume = false;
    std::string cachePath;
    std::string preset, detModel, recModel, clsModel;
    float mergeGap = 0.8f;
    std::string fixCsv;
    bool fetchModels = false;
    std::string source = "auto";

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--image" && i + 1 < argc) {
            imagePath = argv[++i];
        } else if (a == "--pdf" && i + 1 < argc) {
            pdfPath = argv[++i];
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
        } else if (a == "--help" || a == "-h") {
            std::printf("用法:\n");
            std::printf("  ocrgui --pdf <输入.pdf> [--out-pdf <输出.pdf>] [--dpi 300] [--threads 4]\n");
            std::printf("         [--pages 1-10,15] [--font <ttf>] [--no-mask] [--min-score 0.5]\n");
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
        FILE *lf = nullptr;
        if (freopen_s(&lf, logPath.c_str(), "w", stdout) == 0 && lf) {
            _dup2(_fileno(stdout), _fileno(stderr));
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

    if (!pdfPath.empty()) {
        return RunPdfCli(pdfPath, outPdf, modelDir, fontPath, dpi, threads, minScore, pageSpec,
                         maskOriginal, csvPath, reviewScore, resume, cachePath, detModel,
                         recModel, clsModel, mergeGap, fixCsv);
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
    opt.modelDir = ToWide(modelDir);
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
