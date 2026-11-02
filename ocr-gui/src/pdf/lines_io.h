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
///   # ocrgui-cache v1 dpi=300
///   p <页号> <宽> <高>
///   l <x0> <y0> <x1> <y1> <score> <文字>
/// 追加写，崩溃后最多丢半行；载入时按 dpi 校验，不符则整体忽略。
class LineCacheWriter {
public:
    LineCacheWriter() = default;
    ~LineCacheWriter();
    LineCacheWriter(const LineCacheWriter&) = delete;
    LineCacheWriter& operator=(const LineCacheWriter&) = delete;

    bool Open(const std::string& path, double dpi, std::string* err);
    void Append(const OcrPageLines& page);
    void Close();
    bool IsOpen() const { return f_ != nullptr; }

private:
    std::FILE* f_ = nullptr;
};

/// 载入缓存；同页出现多次时以最后一条为准。dpi 不符返回空（视为无缓存）。
bool LoadLineCache(const std::string& path, double dpi, std::vector<OcrPageLines>* pages);

}  // namespace pdf
