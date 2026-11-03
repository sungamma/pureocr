#include "pdf/output_check.h"

#include <algorithm>
#include <cstdint>

#include "mupdf/fitz.h"

#include "ocr/image.h"
#include "pdf/doc.h"

namespace pdf {
namespace {

constexpr double kCheckDpi = 32.0;    // 32 dpi 足够看出「整页空白」，而且很快
constexpr double kLostRatio = 0.6;    // 成品墨迹 < 原页 60% 视为丢内容
constexpr double kMinInk = 0.0002;    // 原页本就几乎空白（墨迹 < 0.02%）的页不判定
constexpr int kMinChars = 20;         // 原页字符太少就不比字符数（避免误报）
constexpr double kTextKeep = 0.5;     // 成品字符数 < 原页 50% 视为提取链断了
constexpr double kStackedRatio = 1.05;  // 成品字符数 > 原页 105% 说明原文与 OCR 文本并存

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

/// 页面上「有效字符」个数：去掉零宽字符和空白。写回把文字链搞坏时这里会明显变小。
int TextCharCount(fz_context* ctx, fz_document* doc, int pageIndex) {
    fz_page* page = nullptr;
    fz_stext_page* text = nullptr;
    int count = 0;
    fz_try(ctx) {
        page = fz_load_page(ctx, doc, pageIndex);
        text = fz_new_stext_page_from_page(ctx, page, nullptr);
        for (fz_stext_block* b = text->first_block; b; b = b->next) {
            if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
            for (fz_stext_line* line = b->u.t.first_line; line; line = line->next) {
                for (fz_stext_char* ch = line->first_char; ch; ch = ch->next) {
                    const int c = ch->c;
                    if (c <= 0) continue;
                    if (c == 0x200B || c == 0xFEFF || c == 0x00A0) continue;
                    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
                    ++count;
                }
            }
        }
    }
    fz_always(ctx) {
        if (text) fz_drop_stext_page(ctx, text);
        if (page) fz_drop_page(ctx, page);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        return 0;
    }
    return count;
}

}  // namespace

bool CheckOutput(const std::string& inputPath, const std::string& outputPath,
                 const std::vector<int>& pages, bool textMustNotShrink, OutputCheck* out,
                 std::string* err) {
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
            ++out->checked;
            continue;  // 内容都没了，不用再比文字
        }
        if (textMustNotShrink) {
            const int charsBefore = TextCharCount(before.Ctx(), before.Fz(), p);
            if (charsBefore >= kMinChars) {
                const int charsAfter = TextCharCount(after.Ctx(), after.Fz(), p);
                if (charsAfter < static_cast<int>(charsBefore * kTextKeep)) {
                    out->textLossPages.push_back(p + 1);
                } else if (charsAfter > static_cast<int>(charsBefore * kStackedRatio)) {
                    ++out->stackedPages;  // 原页有字又叠了 OCR 文本：复制会得到两份
                }
            }
        }
        ++out->checked;
    }
    return true;
}

}  // namespace pdf
