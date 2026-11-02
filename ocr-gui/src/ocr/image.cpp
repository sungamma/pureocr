#include "ocr/image.h"

#include <algorithm>
#include <cmath>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace ocr {

bool LoadImageFile(const std::string &path, Image &out) {
    int w = 0, h = 0, c = 0;
    unsigned char *px = stbi_load(path.c_str(), &w, &h, &c, 3);  // 强制转 RGB
    if (!px) return false;
    out.w = w;
    out.h = h;
    out.data.assign(px, px + static_cast<size_t>(w) * h * 3);
    stbi_image_free(px);
    return true;
}

Image Resize(const Image &src, int newW, int newH) {
    Image dst;
    if (src.empty() || newW <= 0 || newH <= 0) return dst;
    dst.w = newW;
    dst.h = newH;
    dst.data.resize(static_cast<size_t>(newW) * newH * 3);

    const float sx = static_cast<float>(src.w) / newW;
    const float sy = static_cast<float>(src.h) / newH;
    for (int y = 0; y < newH; ++y) {
        float fy = (y + 0.5f) * sy - 0.5f;
        if (fy < 0) fy = 0;
        int y0 = static_cast<int>(fy);
        int y1 = std::min(y0 + 1, src.h - 1);
        float wy = fy - y0;
        for (int x = 0; x < newW; ++x) {
            float fx = (x + 0.5f) * sx - 0.5f;
            if (fx < 0) fx = 0;
            int x0 = static_cast<int>(fx);
            int x1 = std::min(x0 + 1, src.w - 1);
            float wx = fx - x0;
            const uint8_t *p00 = src.at(x0, y0);
            const uint8_t *p10 = src.at(x1, y0);
            const uint8_t *p01 = src.at(x0, y1);
            const uint8_t *p11 = src.at(x1, y1);
            uint8_t *d = dst.at(x, y);
            for (int k = 0; k < 3; ++k) {
                float v = (p00[k] * (1 - wx) + p10[k] * wx) * (1 - wy) +
                          (p01[k] * (1 - wx) + p11[k] * wx) * wy;
                d[k] = static_cast<uint8_t>(std::lround(std::min(255.0f, std::max(0.0f, v))));
            }
        }
    }
    return dst;
}

Image Crop(const Image &src, int x0, int y0, int x1, int y1) {
    Image dst;
    x0 = std::max(0, std::min(x0, src.w - 1));
    y0 = std::max(0, std::min(y0, src.h - 1));
    x1 = std::max(x0 + 1, std::min(x1, src.w));
    y1 = std::max(y0 + 1, std::min(y1, src.h));
    dst.w = x1 - x0;
    dst.h = y1 - y0;
    dst.data.resize(static_cast<size_t>(dst.w) * dst.h * 3);
    for (int y = 0; y < dst.h; ++y) {
        std::copy(src.at(x0, y0 + y), src.at(x0, y0 + y) + dst.w * 3, dst.at(0, y));
    }
    return dst;
}

Image Rotate180(const Image &src) {
    Image dst;
    dst.w = src.w;
    dst.h = src.h;
    dst.data.resize(src.data.size());
    for (int y = 0; y < src.h; ++y) {
        for (int x = 0; x < src.w; ++x) {
            const uint8_t *s = src.at(src.w - 1 - x, src.h - 1 - y);
            uint8_t *d = dst.at(x, y);
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
        }
    }
    return dst;
}

}  // namespace ocr
