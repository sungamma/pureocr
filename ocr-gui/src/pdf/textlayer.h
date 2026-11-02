#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "ocr/engine.h"

struct fz_context;
struct fz_font;
struct pdf_document;
struct pdf_obj;

namespace pdf {

class Document;

/// 隐形文字层参数
struct TextLayerOptions {
    std::string fontPath;      // CJK 字体文件（simhei.ttf 等）
    double dpi = 300.0;        // OCR 渲染分辨率，用于把像素坐标折算成 pt
};

/// 在原始 PDF 上叠加隐形（Tr 3）OCR 文字层。
/// 位置算法与 Python 版 build_text_layer_pdf 完全一致：
///   fontsize = box_h / em_height
///   baseline = y0 * scale - top_ratio * fontsize
///   横向 squeeze = box_w / 文本自然宽度
class TextLayer {
public:
    TextLayer();
    ~TextLayer();
    TextLayer(const TextLayer&) = delete;
    TextLayer& operator=(const TextLayer&) = delete;

    bool Init(Document& doc, const TextLayerOptions& opt, std::string* err);

    /// 把原文档已有字体（可能 ToUnicode 损坏）的字符全部映射成零宽字符，
    /// 这样复制出来的文字只包含 OCR 结果。
    bool MaskOriginalText(std::string* err);

    /// 为单页写入隐形文字层，lines 坐标为渲染图像素坐标
    bool AddPage(int pageIndex, const std::vector<ocr::TextLine>& lines, std::string* err);

    /// 子集化字体并保存
    bool Save(const std::string& outPath, std::string* err);

    int InsertedLines() const { return insertedLines_; }
    int MaskedFonts() const { return maskedFonts_; }
    int MaskedCodes() const { return maskedCodes_; }
    float TopRatio() const { return topRatio_; }
    float BottomRatio() const { return bottomRatio_; }
    float EmHeight() const { return emHeight_; }

private:
    int GlyphId(uint32_t codepoint);
    void MaskResources(pdf_obj* resources, int depth);
    void SubsetOcrFont();

    fz_context* ctx_ = nullptr;
    pdf_document* doc_ = nullptr;
    fz_font* font_ = nullptr;
    pdf_obj* fontRef_ = nullptr;

    float topRatio_ = -0.88f;
    float bottomRatio_ = 0.12f;
    float emHeight_ = 1.0f;
    double dpi_ = 300.0;
    float scale_ = 72.0f / 300.0f;

    int insertedLines_ = 0;
    int maskedFonts_ = 0;
    int maskedCodes_ = 0;

    std::unordered_map<uint32_t, int> gidCache_;
    std::vector<int> seenResources_;
    std::vector<int> seenFonts_;
};

}  // namespace pdf
