#pragma once
#include <memory>
#include <string>
#include <vector>

namespace pdf {

/// 单页预览渲染（PDF -> RGBA），供 GUI 贴图使用。
/// 内部持有 MuPDF 文档句柄，路径不变时复用。
class PagePreview {
public:
    PagePreview();
    ~PagePreview();
    PagePreview(const PagePreview&) = delete;
    PagePreview& operator=(const PagePreview&) = delete;

    /// 打开 PDF；与上次路径相同且已打开时直接返回 true
    bool Open(const std::string& path, std::string* err);
    void Close();
    bool IsOpen() const;
    int PageCount() const;

    /// 渲染某页到 RGBA（每像素 4 字节，行优先）
    bool Render(int pageIndex, double dpi, std::vector<unsigned char>* rgba, int* w, int* h,
                std::string* err);

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}  // namespace pdf
