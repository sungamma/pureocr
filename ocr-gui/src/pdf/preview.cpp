#include "pdf/preview.h"

#include <cstddef>

#include "ocr/image.h"
#include "pdf/doc.h"

namespace pdf {

struct PagePreview::Impl {
    std::string path;
    Document doc;
};

PagePreview::PagePreview() : d_(new Impl) {}
PagePreview::~PagePreview() = default;

bool PagePreview::IsOpen() const { return d_ && d_->doc.IsOpen(); }

int PagePreview::PageCount() const { return IsOpen() ? d_->doc.PageCount() : 0; }

void PagePreview::Close() { d_.reset(new Impl); }

bool PagePreview::Open(const std::string& path, std::string* err) {
    if (d_ && d_->doc.IsOpen() && d_->path == path) return true;
    auto impl = std::unique_ptr<Impl>(new Impl);
    impl->path = path;
    if (!impl->doc.Open(path, err)) return false;
    d_ = std::move(impl);
    return true;
}

bool PagePreview::Render(int pageIndex, double dpi, std::vector<unsigned char>* rgba, int* w,
                         int* h, std::string* err) {
    if (!rgba || !w || !h) return false;
    if (!IsOpen()) {
        if (err) *err = "文档未打开";
        return false;
    }
    ocr::Image img;
    if (!d_->doc.RenderPage(pageIndex, dpi, &img, err)) return false;
    const size_t n = static_cast<size_t>(img.w) * static_cast<size_t>(img.h);
    rgba->resize(n * 4);
    unsigned char* dst = rgba->data();
    const unsigned char* src = img.data.data();
    for (size_t i = 0; i < n; ++i) {
        dst[i * 4 + 0] = src[i * 3 + 0];
        dst[i * 4 + 1] = src[i * 3 + 1];
        dst[i * 4 + 2] = src[i * 3 + 2];
        dst[i * 4 + 3] = 255;
    }
    *w = img.w;
    *h = img.h;
    return true;
}

}  // namespace pdf
