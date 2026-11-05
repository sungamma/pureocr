#include "ocr/engine.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include <onnxruntime_cxx_api.h>

#include "util/utf8_io.h"
#include "util/platform.h"

namespace ocr {
namespace {

using Shape = std::vector<int64_t>;

// 与 rapidocr 的 NormalizeImage 一致
constexpr float kMean[3] = {0.485f, 0.456f, 0.406f};
constexpr float kStd[3] = {0.229f, 0.224f, 0.225f};

#ifdef _WIN32
using PathStr = std::wstring;
inline PathStr ToPath(const std::string &s) { return util::Utf8ToWide(s); }
inline std::string FromPath(const PathStr &p) { return util::WideToUtf8(p.c_str()); }
inline const wchar_t *PathCStr(const PathStr &p) { return p.c_str(); }
#define JOIN_SEP L"/"
#else
using PathStr = std::string;
inline PathStr ToPath(const std::string &s) { return s; }
inline std::string FromPath(const PathStr &p) { return p; }
inline const char *PathCStr(const PathStr &p) { return p.c_str(); }
#define JOIN_SEP "/"
#endif

inline bool IsAbsolutePath(const std::string &s) {
    if (s.empty()) return false;
#ifdef _WIN32
    if (s.size() > 1 && s[1] == ':') return true;                       // C:\ 或 C:/
    if (s.size() > 1 && (s[0] == '\\' || s[0] == '/') &&
        (s[1] == '\\' || s[1] == '/')) return true;                     // UNC
    return false;
#else
    return s[0] == '/';
#endif
}

inline bool FileExists(const PathStr &p) { return util::PathExists(FromPath(p)); }

struct Pt {
    float x = 0, y = 0;
};

float Cross(const Pt &o, const Pt &a, const Pt &b) {
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

std::vector<Pt> ConvexHull(std::vector<Pt> pts) {
    std::sort(pts.begin(), pts.end(), [](const Pt &a, const Pt &b) {
        return a.x < b.x || (a.x == b.x && a.y < b.y);
    });
    pts.erase(std::unique(pts.begin(), pts.end(),
                          [](const Pt &a, const Pt &b) { return a.x == b.x && a.y == b.y; }),
              pts.end());
    if (pts.size() < 3) return pts;
    std::vector<Pt> h(2 * pts.size());
    size_t k = 0;
    for (size_t i = 0; i < pts.size(); ++i) {
        while (k >= 2 && Cross(h[k - 2], h[k - 1], pts[i]) <= 0) --k;
        h[k++] = pts[i];
    }
    for (size_t i = pts.size() - 1, t = k + 1; i > 0; --i) {
        while (k >= t && Cross(h[k - 2], h[k - 1], pts[i - 1]) <= 0) --k;
        h[k++] = pts[i - 1];
    }
    h.resize(k - 1);
    return h;
}

struct RotRect {
    float cx = 0, cy = 0, w = 0, h = 0, angle = 0;
    bool valid = false;
};

/// 旋转卡壳：最小面积外接矩形
RotRect MinAreaRect(const std::vector<Pt> &hull) {
    RotRect best;
    if (hull.size() < 3) return best;
    float bestArea = 1e30f;
    for (size_t i = 0; i < hull.size(); ++i) {
        const Pt &p = hull[i];
        const Pt &q = hull[(i + 1) % hull.size()];
        float ex = q.x - p.x, ey = q.y - p.y;
        const float len = std::sqrt(ex * ex + ey * ey);
        if (len < 1e-6f) continue;
        ex /= len;
        ey /= len;
        const float nx = -ey, ny = ex;
        float minU = 1e30f, maxU = -1e30f, minV = 1e30f, maxV = -1e30f;
        for (const Pt &r : hull) {
            const float u = r.x * ex + r.y * ey;
            const float v = r.x * nx + r.y * ny;
            minU = std::min(minU, u);
            maxU = std::max(maxU, u);
            minV = std::min(minV, v);
            maxV = std::max(maxV, v);
        }
        const float w = maxU - minU, h = maxV - minV;
        if (w * h < bestArea) {
            bestArea = w * h;
            const float cu = (minU + maxU) * 0.5f, cv = (minV + maxV) * 0.5f;
            best.cx = cu * ex + cv * nx;
            best.cy = cu * ey + cv * ny;
            best.w = w;
            best.h = h;
            best.angle = std::atan2(ey, ex);
            best.valid = true;
        }
    }
    return best;
}

struct Model {
    std::unique_ptr<Ort::Session> sess;
    std::string inName;
    std::string outName;
};

struct DetBox {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // 轴对齐框（对外使用）
    float quad[4][2] = {};                  // 四角 tl,tr,br,bl（裁剪用）
    float score = 0;
};

/// 对齐 PaddleOCR get_mini_boxes 的角点排序：按 x 排序后依 y 关系定 tl,tr,br,bl
void OrderQuad(float q[4][2]) {
    int idx[4] = {0, 1, 2, 3};
    std::stable_sort(idx, idx + 4, [&](int a, int b) { return q[a][0] < q[b][0]; });
    const int i1 = (q[idx[1]][1] > q[idx[0]][1]) ? idx[0] : idx[1];
    const int i4 = (q[idx[1]][1] > q[idx[0]][1]) ? idx[1] : idx[0];
    const int i2 = (q[idx[3]][1] > q[idx[2]][1]) ? idx[2] : idx[3];
    const int i3 = (q[idx[3]][1] > q[idx[2]][1]) ? idx[3] : idx[2];
    const float out[4][2] = {{q[i1][0], q[i1][1]}, {q[i2][0], q[i2][1]},
                             {q[i3][0], q[i3][1]}, {q[i4][0], q[i4][1]}};
    for (int k = 0; k < 4; ++k) { q[k][0] = out[k][0]; q[k][1] = out[k][1]; }
}

/// 合并同一行的相邻碎片：垂直重叠 ≥ 0.6 倍行高、水平间隙 ≤ gapRatio 倍行高即合并；
/// 间隙大于 spaceRatio 倍行高时补一个空格（如 `B.1` + `概述` → `B.1 概述`）。
/// 纯数字（只有数字、小数点、千分位逗号），这类片段在图表/表格里常是两个独立的标签
bool IsPlainNumber(const std::string &s) {
    bool hasDigit = false;
    for (char c : s) {
        if (c >= '0' && c <= '9') {
            hasDigit = true;
        } else if (c != '.' && c != ',' && c != '-') {
            return false;
        }
    }
    return hasDigit;
}

void MergeSameRow(std::vector<TextLine> *lines, float gapRatio, float spaceRatio) {
    if (!lines || lines->size() < 2 || gapRatio <= 0.0f) return;
    std::vector<TextLine> &L = *lines;
    std::vector<size_t> order(L.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const float ca = (L[a].y0 + L[a].y1) * 0.5f;
        const float cb = (L[b].y0 + L[b].y1) * 0.5f;
        const float ha = std::max(1.0f, L[a].y1 - L[a].y0);
        if (std::fabs(ca - cb) > 0.5f * ha) return ca < cb;
        return L[a].x0 < L[b].x0;
    });
    std::vector<char> used(L.size(), 0);
    std::vector<TextLine> out;
    out.reserve(L.size());
    for (size_t oi = 0; oi < order.size(); ++oi) {
        const size_t i = order[oi];
        if (used[i]) continue;
        used[i] = 1;
        TextLine acc = L[i];
        for (;;) {
            size_t best = static_cast<size_t>(-1);
            float bestGap = 0.0f;
            const float ha = std::max(1.0f, acc.y1 - acc.y0);
            for (size_t oj = oi + 1; oj < order.size(); ++oj) {
                const size_t j = order[oj];
                if (used[j]) continue;
                const TextLine &b = L[j];
                const float h = std::min(ha, std::max(1.0f, b.y1 - b.y0));
                const float ov = std::min(acc.y1, b.y1) - std::max(acc.y0, b.y0);
                if (ov < 0.6f * h) continue;
                const float gap = b.x0 - acc.x1;
                if (gap < -0.5f * h || gap > gapRatio * h) continue;
                // 两个纯数字（图表轴标签、表格数字单元格）绝不合并，避免 `1.05`+`1.02`
                if (IsPlainNumber(acc.text) && IsPlainNumber(b.text)) continue;
                if (best == static_cast<size_t>(-1) || b.x0 < L[best].x0) {
                    best = j;
                    bestGap = gap;
                }
            }
            if (best == static_cast<size_t>(-1)) break;
            const TextLine &b = L[best];
            const float h = std::min(ha, std::max(1.0f, b.y1 - b.y0));
            const float wa = std::max(1.0f, acc.x1 - acc.x0);
            const float wb = std::max(1.0f, b.x1 - b.x0);
            if (bestGap > spaceRatio * h && !acc.text.empty()) acc.text += ' ';
            acc.text += b.text;
            acc.score = (acc.score * wa + b.score * wb) / (wa + wb);
            acc.x0 = std::min(acc.x0, b.x0);
            acc.y0 = std::min(acc.y0, b.y0);
            acc.x1 = std::max(acc.x1, b.x1);
            acc.y1 = std::max(acc.y1, b.y1);
            used[best] = 1;
        }
        out.push_back(acc);
    }
    L.swap(out);
}

PathStr BaseName(const PathStr &p) {
    const size_t slash = p.find_last_of(ToPath("/\\"));
    return slash == PathStr::npos ? p : p.substr(slash + 1);
}

std::string BaseNameUtf8(const PathStr &p) { return FromPath(BaseName(p)); }

/// 解析模型路径：显式名可为绝对路径或 modelDir 下的文件名；未指定则按候选顺序自动探测
bool ResolveModel(const PathStr &dir, const std::string &spec,
                  std::initializer_list<const char *> candidates, const char *what,
                  PathStr *out, std::string *err) {
    if (!spec.empty()) {
        const PathStr w = ToPath(spec);
        const bool abs = IsAbsolutePath(spec);
        const PathStr p = abs ? w : (dir + JOIN_SEP + w);
        if (!FileExists(p)) {
            if (err) *err = std::string("找不到 ") + what + " 模型: " + spec;
            return false;
        }
        *out = p;
        return true;
    }
    std::string tried;
    for (const char *c : candidates) {
        const PathStr p = dir + JOIN_SEP + ToPath(c);
        if (FileExists(p)) {
            *out = p;
            return true;
        }
        if (!tried.empty()) tried += ", ";
        tried += c;
    }
    if (err) {
        *err = std::string("找不到 ") + what + " 模型（已尝试: " + tried + "，目录: " +
               FromPath(dir) + "）";
    }
    return false;
}

/// 读词典文件（UTF-8 可带 BOM），按 Paddle 约定补末尾空格
bool LoadKeysFile(const PathStr &path, std::vector<std::string> *chars) {
    FILE *f = util::OpenUtf8(FromPath(path), L"rb");
    if (!f) return false;
    std::string all;
    char buf[8192];
    size_t rd = 0;
    while ((rd = std::fread(buf, 1, sizeof(buf), f)) > 0) all.append(buf, rd);
    std::fclose(f);
    if (all.size() >= 3 && static_cast<unsigned char>(all[0]) == 0xEF &&
        static_cast<unsigned char>(all[1]) == 0xBB && static_cast<unsigned char>(all[2]) == 0xBF) {
        all.erase(0, 3);
    }
    chars->clear();
    size_t start = 0;
    while (start <= all.size()) {
        const size_t nl = all.find('\n', start);
        std::string line =
            all.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        while (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) chars->push_back(line);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    if (!chars->empty() && chars->back() != " ") chars->push_back(" ");
    return !chars->empty();
}

bool ResolveKeys(const PathStr &dir, const std::string &spec,
                 const std::string &recName, PathStr *out) {
    if (!spec.empty()) {
        const PathStr w = ToPath(spec);
        const bool abs = IsAbsolutePath(spec);
        const PathStr p = abs ? w : (dir + JOIN_SEP + w);
        if (!FileExists(p)) return false;
        *out = p;
        return true;
    }
    // v5 的字典和 v3/v4 完全不同，按 rec 模型名优先选对应字典
    const bool isV5 = recName.find("v5") != std::string::npos;
    const char *v5Cands[] = {"ppocrv5_dict.txt", "ppocr_keys_v1.txt"};
    const char *v34Cands[] = {"ppocr_keys_v1.txt", "ppocrv5_dict.txt"};
    const char **cands = isV5 ? v5Cands : v34Cands;
    for (size_t ci = 0; ci < 2; ++ci) {
        const char *c = cands[ci];
        const PathStr p = dir + JOIN_SEP + ToPath(c);
        if (FileExists(p)) {
            *out = p;
            return true;
        }
    }
    return false;
}


}  // namespace

struct Engine::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "ocrgui"};
    Model det, cls, rec;
    std::vector<std::string> chars;
    EngineOptions opt;

    bool Load(const PathStr &path, int threads, Model &m, std::string *err) {
        try {
            Ort::SessionOptions so;
            so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            int n = threads;
            if (n <= 0) {
                n = static_cast<int>(std::thread::hardware_concurrency());
                n = std::max(1, n / 2);
            }
            so.SetIntraOpNumThreads(n);
            so.SetInterOpNumThreads(1);
            m.sess = std::make_unique<Ort::Session>(env, PathCStr(path), so);
            Ort::AllocatorWithDefaultOptions alloc;
            m.inName = m.sess->GetInputNameAllocated(0, alloc).get();
            m.outName = m.sess->GetOutputNameAllocated(0, alloc).get();
            return true;
        } catch (const std::exception &e) {
            if (err) *err = e.what();
            return false;
        }
    }

    std::vector<float> Run(const Model &m, const std::vector<float> &in, const Shape &shape,
                           Shape *outShape) const {
        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value tensor = Ort::Value::CreateTensor<float>(
            mem, const_cast<float *>(in.data()), in.size(), shape.data(), shape.size());
        const char *inNames[] = {m.inName.c_str()};
        const char *outNames[] = {m.outName.c_str()};
        auto outs = m.sess->Run(Ort::RunOptions{nullptr}, inNames, &tensor, 1, outNames, 1);
        auto info = outs[0].GetTensorTypeAndShapeInfo();
        if (outShape) *outShape = info.GetShape();
        const float *p = outs[0].GetTensorData<float>();
        return std::vector<float>(p, p + info.GetElementCount());
    }

    /// 检测：返回原图坐标下的轴对齐文本框（上→下、左→右）
    std::vector<DetBox> Detect(const Image &img, std::string *err) {
        std::vector<DetBox> result;
        if (img.empty()) return result;

        // ---- resize（对齐 DetResizeForTest: limit_type=min）----
        const int h = img.h, w = img.w;
        float ratio = 1.f;
        if (std::min(h, w) < opt.limitSideLen) {
            ratio = static_cast<float>(opt.limitSideLen) / std::min(h, w);
        }
        int resizeH = static_cast<int>(h * ratio);
        int resizeW = static_cast<int>(w * ratio);
        resizeH = std::max(32, static_cast<int>(std::lround(resizeH / 32.0) * 32));
        resizeW = std::max(32, static_cast<int>(std::lround(resizeW / 32.0) * 32));

        Image resized = Resize(img, resizeW, resizeH);

        // ---- normalize + HWC->CHW ----
        std::vector<float> input(static_cast<size_t>(3) * resizeH * resizeW);
        for (int y = 0; y < resizeH; ++y) {
            const uint8_t *row = resized.at(0, y);
            for (int x = 0; x < resizeW; ++x) {
                for (int c = 0; c < 3; ++c) {
                    const float v = row[x * 3 + c] / 255.f;
                    input[(static_cast<size_t>(c) * resizeH + y) * resizeW + x] = (v - kMean[c]) / kStd[c];
                }
            }
        }

        Shape outShape;
        std::vector<float> prob;
        try {
            prob = Run(det, input, {1, 3, resizeH, resizeW}, &outShape);
        } catch (const std::exception &e) {
            if (err) *err = e.what();
            return result;
        }
        if (outShape.size() != 4) return result;
        const int ph = static_cast<int>(outShape[2]);
        const int pw = static_cast<int>(outShape[3]);

        // ---- 二值化 ----
        std::vector<uint8_t> mask(static_cast<size_t>(ph) * pw, 0);
        for (size_t i = 0; i < mask.size(); ++i) mask[i] = prob[i] > opt.detThresh ? 1 : 0;

        // ---- 膨胀（2x2，2 次，对齐 cv2.dilate + use_dilation）----
        if (opt.useDilation) {
            for (int it = 0; it < 2; ++it) {
                std::vector<uint8_t> dst = mask;
                for (int y = 0; y < ph; ++y) {
                    for (int x = 0; x < pw; ++x) {
                        if (!mask[static_cast<size_t>(y) * pw + x]) continue;
                        for (int dy = 0; dy < 2; ++dy)
                            for (int dx = 0; dx < 2; ++dx) {
                                const int ny = y + dy, nx = x + dx;
                                if (ny < ph && nx < pw) dst[static_cast<size_t>(ny) * pw + nx] = 1;
                            }
                    }
                }
                mask.swap(dst);
            }
        }

        // ---- 连通域 ----
        std::vector<int> label(static_cast<size_t>(ph) * pw, -1);
        std::vector<std::vector<int>> comps;
        std::vector<int> stack;
        for (int y = 0; y < ph; ++y) {
            for (int x = 0; x < pw; ++x) {
                const size_t idx = static_cast<size_t>(y) * pw + x;
                if (!mask[idx] || label[idx] >= 0) continue;
                const int id = static_cast<int>(comps.size());
                comps.emplace_back();
                stack.push_back(static_cast<int>(idx));
                label[idx] = id;
                while (!stack.empty()) {
                    const int cur = stack.back();
                    stack.pop_back();
                    comps.back().push_back(cur);
                    const int cy = cur / pw, cx = cur % pw;
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int ny = cy + dy, nx = cx + dx;
                            if (ny < 0 || ny >= ph || nx < 0 || nx >= pw) continue;
                            const size_t nidx = static_cast<size_t>(ny) * pw + nx;
                            if (mask[nidx] && label[nidx] < 0) {
                                label[nidx] = id;
                                stack.push_back(static_cast<int>(nidx));
                            }
                        }
                    }
                }
            }
        }

        // ---- 每个连通域 -> 最小外接矩形 -> 打分 -> 外扩 ----
        const float scaleX = static_cast<float>(w) / resizeW;
        const float scaleY = static_cast<float>(h) / resizeH;
        for (const auto &comp : comps) {
            if (static_cast<int>(comp.size()) < 9) continue;
            std::vector<Pt> pts;
            pts.reserve(comp.size());
            float scoreSum = 0;
            for (int idx : comp) {
                const int y = idx / pw, x = idx % pw;
                pts.push_back({static_cast<float>(x), static_cast<float>(y)});
                scoreSum += prob[idx];
            }
            const float score = scoreSum / comp.size();
            const std::vector<Pt> hull = ConvexHull(std::move(pts));
            const RotRect rr = MinAreaRect(hull);
            if (!rr.valid) continue;
            if (std::min(rr.w, rr.h) < 3.f) continue;

            // 打分对齐 box_score_fast：取「最小外接矩形覆盖区域」的平均概率，
            // 而不是笔画像素的平均概率（否则细长笔画会被误判成高置信度）
            float boxScore = score;
            {
                const float ca = std::cos(rr.angle), sa = std::sin(rr.angle);
                const float hw = rr.w * 0.5f, hh = rr.h * 0.5f;
                const float rad = std::sqrt(hw * hw + hh * hh);
                const int bx0 = std::max(0, static_cast<int>(rr.cx - rad));
                const int bx1 = std::min(pw - 1, static_cast<int>(rr.cx + rad));
                const int by0 = std::max(0, static_cast<int>(rr.cy - rad));
                const int by1 = std::min(ph - 1, static_cast<int>(rr.cy + rad));
                float sum = 0;
                int cnt = 0;
                for (int y = by0; y <= by1; ++y) {
                    for (int x = bx0; x <= bx1; ++x) {
                        const float dx = x - rr.cx, dy = y - rr.cy;
                        const float u = dx * ca + dy * sa;
                        const float v = -dx * sa + dy * ca;
                        if (std::fabs(u) <= hw && std::fabs(v) <= hh) {
                            sum += prob[static_cast<size_t>(y) * pw + x];
                            ++cnt;
                        }
                    }
                }
                if (cnt > 0) boxScore = sum / cnt;
            }
            (void)score;
            if (boxScore < opt.boxThresh) continue;

            // unclip：按面积/周长外扩（矩形等价于四边各外扩 d）
            float d = (rr.w * rr.h) * opt.unclipRatio / (2.f * (rr.w + rr.h));
            // pyclipper 的 polygon 面积/周长按取整后的四点算，略小于连续值，这里补偿 1px
            d = std::max(0.f, d - 1.f);
            const float ew = rr.w + 2 * d, eh = rr.h + 2 * d;

            const float ca = std::cos(rr.angle), sa = std::sin(rr.angle);
            float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
            const float hw = ew * 0.5f, hh = eh * 0.5f;
            const float corners[4][2] = {{-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}};
            for (const auto &c : corners) {
                const float px = rr.cx + c[0] * ca - c[1] * sa;
                const float py = rr.cy + c[0] * sa + c[1] * ca;
                minX = std::min(minX, px);
                maxX = std::max(maxX, px);
                minY = std::min(minY, py);
                maxY = std::max(maxY, py);
            }

            if (std::min(ew, eh) < 5.f) continue;  // 对齐 min_size + 2

            DetBox b;
            b.x0 = std::max(0.f, minX * scaleX);
            b.y0 = std::max(0.f, minY * scaleY);
            b.x1 = std::min(static_cast<float>(w), maxX * scaleX);
            b.y1 = std::min(static_cast<float>(h), maxY * scaleY);
            b.score = boxScore;
            if (b.x1 - b.x0 < 3 || b.y1 - b.y0 < 3) continue;
            // 四角按 tl,tr,br,bl 顺序（映射回原图坐标）
            const float local[4][2] = {{-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}};
            for (int k = 0; k < 4; ++k) {
                float px = rr.cx + local[k][0] * ca - local[k][1] * sa;
                float py = rr.cy + local[k][0] * sa + local[k][1] * ca;
                px = std::min(std::max(px * scaleX, 0.f), static_cast<float>(w) - 1);
                py = std::min(std::max(py * scaleY, 0.f), static_cast<float>(h) - 1);
                b.quad[k][0] = px;
                b.quad[k][1] = py;
            }
            OrderQuad(b.quad);
            result.push_back(b);
        }

        // 对齐 rapidocr 的 sorted_boxes：先按 (左上 y, 左上 x)，再做局部冒泡纠正
        std::stable_sort(result.begin(), result.end(), [](const DetBox &a, const DetBox &b) {
            if (a.y0 != b.y0) return a.y0 < b.y0;
            return a.x0 < b.x0;
        });
        for (size_t i = 0; i + 1 < result.size(); ++i) {
            for (size_t j = i + 1; j-- > 0;) {
                if (std::fabs(result[j + 1].y0 - result[j].y0) < 10.f &&
                    result[j + 1].x0 < result[j].x0) {
                    std::swap(result[j], result[j + 1]);
                } else {
                    break;
                }
            }
        }
        return result;
    }

    /// 方向分类：返回 true 表示需要旋转 180
    bool NeedRotate(const Image &crop, bool *ok) const {
        *ok = false;
        const int H = 48, W = 192;
        float r = static_cast<float>(crop.w) / crop.h;
        int rw = static_cast<int>(std::ceil(H * r));
        rw = std::min(rw, W);
        if (rw < 1) rw = 1;
        Image rs = Resize(crop, rw, H);
        std::vector<float> input(static_cast<size_t>(3) * H * W, 0.f);
        for (int y = 0; y < H; ++y) {
            const uint8_t *row = rs.at(0, y);
            for (int x = 0; x < rw; ++x) {
                for (int c = 0; c < 3; ++c) {
                    input[(static_cast<size_t>(c) * H + y) * W + x] =
                        (row[x * 3 + c] / 255.f - 0.5f) / 0.5f;
                }
            }
        }
        try {
            std::vector<float> out = Run(cls, input, {1, 3, H, W}, nullptr);
            *ok = true;
            return out.size() >= 2 && out[1] > 0.9f;
        } catch (...) {
            return false;
        }
    }

    /// 四点裁剪（对齐 get_rotate_crop_image：外接边缘给边框像素，高宽比 >=1.5 时转 90°）
    Image CropQuad(const Image &img, const DetBox &b) const {
        const float dxW = std::hypot(b.quad[0][0] - b.quad[1][0], b.quad[0][1] - b.quad[1][1]);
        const float dxW2 = std::hypot(b.quad[2][0] - b.quad[3][0], b.quad[2][1] - b.quad[3][1]);
        const float dxH = std::hypot(b.quad[0][0] - b.quad[3][0], b.quad[0][1] - b.quad[3][1]);
        const float dxH2 = std::hypot(b.quad[1][0] - b.quad[2][0], b.quad[1][1] - b.quad[2][1]);
        int cw = static_cast<int>(std::max(dxW, dxW2));
        int ch = static_cast<int>(std::max(dxH, dxH2));
        if (cw < 2 || ch < 2) return Image{};

        // 仿射映射：dest(0,0) -> quad[0]，dest 单位向量沿 quad[0]->quad[1]、quad[0]->quad[3]
        const float ux = (b.quad[1][0] - b.quad[0][0]) / std::max(1.f, dxW);
        const float uy = (b.quad[1][1] - b.quad[0][1]) / std::max(1.f, dxW);
        const float vx = (b.quad[3][0] - b.quad[0][0]) / std::max(1.f, dxH);
        const float vy = (b.quad[3][1] - b.quad[0][1]) / std::max(1.f, dxH);
        const float su = dxW / cw, sv = dxH / ch;

        Image dst;
        dst.w = cw;
        dst.h = ch;
        dst.data.resize(static_cast<size_t>(cw) * ch * 3);
        for (int y = 0; y < ch; ++y) {
            const float v = y * sv;
            for (int x = 0; x < cw; ++x) {
                const float u = x * su;
                const float sx = std::min(std::max(b.quad[0][0] + u * ux + v * vx, 0.f),
                                          static_cast<float>(img.w - 1));
                const float sy = std::min(std::max(b.quad[0][1] + u * uy + v * vy, 0.f),
                                          static_cast<float>(img.h - 1));
                const int x0 = static_cast<int>(sx), y0 = static_cast<int>(sy);
                const int x1 = std::min(x0 + 1, img.w - 1), y1 = std::min(y0 + 1, img.h - 1);
                const float wx = sx - x0, wy = sy - y0;
                const uint8_t *p00 = img.at(x0, y0);
                const uint8_t *p10 = img.at(x1, y0);
                const uint8_t *p01 = img.at(x0, y1);
                const uint8_t *p11 = img.at(x1, y1);
                uint8_t *d = dst.at(x, y);
                for (int c = 0; c < 3; ++c) {
                    const float val = (p00[c] * (1 - wx) + p10[c] * wx) * (1 - wy) +
                                      (p01[c] * (1 - wx) + p11[c] * wx) * wy;
                    d[c] = static_cast<uint8_t>(std::min(255.f, std::max(0.f, val + 0.5f)));
                }
            }
        }
        if (static_cast<float>(dst.h) / dst.w >= 1.5f) {  // 竖排文字 -> 逆时针转 90°
            Image rot;
            rot.w = dst.h;
            rot.h = dst.w;
            rot.data.resize(dst.data.size());
            for (int y = 0; y < dst.h; ++y) {
                for (int x = 0; x < dst.w; ++x) {
                    const uint8_t *sp = dst.at(x, y);
                    uint8_t *dp = rot.at(y, dst.w - 1 - x);
                    dp[0] = sp[0];
                    dp[1] = sp[1];
                    dp[2] = sp[2];
                }
            }
            return rot;
        }
        return dst;
    }

    /// 批量识别（对齐 rapidocr：按宽高比排序，每 6 张一批，批内取最大宽度）
    std::vector<std::pair<std::string, float>> RecogniseBatch(const std::vector<const Image *> &crops) {
        std::vector<std::pair<std::string, float>> res(crops.size(), {"", 0.f});
        if (crops.empty()) return res;
        const int H = 48;
        const int kBatch = 1;  // 每张独立算宽度，避免批内其它图影响结果

        std::vector<size_t> order(crops.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            const float ra = static_cast<float>(crops[a]->w) / crops[a]->h;
            const float rb = static_cast<float>(crops[b]->w) / crops[b]->h;
            return ra < rb;
        });

        for (size_t beg = 0; beg < order.size(); beg += kBatch) {
            const size_t endIdx = std::min(order.size(), beg + kBatch);
            float maxRatio = 0.f;
            for (size_t i = beg; i < endIdx; ++i) {
                const Image *c = crops[order[i]];
                maxRatio = std::max(maxRatio, static_cast<float>(c->w) / c->h);
            }
            int imgW = static_cast<int>(H * maxRatio);
            imgW = std::max(8, std::min(imgW, 4000));
            const size_t n = endIdx - beg;
            std::vector<float> input(static_cast<size_t>(3) * H * imgW * n, 0.f);
            for (size_t i = 0; i < n; ++i) {
                const Image *c = crops[order[beg + i]];
                float r = static_cast<float>(c->w) / c->h;
                int rw = static_cast<int>(std::ceil(H * r));
                rw = std::min(rw, imgW);
                if (rw < 1) rw = 1;
                Image rs = Resize(*c, rw, H);
                float *base = &input[i * 3 * H * imgW];
                for (int y = 0; y < H; ++y) {
                    const uint8_t *row = rs.at(0, y);
                    for (int x = 0; x < rw; ++x) {
                        for (int ch = 0; ch < 3; ++ch) {
                            base[(static_cast<size_t>(ch) * H + y) * imgW + x] =
                                (row[x * 3 + ch] / 255.f - 0.5f) / 0.5f;
                        }
                    }
                }
            }
            Shape outShape;
            std::vector<float> out;
            try {
                out = Run(rec, input, {static_cast<int64_t>(n), 3, H, imgW}, &outShape);
            } catch (...) {
                continue;
            }
            if (outShape.size() != 3) continue;
            const int T = static_cast<int>(outShape[1]);
            const int C = static_cast<int>(outShape[2]);
            for (size_t i = 0; i < n; ++i) {
                std::string text;
                float sum = 0;
                int cnt = 0, prev = -1;
                for (int t = 0; t < T; ++t) {
                    const float *row = &out[(i * T + t) * C];
                    int best = 0;
                    float bestP = row[0];
                    for (int c = 1; c < C; ++c) {
                        if (row[c] > bestP) {
                            bestP = row[c];
                            best = c;
                        }
                    }
                    if (best == 0) {
                        prev = -1;
                        continue;
                    }
                    if (best == prev) continue;
                    prev = best;
                    const int ci = best - 1;
                    if (ci >= 0 && ci < static_cast<int>(chars.size())) text += chars[ci];
                    sum += bestP;
                    ++cnt;
                }
                res[order[beg + i]] = {text, cnt ? sum / cnt : 0.f};
            }
        }
        return res;
    }
};

std::string EraseWhitespace(const std::string &s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

Engine::Engine() : d_(std::make_unique<Impl>()) {}
Engine::~Engine() = default;

const ModelPreset *ModelPresetList(int *count) {
    static const ModelPreset kPresets[] = {
        {"v5", "v5 移动版（最新）", "ch_PP-OCRv5_det_infer.onnx", "ch_PP-OCRv5_rec_infer.onnx",
         "ch_ppocr_mobile_v2.0_cls_infer.onnx"},
        {"v4", "v4 移动版", "ch_PP-OCRv4_det_infer.onnx", "ch_PP-OCRv4_rec_infer.onnx",
         "ch_ppocr_mobile_v2.0_cls_infer.onnx"},
        {"v3", "v3 移动版", "ch_PP-OCRv3_det_infer.onnx", "ch_PP-OCRv3_rec_infer.onnx",
         "ch_ppocr_mobile_v2.0_cls_infer.onnx"},
        {"v5s", "V5服务器版（最准、最慢）", "ch_PP-OCRv5_det_server_infer.onnx",
         "ch_PP-OCRv5_rec_server_infer.onnx", "ch_ppocr_mobile_v2.0_cls_infer.onnx"},
        {"v4s", "v4 服务器版（更准但慢）", "ch_PP-OCRv4_det_server_infer.onnx",
         "ch_PP-OCRv4_rec_server_infer.onnx", "ch_ppocr_mobile_v2.0_cls_infer.onnx"},
        {"auto", "自动探测（v4 优先）", "", "", "ch_ppocr_mobile_v2.0_cls_infer.onnx"},
    };
    if (count) *count = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));
    return kPresets;
}

std::string ModelTag(const EngineOptions &opt) {
    PathStr det, rec;
    std::string ignored;
    const PathStr dir = ToPath(opt.modelDir);
    if (!ResolveModel(dir, opt.detModel,
                      {"ch_PP-OCRv5_det_infer.onnx", "ch_PP-OCRv4_det_infer.onnx",
                       "ch_PP-OCRv3_det_infer.onnx"},
                      "det", &det, &ignored)) {
        det.clear();
    }
    if (!ResolveModel(dir, opt.recModel,
                      {"ch_PP-OCRv5_rec_infer.onnx", "ch_PP-OCRv4_rec_infer.onnx",
                       "ch_PP-OCRv3_rec_infer.onnx"},
                      "rec", &rec, &ignored)) {
        rec.clear();
    }
    return BaseNameUtf8(det) + "+" + BaseNameUtf8(rec);
}

bool Engine::Init(const EngineOptions &opt, std::string *err) {
    d_->opt = opt;
    const PathStr dir = ToPath(opt.modelDir);
    // 自动探测顺序：v4 优先、其次 v3；把模型文件丢进 modelDir 即可切换
    PathStr detPath, recPath, clsPath;
    if (!ResolveModel(dir, opt.detModel,
                      {"ch_PP-OCRv5_det_infer.onnx", "ch_PP-OCRv4_det_infer.onnx",
                       "ch_PP-OCRv3_det_infer.onnx"},
                      "det", &detPath, err)) {
        return false;
    }
    if (!ResolveModel(dir, opt.recModel,
                      {"ch_PP-OCRv5_rec_infer.onnx", "ch_PP-OCRv4_rec_infer.onnx",
                       "ch_PP-OCRv3_rec_infer.onnx"},
                      "rec", &recPath, err)) {
        return false;
    }
    if (!d_->Load(detPath, opt.threads, d_->det, err)) return false;
    if (!d_->Load(recPath, opt.threads, d_->rec, err)) return false;
    if (opt.useAngleCls) {
        std::string clsErr;
        if (ResolveModel(dir, opt.clsModel, {"ch_ppocr_mobile_v2.0_cls_infer.onnx"}, "cls",
                         &clsPath, &clsErr)) {
            if (!d_->Load(clsPath, opt.threads, d_->cls, err)) return false;
        } else {
            d_->opt.useAngleCls = false;  // 没有方向分类模型就跳过，不影响主流程
        }
    }

    // 识别字典直接取自 rec 模型的 metadata
    try {
        Ort::ModelMetadata meta = d_->rec.sess->GetModelMetadata();
        Ort::AllocatorWithDefaultOptions alloc;
        auto val = meta.LookupCustomMetadataMapAllocated("character", alloc);
        if (val) {
            const std::string chars(val.get());  // ORT metadata 本身就是 UTF-8
            size_t start = 0;
            while (start <= chars.size()) {
                const size_t nl = chars.find('\n', start);
                if (nl == std::string::npos) {
                    if (start < chars.size()) d_->chars.push_back(chars.substr(start));
                    break;
                }
                d_->chars.push_back(chars.substr(start, nl - start));
                start = nl + 1;
            }
        }
    } catch (...) {
    }
    // 回退：rec 模型没带 metadata 字典时读字典文件（Paddle 约定：blank + 字典 + 末尾空格）
    if (d_->chars.empty()) {
        PathStr keysPath;
        if (ResolveKeys(dir, opt.keysFile, BaseNameUtf8(recPath), &keysPath)) {
            LoadKeysFile(keysPath, &d_->chars);
        }
    }
    if (d_->chars.empty()) {
        if (err) *err = "识别字典为空（model metadata 中未找到 character）";
        return false;
    }
    return true;
}

std::vector<TextLine> Engine::Recognize(const Image &img, std::string *err) {
    std::vector<TextLine> lines;
    const std::vector<DetBox> boxes = d_->Detect(img, err);
    std::vector<DetBox> kept;
    std::vector<Image> crops;
    for (const DetBox &b : boxes) {
        const int x0 = static_cast<int>(std::floor(b.x0));
        const int y0 = static_cast<int>(std::floor(b.y0));
        const int x1 = static_cast<int>(std::ceil(b.x1));
        const int y1 = static_cast<int>(std::ceil(b.y1));
        if (x1 - x0 < 4 || y1 - y0 < 4) continue;
        Image crop = d_->CropQuad(img, b);
        if (crop.empty()) continue;
        bool clsOk = false;
        if (d_->opt.useAngleCls && d_->NeedRotate(crop, &clsOk)) crop = Rotate180(crop);
        kept.push_back(b);
        crops.push_back(std::move(crop));
    }

    if (!d_->opt.dumpCropsDir.empty()) {
        for (size_t i = 0; i < crops.size(); ++i) {
            char buf[512];
            std::snprintf(buf, sizeof(buf), "%s/crop_%03zu.png", d_->opt.dumpCropsDir.c_str(), i);
            SaveImagePng(crops[i], buf);
        }
    }

    std::vector<const Image *> ptrs;
    ptrs.reserve(crops.size());
    for (const Image &c : crops) ptrs.push_back(&c);
    std::vector<std::pair<std::string, float>> recRes;
    try {
        recRes = d_->RecogniseBatch(ptrs);
    } catch (const std::exception &e) {
        if (err) *err = e.what();
        return lines;
    }

    for (size_t i = 0; i < kept.size(); ++i) {
        const DetBox &b = kept[i];
        const std::pair<std::string, float> &r = recRes[i];
        const std::string text = EraseWhitespace(r.first);
        if (text.empty() || r.second < d_->opt.minScore) continue;
        TextLine tl;
        tl.x0 = b.x0;
        tl.y0 = b.y0;
        tl.x1 = b.x1;
        tl.y1 = b.y1;
        tl.text = text;
        tl.score = r.second;
        lines.push_back(tl);
    }
    // 同一行碎片合并（mergeGapRatio = 0 表示关闭）
    MergeSameRow(&lines, d_->opt.mergeGapRatio, 0.35f);
    return lines;
}

}  // namespace ocr
