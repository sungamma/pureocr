#pragma once
#include <memory>
#include <string>
#include <vector>

#include "ocr/image.h"

namespace ocr {

/// 一行识别结果（坐标为原图像素，轴对齐框）
struct TextLine {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    std::string text;
    float score = 0.f;
};

/// 与 rapidocr_onnxruntime 默认配置保持一致
struct EngineOptions {
    std::wstring modelDir;      // 含三个 .onnx 的目录
    std::string detModel;       // det 模型：modelDir 下的文件名，或绝对路径；空 = 自动探测（v4 优先）
    std::string recModel;       // rec 模型：同上
    std::string clsModel;       // 方向分类模型：同上；找不到会自动关闭该步骤
    std::string keysFile;       // rec 无 metadata 字典时用的字典文件；空 = 自动在 modelDir 找
    float detThresh = 0.3f;     // 二值化阈值
    float boxThresh = 0.5f;     // 文本框平均置信度阈值
    float unclipRatio = 1.6f;   // 文本框外扩比例
    int limitSideLen = 736;     // 检测输入短边下限
    bool useDilation = true;
    bool useAngleCls = true;
    float minScore = 0.5f;      // 识别结果过滤阈值
    /// 同一行相邻碎片合并阈值（倍数 × 行高）；0 = 不合并。
    /// v4/v5 的 det 常把一句话切成几段，用这个把它们拼回一行
    float mergeGapRatio = 0.8f;
    int threads = 0;            // 0 = 交给 ORT 默认
    std::string dumpCropsDir;   // 非空时把每张裁剪图存成 PNG（调试用）
};

class Engine {
public:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    bool Init(const EngineOptions& opt, std::string* err);
    /// 识别整页图像，结果按 上→下、左→右 排序
    std::vector<TextLine> Recognize(const Image& img, std::string* err);

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

/// 预置模型组合（det/rec/cls 文件名），GUI 下拉与 CLI --preset 共用
struct ModelPreset {
    const char* id;     // 命令行用：auto / v3 / v4 / v4s
    const char* label;  // 界面显示名
    const char* det;    // 空 = 交给自动探测，下同
    const char* rec;
    const char* cls;
};
const ModelPreset* ModelPresetList(int* count);

/// 实际加载的模型标识（det+rec 文件名），用于页级缓存失效判断等
std::string ModelTag(const EngineOptions& opt);

std::string EraseWhitespace(const std::string& s);

}  // namespace ocr
