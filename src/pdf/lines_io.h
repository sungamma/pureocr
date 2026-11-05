#pragma once
#include <cstdio>
#include <string>
#include <vector>

#include "pdf/ocr_pages.h"

namespace pdf {

/// 低于 reviewScore 的行数（人工复核用）
int CountLowScoreLines(const std::vector<OcrPageLines>& pages, float reviewScore);

/// 把识别结果写成 CSV（UTF-8 BOM，Excel 直接打开不乱码）。
/// 坐标为 pt（像素 * 72 / dpi）；reviewScore 决定「待复核」列。
bool WriteLinesCsv(const std::string& path, const std::vector<OcrPageLines>& pages, double dpi,
                   float reviewScore, std::string* err);

/// 页级识别结果缓存（断点续跑用），行文本格式：
///   # pureocr-cache v1 dpi=300
///   p <页号> <宽> <高>
///   l <x0> <y0> <x1> <y1> <score> <文字>
/// 追加写，崩溃后最多丢半行；载入时按 dpi 校验，不符则整体忽略。
/// 复核 CSV 中的一条文字修正（按 页号 + 页内行号 定位）
struct TextFix {
    int page = 0;      // 1 起的绝对页号（与导出的 CSV 一致）
    int line = 0;      // 1 起的页内行号
    std::string text;  // 修正后的文字
};

/// 读取复核 CSV（Excel 改完再存也能读）：至少要有 页/行/文字 三列，文字取最后一列
bool LoadTextFixes(const std::string& path, std::vector<TextFix>* fixes, std::string* err);

class LineCacheWriter {
public:
    LineCacheWriter() = default;
    ~LineCacheWriter();
    LineCacheWriter(const LineCacheWriter&) = delete;
    LineCacheWriter& operator=(const LineCacheWriter&) = delete;

    bool Open(const std::string& path, double dpi, const std::string& tag, std::string* err);
    void Append(const OcrPageLines& page);
    void Close();
    bool IsOpen() const { return f_ != nullptr; }

private:
    std::FILE* f_ = nullptr;
};

/// 载入缓存；同页出现多次时以最后一条为准。dpi 不符返回空（视为无缓存）。
bool LoadLineCache(const std::string& path, double dpi, const std::string& tag,
                   std::vector<OcrPageLines>* pages);

}  // namespace pdf
