#include "pdf/lines_io.h"

#include <cstdlib>
#include <cstring>

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

std::string EscapeField(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '\\': o += "\\\\"; break;
            case '\t': o += "\\t"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            default: o += c;
        }
    }
    return o;
}

std::string UnescapeField(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            const char n = s[++i];
            if (n == 't') o += '\t';
            else if (n == 'n') o += '\n';
            else if (n == 'r') o += '\r';
            else o += n;
        } else {
            o += s[i];
        }
    }
    return o;
}

/// 按分隔符切分（不合并连续分隔符，以保留空字段）
void SplitTabs(const std::string& line, std::vector<std::string>* out) {
    out->clear();
    size_t start = 0;
    for (;;) {
        const size_t tab = line.find('\t', start);
        if (tab == std::string::npos) {
            out->push_back(line.substr(start));
            return;
        }
        out->push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
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

// ---------------- 页级缓存 ----------------

LineCacheWriter::~LineCacheWriter() { Close(); }

bool LineCacheWriter::Open(const std::string& path, double dpi, const std::string& tag,
                           std::string* err) {
    Close();
    FILE* probe = nullptr;
    bool empty = true;
    if (fopen_s(&probe, path.c_str(), "rb") == 0 && probe) {
        std::fseek(probe, 0, SEEK_END);
        empty = std::ftell(probe) == 0;
        std::fclose(probe);
    }
    if (fopen_s(&f_, path.c_str(), "ab") != 0 || !f_) {
        if (err) *err = "无法写入缓存: " + path;
        return false;
    }
    if (empty) {
        std::fprintf(f_, "# ocrgui-cache v1 dpi=%g models=%s\n", dpi, tag.c_str());
        std::fflush(f_);
    }
    return true;
}

void LineCacheWriter::Append(const OcrPageLines& page) {
    if (!f_) return;
    std::fprintf(f_, "p\t%d\t%d\t%d\n", page.page, page.width, page.height);
    for (const auto& l : page.lines) {
        std::fprintf(f_, "l\t%.2f\t%.2f\t%.2f\t%.2f\t%.4f\t", static_cast<double>(l.x0),
                     static_cast<double>(l.y0), static_cast<double>(l.x1),
                     static_cast<double>(l.y1), static_cast<double>(l.score));
        std::fputs(EscapeField(l.text).c_str(), f_);
        std::fputs("\n", f_);
    }
    std::fflush(f_);  // 每页落盘：进程被强杀也不丢已完成页
}

void LineCacheWriter::Close() {
    if (f_) {
        std::fclose(f_);
        f_ = nullptr;
    }
}

bool LoadLineCache(const std::string& path, double dpi, const std::string& tag,
                   std::vector<OcrPageLines>* pages) {
    if (pages) pages->clear();
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return false;

    std::vector<std::string> fields;
    std::string text;
    text.reserve(4096);
    char buf[8192];
    OcrPageLines cur;
    bool havePage = false;
    bool dpiOk = true;
    std::string cacheTag;
    std::vector<OcrPageLines> acc;

    while (std::fgets(buf, sizeof(buf), f)) {
        std::string line(buf);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (line.empty()) continue;
        if (line[0] == '#') {
            const size_t p = line.find("dpi=");
            if (p != std::string::npos) {
                const double d = std::atof(line.c_str() + p + 4);
                if (d > 0 && dpi > 0 && std::abs(d - dpi) > 0.5) dpiOk = false;
            }
            const size_t m = line.find("models=");
            if (m != std::string::npos) cacheTag = line.c_str() + m + 7;
            continue;
        }
        SplitTabs(line, &fields);
        if (fields[0] == "p" && fields.size() >= 4) {
            if (havePage) acc.push_back(cur);
            cur = OcrPageLines();
            cur.page = std::atoi(fields[1].c_str());
            cur.width = std::atoi(fields[2].c_str());
            cur.height = std::atoi(fields[3].c_str());
            havePage = true;
        } else if (fields[0] == "l" && fields.size() >= 7 && havePage) {
            ocr::TextLine l;
            l.x0 = static_cast<float>(std::atof(fields[1].c_str()));
            l.y0 = static_cast<float>(std::atof(fields[2].c_str()));
            l.x1 = static_cast<float>(std::atof(fields[3].c_str()));
            l.y1 = static_cast<float>(std::atof(fields[4].c_str()));
            l.score = static_cast<float>(std::atof(fields[5].c_str()));
            l.text = UnescapeField(fields[6]);
            cur.lines.push_back(std::move(l));
        }
    }
    if (havePage) acc.push_back(cur);
    std::fclose(f);
    if (!dpiOk) return false;
    if (!tag.empty() && cacheTag != tag) return false;  // 换了模型，缓存作废  // dpi 变了，缓存不可用
    if (pages) *pages = std::move(acc);
    return true;
}

}  // namespace pdf
