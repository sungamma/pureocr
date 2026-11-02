#include "cli.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <io.h>
#include <windows.h>

#include "ocr/engine.h"
#include "ocr/image.h"

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
              const std::string &pageSpec, bool maskOriginal) {
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
        outPath.replace_filename(inPath.stem().u8string() + u8"_可复制文字" + inPath.extension().u8string());
    }

    pdf::OcrRunOptions opt;
    opt.inputPath = pdfPath;
    opt.outputPath = outPath.u8string();
    opt.modelDir = modelDir;
    opt.fontPath = fontPath;
    opt.dpi = dpi;
    opt.threads = threads > 0 ? threads : 4;
    opt.minScore = minScore;
    opt.maskOriginal = maskOriginal;
    if (!pageSpec.empty() && !pdf::ParsePageSpec(pageSpec, &opt.pages)) {
        std::fprintf(stderr, "页码参数无效: %s\n", pageSpec.c_str());
        return 2;
    }

    std::printf("输入: %s\n", pdfPath.c_str());
    std::printf("输出: %s\n", opt.outputPath.c_str());
    std::printf("DPI %g，线程 %d，最低置信度 %.2f\n", opt.dpi, opt.threads, opt.minScore);
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
    (void)t0;
    return 0;
#else
    (void)pdfPath; (void)outPdf; (void)modelDir; (void)fontPath; (void)dpi; (void)threads;
    (void)minScore; (void)pageSpec; (void)maskOriginal;
    std::fprintf(stderr, "本版本未编译 MuPDF 支持，无法处理 PDF\n");
    return 1;
#endif
}

}  // namespace

int RunCli(int argc, char **argv) {
    std::string imagePath, pdfPath, outPdf, modelDir, outPath, dumpDir, fontPath, pageSpec, logPath;
    float minScore = 0.5f;
    int threads = 0;
    double dpi = 300.0;
    bool maskOriginal = true;

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
        } else if (a == "--threads" && i + 1 < argc) {
            threads = std::stoi(argv[++i]);
        } else if (a == "--help" || a == "-h") {
            std::printf("用法:\n");
            std::printf("  ocrgui --pdf <输入.pdf> [--out-pdf <输出.pdf>] [--dpi 300] [--threads 4]\n");
            std::printf("         [--pages 1-10,15] [--font <ttf>] [--no-mask] [--min-score 0.5]\n");
            std::printf("         [--model-dir <模型目录>] [--log <日志文件>]\n");
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

    if (!pdfPath.empty()) {
        return RunPdfCli(pdfPath, outPdf, modelDir, fontPath, dpi, threads, minScore, pageSpec,
                         maskOriginal);
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
        FILE *fp = std::fopen(outPath.c_str(), "wb");
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
