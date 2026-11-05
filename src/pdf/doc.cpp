#include "pdf/doc.h"

#include <cstring>

#include "pdf/system_fonts.h"

#include "mupdf/fitz.h"
#include "mupdf/pdf.h"

namespace pdf {
namespace {

std::string CaughtMessage(fz_context* ctx, const std::exception* ex) {
    if (const char* m = fz_caught_message(ctx)) {
        if (m[0]) return m;
    }
    return ex ? ex->what() : std::string("未知错误");
}

void CollectOutline(fz_context* ctx, fz_document* doc, fz_outline* item, int level,
                    std::vector<Bookmark>* out) {
    for (; item; item = item->next) {
        Bookmark bm;
        bm.level = level;
        bm.title = item->title ? item->title : "";
        if (item->page.page >= 0) {
            bm.page = fz_page_number_from_location(ctx, doc, item->page);
        }
        out->push_back(bm);
        if (item->down) CollectOutline(ctx, doc, item->down, level + 1, out);
    }
}

}  // namespace

Document::Document() = default;

Document::~Document() { Close(); }

void Document::Close() {
    if (ctx_) {
        if (fzDoc_) fz_drop_document(ctx_, fzDoc_);
        fzDoc_ = nullptr;
        pdf_ = nullptr;
        fz_drop_context(ctx_);
        ctx_ = nullptr;
    }
}

bool Document::Open(const std::string& path, std::string* err) {
    Close();
    auto fail = [&](const std::string& msg) {
        if (err) *err = msg;
        Close();
        return false;
    };

    ctx_ = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx_) return fail("无法创建 MuPDF 上下文");
#if defined(OCR_MUPDF_SLIM)
    // slim 版剔除了内嵌 CJK 字体，回退字体改从 Windows 系统字体目录取
    InstallSystemFontHooks(ctx_);
#endif
    fz_try(ctx_) {
        fz_register_document_handlers(ctx_);
        fzDoc_ = fz_open_document(ctx_, path.c_str());
    }
    fz_catch(ctx_) {
        return fail(CaughtMessage(ctx_, nullptr));
    }
    if (!fzDoc_) return fail("无法打开文档: " + path);

    pdf_ = pdf_specifics(ctx_, fzDoc_);
    if (!pdf_) return fail("不是 PDF 文件（或为加密文档）: " + path);
    return true;
}

int Document::PageCount() const {
    if (!fzDoc_) return 0;
    int n = 0;
    fz_try(ctx_) { n = fz_count_pages(ctx_, fzDoc_); }
    fz_catch(ctx_) { n = 0; }
    return n;
}

void Document::PageSize(int index, float* w, float* h) const {
    if (w) *w = 0.f;
    if (h) *h = 0.f;
    if (!fzDoc_) return;
    fz_page* page = nullptr;
    fz_try(ctx_) {
        page = fz_load_page(ctx_, fzDoc_, index);
        const fz_rect r = fz_bound_page(ctx_, page);
        if (w) *w = r.x1 - r.x0;
        if (h) *h = r.y1 - r.y0;
    }
    fz_always(ctx_) {
        if (page) fz_drop_page(ctx_, page);
    }
    fz_catch(ctx_) {}
}

bool Document::RenderPage(int index, double dpi, ocr::Image* out, std::string* err) {
    if (!fzDoc_ || !out) {
        if (err) *err = "文档未打开";
        return false;
    }
    const float zoom = static_cast<float>(dpi / 72.0);
    fz_pixmap* pix = nullptr;
    fz_try(ctx_) {
        fz_matrix ctm = fz_scale(zoom, zoom);
        pix = fz_new_pixmap_from_page_number(ctx_, fzDoc_, index, ctm, fz_device_rgb(ctx_), 0);
    }
    fz_catch(ctx_) {
        if (err) *err = CaughtMessage(ctx_, nullptr);
        return false;
    }

    const int w = fz_pixmap_width(ctx_, pix);
    const int h = fz_pixmap_height(ctx_, pix);
    const int n = fz_pixmap_components(ctx_, pix);
    const ptrdiff_t stride = fz_pixmap_stride(ctx_, pix);
    const unsigned char* s = fz_pixmap_samples(ctx_, pix);

    out->w = w;
    out->h = h;
    out->data.resize(static_cast<size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y) {
        const unsigned char* src = s + static_cast<ptrdiff_t>(y) * stride;
        unsigned char* dst = out->data.data() + static_cast<size_t>(y) * w * 3;
        if (n >= 3) {
            for (int x = 0; x < w; ++x) {
                dst[x * 3 + 0] = src[x * n + 0];
                dst[x * 3 + 1] = src[x * n + 1];
                dst[x * 3 + 2] = src[x * n + 2];
            }
        } else {  // 灰度
            for (int x = 0; x < w; ++x) {
                dst[x * 3 + 0] = dst[x * 3 + 1] = dst[x * 3 + 2] = src[x * n];
            }
        }
    }
    fz_drop_pixmap(ctx_, pix);
    return true;
}

std::vector<Bookmark> Document::Outline() {
    std::vector<Bookmark> out;
    if (!fzDoc_) return out;
    fz_outline* root = nullptr;
    fz_try(ctx_) { root = fz_load_outline(ctx_, fzDoc_); }
    fz_catch(ctx_) {
        fz_report_error(ctx_);
        return out;
    }
    if (root) {
        fz_try(ctx_) { CollectOutline(ctx_, fzDoc_, root, 0, &out); }
        fz_catch(ctx_) { fz_report_error(ctx_); }
        fz_drop_outline(ctx_, root);
    }
    return out;
}

}  // namespace pdf
