#pragma once
#include <string>
#include <vector>

namespace pdf {

/// 成品自检结果
struct OutputCheck {
    int checked = 0;              // 实际比较过的页数
    std::vector<int> lostPages;   // 1 起页号：成品这一页墨迹明显比原页少（整页内容丢了）
    std::vector<int> textLossPages;  // 1 起页号：墨迹还在，但可提取文字没了（提取链被破坏）
    int stackedPages = 0;         // 原页本就有可提取文字、成品里原文与 OCR 文本并存的页数
};

/// 保存后自检：把成品重新打开，逐页低分辨率渲染 + 提取文字，和原 PDF 同页比。
///  - 墨迹：成品低于原页 60% → 判该页内容丢失。
///  - 文字：textMustNotShrink 为真时（即本次没有屏蔽原文字体），成品字符数应
///    不少于原页；低于 50% 判该页提取链被打断。屏蔽场景下原文本来就被清成零宽
///    字符，这条判据没有意义，故由调用方关掉。
bool CheckOutput(const std::string& inputPath, const std::string& outputPath,
                 const std::vector<int>& pages, bool textMustNotShrink, OutputCheck* out,
                 std::string* err);

}  // namespace pdf
