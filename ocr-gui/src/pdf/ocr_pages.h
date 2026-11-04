#pragma once
#include <functional>
#include <string>
#include <vector>

#include "ocr/engine.h"

namespace pdf {

struct OcrRunOptions {
    std::string inputPath;   // UTF-8 输入 PDF
    std::string outputPath;  // UTF-8 输出 PDF
    std::string modelDir;    // UTF-8 模型目录
    std::string fontPath;    // UTF-8 CJK 字体（simhei.ttf 等）
    double dpi = 300.0;
    int threads = 4;
    float minScore = 0.5f;    // 低于此分数直接丢弃
    float reviewScore = 0.75f; // 低于此分数标记「待复核」（只影响标记，不过滤）
    std::string csvPath;
    std::string cachePath;     // 页级缓存（断点续跑用）；为空则跟随输出文件命名
    bool resume = false;       // 断点续跑：命中缓存页直接复用，只对未完成页跑 OCR       // 非空则导出逐行 CSV
    std::string detModel;      // det 模型文件名/绝对路径；空 = 自动探测（v4 优先）
    std::string recModel;      // rec 模型：同上
    std::string clsModel;      // 方向分类模型：同上
    float mergeGapRatio = 0.8f;  // 同行碎片合并阈值（× 行高）；0 = 关闭
    std::string fixCsvPath;      // 复核 CSV：识别后按 页/行 回填人工修正的文字
    // 屏蔽原文字体（复制/搜索只剩 OCR 文本）。默认关闭：这是全文最容易误伤的一步，
    // 它改的是跨页共享的字体对象；只有用户明确要求「用 OCR 文本替换原文」时才开。
    bool maskOriginal = false;  // 屏蔽原文字层，复制文字只输出 OCR 结果
    // 断点续跑缓存只在中断后有 value；整篇跑完默认删除，加 keepCache 才留（复核回灌流程要留）
    bool keepCache = false;
    std::vector<int> pages;    // 0 基页号；空表示全部
};

/// 一页的识别结果（像素坐标，宽高为渲染图像尺寸，用于归一化后画框）
struct OcrPageLines {
    int page = -1;
    int width = 0;
    int height = 0;
    std::vector<ocr::TextLine> lines;
};

struct OcrRunStats {
    int pageCount = 0;
    int lineCount = 0;
    int maskedFonts = 0;
    int maskedCodes = 0;
    double seconds = 0.0;
    double maskSeconds = 0.0;   // 屏蔽原文字层耗时
    double saveSeconds = 0.0;   // 子集化 + 保存耗时
    /// 逐页识别框（GUI 预览叠加用）；只在成功返回时填充
    std::vector<OcrPageLines> pageLines;
    int reviewLines = 0;
    int cachedPages = 0;  // 断点续跑命中缓存的页数
    int ocrPages = 0;     // 本次真正跑 OCR 的页数
    int fixedLines = 0;
    bool maskSkippedPartial = false;  // 开了屏蔽但只处理部分页，被自动跳过
    int stackedPages = 0;   // 原页本就可复制、成品里原文与 OCR 文本并存的页数
    int verifiedPages = 0;   // 成品自检通过（内容完整）的页数
    bool cacheRemoved = false;  // 整篇完成后已删除断点续跑缓存
    std::string verifyNote;   // 自检没能执行的原因（非空时说明这次没验证上）   // 复核 CSV 回填的行数  // 低于 reviewScore 的行数
};

/// 进度回调：done/total 页数，pageIndex 为刚写入文字层的页，lines 为该页行数
using OcrProgress = std::function<void(int done, int total, int pageIndex, int lines)>;

/// 解析页码规格："1-10,15,20-25" -> 0 基页号列表；空串表示全部
bool ParsePageSpec(const std::string& spec, std::vector<int>* pages);

/// 渲染 PDF 每页 -> OCR -> 写隐形文字层 -> 保存
bool RunPdfOcr(const OcrRunOptions& opt, const OcrProgress& progress, OcrRunStats* stats,
               std::string* err);

}  // namespace pdf
