#pragma once
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

}  // namespace pdf
