#include "ocr/engine.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include <onnxruntime_cxx_api.h>
#include <windows.h>

namespace ocr {
namespace {

using Shape = std::vector<int64_t>;

// 与 rapidocr 的 NormalizeImage 一致
constexpr float kMean[3] = {0.485f, 0.456f, 0.406f};
constexpr float kStd[3] = {0.229f, 0.224f, 0.225f};

std::wstring Utf8ToWide(const std::string &s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string WideToUtf8(const std::wstring &w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

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
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float score = 0;
};

}  // namespace

struct Engine::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "ocrgui"};
    Model det, cls, rec;
    std::vector<std::string> chars;
    EngineOptions opt;

    bool Load(const std::wstring &path, int threads, Model &m, std::string *err) {
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
            m.sess = std::make_unique<Ort::Session>(env, path.c_str(), so);
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
            const float d = (rr.w * rr.h) * opt.unclipRatio / (2.f * (rr.w + rr.h));
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

            DetBox b;
            b.x0 = std::max(0.f, minX * scaleX);
            b.y0 = std::max(0.f, minY * scaleY);
            b.x1 = std::min(static_cast<float>(w), maxX * scaleX);
            b.y1 = std::min(static_cast<float>(h), maxY * scaleY);
            b.score = boxScore;
            if (b.x1 - b.x0 < 3 || b.y1 - b.y0 < 3) continue;
            result.push_back(b);
        }

        std::sort(result.begin(), result.end(), [](const DetBox &a, const DetBox &b) {
            if (std::fabs(a.y0 - b.y0) > 10) return a.y0 < b.y0;
            return a.x0 < b.x0;
        });
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

    /// 批量识别（对齐 rapidocr：按宽高比排序，每 6 张一批，批内取最大宽度）
    std::vector<std::pair<std::string, float>> RecogniseBatch(const std::vector<const Image *> &crops) {
        std::vector<std::pair<std::string, float>> res(crops.size(), {"", 0.f});
        if (crops.empty()) return res;
        const int H = 48;
        const int kBatch = 6;

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

bool Engine::Init(const EngineOptions &opt, std::string *err) {
    d_->opt = opt;
    const std::wstring dir = opt.modelDir;
    if (!d_->Load(dir + L"/ch_PP-OCRv3_det_infer.onnx", opt.threads, d_->det, err)) return false;
    if (!d_->Load(dir + L"/ch_PP-OCRv3_rec_infer.onnx", opt.threads, d_->rec, err)) return false;
    if (opt.useAngleCls) {
        if (!d_->Load(dir + L"/ch_ppocr_mobile_v2.0_cls_infer.onnx", opt.threads, d_->cls, err))
            return false;
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
        Image crop = Crop(img, x0, y0, x1, y1);
        if (crop.empty()) continue;
        bool clsOk = false;
        if (d_->opt.useAngleCls && d_->NeedRotate(crop, &clsOk)) crop = Rotate180(crop);
        kept.push_back(b);
        crops.push_back(std::move(crop));
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
    return lines;
}

}  // namespace ocr
