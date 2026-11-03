#include "pdf/output_check.h"

#include <algorithm>
#include <cstdint>

#include "ocr/image.h"
#include "pdf/doc.h"

namespace pdf {
namespace {

constexpr double kCheckDpi = 32.0;   // 32 dpi 足够看出"整页空白"，而且很快
constexpr double kLostRatio = 0.6;   // 成品墨迹 < 原页 60% 视为丢内容
constexpr double kMinInk = 0.0002;   // 原页本就几乎空白（墨迹 < 0.02%）的页不判定

/// 深色像素占比（0~1）。整页内容被清掉时接近 0。
double InkRatio(const ocr::Image& img) {
    if (img.empty()) return 0.0;
    const size_t px = static_cast<size_t>(img.w) * static_cast<size_t>(img.h);
    size_t dark = 0;
    for (size_t i = 0; i < px; ++i) {
        const uint8_t* p = &img.data[i * 3];
        const int v = (static_cast<int>(p[0]) + p[1] + p[2]) / 3;
        if (v < 200) ++dark;
    }
    return static_cast<double>(dark) / static_cast<double>(px);
}

}  // namespace

bool CheckOutput(const std::string& inputPath, const std::string& outputPath,
                 const std::vector<int>& pages, OutputCheck* out, std::string* err) {
    if (!out) return false;
    *out = OutputCheck{};
    std::string message;
    Document before;
    Document after;
    if (!before.Open(inputPath, &message)) {
        if (err) *err = "自检打不开原 PDF: " + message;
        return false;
    }
    if (!after.Open(outputPath, &message)) {
        if (err) *err = "自检打不开成品 PDF: " + message;
        return false;
    }
    const int n = std::min(before.PageCount(), after.PageCount());
    for (int p : pages) {
        if (p < 0 || p >= n) continue;
        ocr::Image imgBefore;
        ocr::Image imgAfter;
        if (!before.RenderPage(p, kCheckDpi, &imgBefore, &message)) continue;
        if (!after.RenderPage(p, kCheckDpi, &imgAfter, &message)) {
            out->lostPages.push_back(p + 1);
            ++out->checked;
            continue;
        }
        const double inkBefore = InkRatio(imgBefore);
        const double inkAfter = InkRatio(imgAfter);
        if (inkBefore > kMinInk && inkAfter < inkBefore * kLostRatio) {
            out->lostPages.push_back(p + 1);
        }
        ++out->checked;
    }
    return true;
}

}  // namespace pdf
