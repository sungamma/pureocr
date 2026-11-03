#pragma once
#include <string>
#include <vector>

namespace pdf {

/// 成品自检结果
struct OutputCheck {
    int checked = 0;             // 实际比较过的页数
    std::vector<int> lostPages;  // 1 起页号：成品这一页内容明显比原页少（写回把页面搞坏了）
};

/// 保存后自检：把成品重新打开，逐页低分辨率渲染，和原 PDF 同页比"墨迹"占比。
/// 成品某页墨迹不到原页的 60% 就判定内容丢失。
/// 这样一来，"写回内容流出错 → 交付一页白纸"这种事不会再静默发生。
bool CheckOutput(const std::string& inputPath, const std::string& outputPath,
                 const std::vector<int>& pages, OutputCheck* out, std::string* err);

}  // namespace pdf
