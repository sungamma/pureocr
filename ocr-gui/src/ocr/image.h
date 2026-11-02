#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ocr {

/// 简单 RGB 图像容器（行优先，每像素 3 字节）
struct Image {
    int w = 0;
    int h = 0;
    std::vector<uint8_t> data;  // size = w*h*3

    bool empty() const { return w <= 0 || h <= 0; }
    const uint8_t* at(int x, int y) const { return &data[(static_cast<size_t>(y) * w + x) * 3]; }
    uint8_t* at(int x, int y) { return &data[(static_cast<size_t>(y) * w + x) * 3]; }
};

/// 从磁盘读 PNG/JPG/BMP（依赖 stb_image）
bool LoadImageFile(const std::string &path, Image &out);

/// 双线性缩放（对齐 cv2.INTER_LINEAR 的常见行为）
Image Resize(const Image &src, int newW, int newH);

/// 裁剪（越界自动裁剪到图像范围内）
Image Crop(const Image &src, int x0, int y0, int x1, int y1);

/// 旋转 180 度
Image Rotate180(const Image &src);

}  // namespace ocr
