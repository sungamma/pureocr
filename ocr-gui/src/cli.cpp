#include "cli.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include <windows.h>

#include "ocr/engine.h"
#include "ocr/image.h"

namespace {

std::wstring ToWide(const std::string &s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
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

}  // namespace

int RunCli(int argc, char **argv) {
    std::string imagePath, modelDir, outPath, dumpDir;
    float minScore = 0.5f;
    int threads = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--image" && i + 1 < argc) {
            imagePath = argv[++i];
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
            std::printf("用法: ocrgui --image <图片> [--model-dir <目录>] [--min-score 0.5] [--threads N]\n");
            return 0;
        } else {
            std::fprintf(stderr, "未知参数: %s\n", a.c_str());
            return 2;
        }
    }
    if (imagePath.empty()) {
        std::fprintf(stderr, "缺少 --image 参数\n");
        return 2;
    }
    if (modelDir.empty()) {
        char exe[MAX_PATH] = {0};
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        std::string dir(exe);
        const size_t p = dir.find_last_of("\\/");
        modelDir = (p == std::string::npos ? std::string(".") : dir.substr(0, p)) + "\\models";
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
