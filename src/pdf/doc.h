#pragma once
#include <string>
#include <vector>

#include "ocr/image.h"

struct fz_context;
struct fz_document;
struct pdf_document;

namespace pdf {

/// PDF 书签（大纲）项
struct Bookmark {
    int level = 0;
    std::string title;
    int page = -1;  // 0 基页号，-1 表示无目标
};

/// 一个 PDF 文档（读 + 渲染）。非线程安全：每个线程各自持有一个。
class Document {
public:
    Document();
    ~Document();
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    /// path 为 UTF-8 编码的文件路径
    bool Open(const std::string& path, std::string* err);
    bool IsOpen() const { return fzDoc_ != nullptr; }

    int PageCount() const;
    /// 页面显示尺寸（pt），已考虑 /Rotate
    void PageSize(int index, float* w, float* h) const;
    /// 以 dpi 渲染整页到 RGB 图像
    bool RenderPage(int index, double dpi, ocr::Image* out, std::string* err);
    /// 读取书签树（展开成一维，level 为嵌套层级）
    std::vector<Bookmark> Outline();

    fz_context* Ctx() const { return ctx_; }
    fz_document* Fz() const { return fzDoc_; }
    pdf_document* Pdf() const { return pdf_; }

private:
    void Close();

    fz_context* ctx_ = nullptr;
    fz_document* fzDoc_ = nullptr;
    pdf_document* pdf_ = nullptr;
};

}  // namespace pdf
