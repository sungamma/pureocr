#include "pdf/lines_io.h"

#include <cstdio>

namespace pdf {
namespace {

std::string CsvField(const std::string& s) {
    if (s.find_first_of(",\"\r\n") == std::string::npos) return s;
    std::string o = "\"";
    for (char c : s) {
        if (c == '"') o += "\"\"";
        else o += c;
    }
    o += "\"";
    return o;
}

}  // namespace

int CountLowScoreLines(const std::vector<OcrPageLines>& pages, float reviewScore) {
    int n = 0;
    for (const auto& p : pages) {
        for (const auto& l : p.lines) {
            if (l.score < reviewScore) ++n;
        }
    }
    return n;
}

bool WriteLinesCsv(const std::string& path, const std::vector<OcrPageLines>& pages, double dpi,
                   float reviewScore, std::string* err) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) {
        if (err) *err = "无法写入 CSV: " + path;
        return false;
    }
    std::fputs("\xEF\xBB\xBF", f);  // UTF-8 BOM
    std::fputs("页,行,置信度,待复核,宽pt,高pt,左pt,上pt,右pt,下pt,文字\n", f);
    const double k = dpi > 0 ? 72.0 / dpi : 1.0;
    for (const auto& p : pages) {
        for (size_t i = 0; i < p.lines.size(); ++i) {
            const auto& l = p.lines[i];
            std::fprintf(f, "%d,%zu,%.4f,%d,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,", p.page + 1, i + 1,
                         static_cast<double>(l.score), l.score < reviewScore ? 1 : 0,
                         (l.x1 - l.x0) * k, (l.y1 - l.y0) * k, l.x0 * k, l.y0 * k, l.x1 * k,
                         l.y1 * k);
            std::fputs(CsvField(l.text).c_str(), f);
            std::fputs("\n", f);
        }
    }
    std::fclose(f);
    return true;
}

}  // namespace pdf
