#pragma once
#include <functional>
#include <string>
#include <vector>

namespace pdf {

struct OcrRunOptions {
    std::string inputPath;   // UTF-8 输入 PDF
    std::string outputPath;  // UTF-8 输出 PDF
    std::string modelDir;    // UTF-8 模型目录
    std::string fontPath;    // UTF-8 CJK 字体（simhei.ttf 等）
    double dpi = 300.0;
    int threads = 4;
    float minScore = 0.5f;
    bool maskOriginal = true;  // 屏蔽原文字层，复制文字只输出 OCR 结果
    std::vector<int> pages;    // 0 基页号；空表示全部
};

struct OcrRunStats {
    int pageCount = 0;
    int lineCount = 0;
    int maskedFonts = 0;
    int maskedCodes = 0;
    double seconds = 0.0;
    double maskSeconds = 0.0;   // 屏蔽原文字层耗时
    double saveSeconds = 0.0;   // 子集化 + 保存耗时
};

/// 进度回调：done/total 页数，pageIndex 为刚写入文字层的页，lines 为该页行数
using OcrProgress = std::function<void(int done, int total, int pageIndex, int lines)>;

/// 渲染 PDF 每页 -> OCR -> 写隐形文字层 -> 保存
bool RunPdfOcr(const OcrRunOptions& opt, const OcrProgress& progress, OcrRunStats* stats,
               std::string* err);

}  // namespace pdf
