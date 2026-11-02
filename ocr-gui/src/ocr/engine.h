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
    float detThresh = 0.3f;     // 二值化阈值
    float boxThresh = 0.5f;     // 文本框平均置信度阈值
    float unclipRatio = 1.6f;   // 文本框外扩比例
    int limitSideLen = 736;     // 检测输入短边下限
    bool useDilation = true;
    bool useAngleCls = true;
    float minScore = 0.5f;      // 识别结果过滤阈值
    int threads = 0;            // 0 = 交给 ORT 默认
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

std::string EraseWhitespace(const std::string& s);

}  // namespace ocr
