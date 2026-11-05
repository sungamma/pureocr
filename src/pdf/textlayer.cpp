#include "pdf/textlayer.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>

#include "mupdf/fitz.h"
#include "mupdf/pdf.h"

#include "pdf/doc.h"

namespace pdf {
namespace {

constexpr uint32_t kZeroWidth = 0x200B;  // 零宽空格
constexpr float kSqueezeMin = 0.3f;      // 横向压缩下限（同 Python 版）
constexpr float kSqueezeMax = 3.0f;

void DecodeUtf8(const std::string& s, std::vector<uint32_t>* out) {
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp = 0xFFFD;
        int extra = 0;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            extra = 3;
        }
        ++i;
        bool ok = true;
        for (int k = 0; k < extra; ++k, ++i) {
            if (i >= n || (static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
        }
        out->push_back(ok ? cp : 0xFFFD);
    }
}

const char* FindSub(const char* p, const char* end, const char* needle) {
    const size_t len = std::strlen(needle);
    for (const char* q = p; q + len <= end; ++q) {
        if (std::memcmp(q, needle, len) == 0) return q;
    }
    return nullptr;
}

bool ReadHexToken(const char*& p, const char* end, std::string* digits) {
    digits->clear();
    while (p < end && std::isspace(static_cast<unsigned char>(*p))) ++p;
    if (p >= end || *p != '<') return false;
    ++p;
    while (p < end && *p != '>') {
        if (std::isxdigit(static_cast<unsigned char>(*p))) digits->push_back(*p);
        ++p;
    }
    if (p < end) ++p;  // 吃掉 '>'
    return !digits->empty();
}

struct CmapCodes {
    std::set<int> codes;
    int codeBytes = 0;
};

CmapCodes ParseToUnicodeCmap(const char* p, size_t len) {
    CmapCodes out;
    const char* end = p + len;
    auto note = [&](const std::string& digits) {
        const int bytes = static_cast<int>(digits.size() / 2);
        if (bytes <= 0) return;
        if (bytes > out.codeBytes) out.codeBytes = bytes;
        out.codes.insert(static_cast<int>(std::strtol(digits.c_str(), nullptr, 16)));
    };

    // beginbfchar ... endbfchar：<src> <dst> 成对出现
    for (const char* q = p;;) {
        const char* b = FindSub(q, end, "beginbfchar");
        if (!b) break;
        const char* e = FindSub(b, end, "endbfchar");
        if (!e) break;
        const char* t = b + 11;
        std::string d;
        int idx = 0;
        while (t < e && ReadHexToken(t, e, &d)) {
            if (idx % 2 == 0) note(d);
            ++idx;
        }
        q = e + 9;
    }

    // beginbfrange ... endbfrange：<lo> <hi> <dst> 或 <lo> <hi> [ ... ]
    for (const char* q = p;;) {
        const char* b = FindSub(q, end, "beginbfrange");
        if (!b) break;
        const char* e = FindSub(b, end, "endbfrange");
        if (!e) break;
        const char* t = b + 12;
        while (t < e) {
            std::string lo, hi, dst;
            if (!ReadHexToken(t, e, &lo)) break;
            if (!ReadHexToken(t, e, &hi)) break;
            while (t < e && std::isspace(static_cast<unsigned char>(*t))) ++t;
            if (t < e && *t == '[') {
                ++t;
                while (ReadHexToken(t, e, &dst)) {
                }
            } else {
                ReadHexToken(t, e, &dst);
            }
            const int bytes = static_cast<int>(lo.size() / 2);
            if (bytes > 0 && bytes > out.codeBytes) out.codeBytes = bytes;
            const long low = std::strtol(lo.c_str(), nullptr, 16);
            const long high = std::strtol(hi.c_str(), nullptr, 16);
            if (high >= low && high - low <= 4096) {
                for (long c = low; c <= high; ++c) out.codes.insert(static_cast<int>(c));
            }
        }
        q = e + 10;
    }
    return out;
}

std::string HexCode(int code, int codeBytes) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%0*X", codeBytes * 2, code);
    return buf;
}

std::string BuildZeroWidthCmap(const std::set<int>& codes, int codeBytes) {
    if (codeBytes <= 0) codeBytes = 1;
    std::string s;
    s += "/CIDInit /ProcSet findresource begin\n";
    s += "12 dict begin\n";
    s += "begincmap\n";
    s += "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n";
    s += "/CMapName /Adobe-Identity-UCS def\n";
    s += "/CMapType 2 def\n";
    s += "1 begincodespacerange\n";
    s += "<" + std::string(codeBytes * 2, '0') + "> <" + std::string(codeBytes * 2, 'F') + ">\n";
    s += "endcodespacerange\n";
    char target[16];
    std::snprintf(target, sizeof(target), "<%04X>", kZeroWidth);
    std::vector<std::string> entries;
    entries.reserve(codes.size());
    for (int code : codes) entries.push_back("<" + HexCode(code, codeBytes) + "> " + target);
    for (size_t start = 0; start < entries.size(); start += 100) {
        const size_t n = std::min<size_t>(100, entries.size() - start);
        char head[32];
        std::snprintf(head, sizeof(head), "%zu beginbfchar\n", n);
        s += head;
        for (size_t i = start; i < start + n; ++i) {
            s += entries[i];
            s += "\n";
        }
        s += "endbfchar\n";
    }
    s += "endcmap\n";
    s += "CMapName currentdict /CMap defineresource pop\n";
    s += "end\nend\n";
    return s;
}

pdf_obj* MakeStream(fz_context* ctx, pdf_document* doc, const std::string& text, int compress) {
    fz_buffer* buf = fz_new_buffer_from_copied_data(
        ctx, reinterpret_cast<const unsigned char*>(text.data()), text.size());
    pdf_obj* ref = nullptr;
    fz_try(ctx) { ref = pdf_add_stream(ctx, doc, buf, nullptr, compress); }
    fz_always(ctx) { fz_drop_buffer(ctx, buf); }
    fz_catch(ctx) { fz_rethrow(ctx); }
    return ref;
}

}  // namespace

TextLayer::TextLayer() = default;

TextLayer::~TextLayer() {
    if (ctx_ && font_) fz_drop_font(ctx_, font_);
    font_ = nullptr;
}

bool TextLayer::Init(Document& doc, const TextLayerOptions& opt, std::string* err) {
    ctx_ = doc.Ctx();
    doc_ = doc.Pdf();
    dpi_ = opt.dpi > 1.0 ? opt.dpi : 300.0;
    scale_ = static_cast<float>(72.0 / dpi_);
    if (!ctx_ || !doc_) {
        if (err) *err = "文档未打开";
        return false;
    }

    std::string message;
    fz_try(ctx_) {
        font_ = fz_new_font_from_file(ctx_, "OCR-CJK", opt.fontPath.c_str(), 0, 0);
        fontRef_ = pdf_add_cid_font(ctx_, doc_, font_);
    }
    fz_catch(ctx_) {
        message = fz_caught_message(ctx_);
        if (err) *err = "加载字体失败: " + message;
        return false;
    }

    // 测量 "国" 的字形墨迹盒（相对基线，单位 em），与 Python 版算法对齐
    fz_try(ctx_) {
        const int gid = fz_encode_character(ctx_, font_, 0x56FD);
        if (gid > 0) {
            const fz_rect r = fz_bound_glyph(ctx_, font_, gid, fz_identity);
            topRatio_ = r.y0;
            bottomRatio_ = r.y1;
        }
    }
    fz_catch(ctx_) { fz_report_error(ctx_); }
    // 兜底：MuPDF 的 glyph 空间偶有以 1/1000 em 表示的情况
    if (bottomRatio_ - topRatio_ > 2.0f) {
        topRatio_ /= 1000.0f;
        bottomRatio_ /= 1000.0f;
    }
    emHeight_ = bottomRatio_ - topRatio_;
    if (!(emHeight_ > 0.05f && emHeight_ < 3.0f)) {
        const float asc = fz_font_ascender(ctx_, font_);
        const float desc = fz_font_descender(ctx_, font_);
        topRatio_ = -asc;
        bottomRatio_ = -desc;
        emHeight_ = bottomRatio_ - topRatio_;
    }
    return true;
}

int TextLayer::GlyphId(uint32_t cp) {
    auto it = gidCache_.find(cp);
    if (it != gidCache_.end()) return it->second;
    int gid = 0;
    fz_try(ctx_) { gid = fz_encode_character(ctx_, font_, static_cast<int>(cp)); }
    fz_catch(ctx_) { gid = 0; }
    gidCache_.emplace(cp, gid);
    return gid;
}

void TextLayer::CollectFontNums(pdf_obj* resources, int depth, std::vector<int>* out,
                                std::vector<int>* seenRes) {
    if (!resources || depth > 8) return;
    resources = pdf_resolve_indirect(ctx_, resources);
    if (!pdf_is_dict(ctx_, resources)) return;
    const int num = pdf_to_num(ctx_, resources);
    if (num > 0) {
        if (std::find(seenRes->begin(), seenRes->end(), num) != seenRes->end()) return;
        seenRes->push_back(num);
    }

    pdf_obj* fonts = pdf_dict_get(ctx_, resources, PDF_NAME(Font));
    if (pdf_is_dict(ctx_, fonts)) {
        const int n = pdf_dict_len(ctx_, fonts);
        for (int i = 0; i < n; ++i) {
            pdf_obj* font = pdf_resolve_indirect(ctx_, pdf_dict_get_val(ctx_, fonts, i));
            if (!pdf_is_dict(ctx_, font)) continue;
            const int fnum = pdf_to_num(ctx_, font);
            if (fnum > 0 && std::find(out->begin(), out->end(), fnum) == out->end()) {
                out->push_back(fnum);
            }
        }
    }

    pdf_obj* xobjects = pdf_dict_get(ctx_, resources, PDF_NAME(XObject));
    if (pdf_is_dict(ctx_, xobjects)) {
        const int n = pdf_dict_len(ctx_, xobjects);
        for (int i = 0; i < n; ++i) {
            pdf_obj* xo = pdf_resolve_indirect(ctx_, pdf_dict_get_val(ctx_, xobjects, i));
            if (!pdf_is_dict(ctx_, xo)) continue;
            if (!pdf_name_eq(ctx_, pdf_dict_get(ctx_, xo, PDF_NAME(Subtype)), PDF_NAME(Form))) continue;
            pdf_obj* sub = pdf_dict_get(ctx_, xo, PDF_NAME(Resources));
            if (pdf_is_dict(ctx_, sub)) CollectFontNums(sub, depth + 1, out, seenRes);
        }
    }
}

void TextLayer::MaskResources(pdf_obj* resources, int depth) {
    if (!resources || depth > 8) return;
    resources = pdf_resolve_indirect(ctx_, resources);
    if (!pdf_is_dict(ctx_, resources)) return;
    const int num = pdf_to_num(ctx_, resources);
    if (num > 0) {
        if (std::find(seenResources_.begin(), seenResources_.end(), num) != seenResources_.end()) return;
        seenResources_.push_back(num);
    }

    pdf_obj* fonts = pdf_dict_get(ctx_, resources, PDF_NAME(Font));
    if (pdf_is_dict(ctx_, fonts)) {
        const int n = pdf_dict_len(ctx_, fonts);
        for (int i = 0; i < n; ++i) {
            pdf_obj* font = pdf_resolve_indirect(ctx_, pdf_dict_get_val(ctx_, fonts, i));
            if (!pdf_is_dict(ctx_, font)) continue;
            const int fnum = pdf_to_num(ctx_, font);
            if (fnum > 0) {
                if (std::find(seenFonts_.begin(), seenFonts_.end(), fnum) != seenFonts_.end()) continue;
                seenFonts_.push_back(fnum);
            }

            // 这个字体也被"本次不 OCR 的页"用到：动了它，那些页复制出来就变空白了
            if (fnum > 0 && !protectedFonts_.empty() &&
                std::find(protectedFonts_.begin(), protectedFonts_.end(), fnum) !=
                    protectedFonts_.end()) {
                continue;
            }

            CmapCodes parsed;
            pdf_obj* subtype = pdf_dict_get(ctx_, font, PDF_NAME(Subtype));
            const bool composite = pdf_name_eq(ctx_, subtype, PDF_NAME(Type0));

            if (!composite) {
                // 简单字体优先看 /Encoding /Differences
                pdf_obj* enc = pdf_dict_get(ctx_, font, PDF_NAME(Encoding));
                pdf_obj* diffs = pdf_dict_get(ctx_, enc, PDF_NAME(Differences));
                if (pdf_is_array(ctx_, diffs)) {
                    int code = -1;
                    const int dn = pdf_array_len(ctx_, diffs);
                    for (int k = 0; k < dn; ++k) {
                        pdf_obj* item = pdf_array_get(ctx_, diffs, k);
                        if (pdf_is_int(ctx_, item)) {
                            code = static_cast<int>(pdf_to_int(ctx_, item));
                        } else if (pdf_is_name(ctx_, item) && code >= 0) {
                            if (code <= 255) parsed.codes.insert(code);
                            ++code;
                        }
                    }
                    if (!parsed.codes.empty()) parsed.codeBytes = 1;
                }
            }

            if (parsed.codes.empty()) {
                pdf_obj* tounicode = pdf_dict_get(ctx_, font, PDF_NAME(ToUnicode));
                if (tounicode) {
                    fz_buffer* buf = nullptr;
                    fz_try(ctx_) { buf = pdf_load_stream(ctx_, tounicode); }
                    fz_catch(ctx_) {
                        fz_report_error(ctx_);
                        buf = nullptr;
                    }
                    if (buf) {
                        parsed = ParseToUnicodeCmap(reinterpret_cast<const char*>(buf->data), buf->len);
                        fz_drop_buffer(ctx_, buf);
                    }
                }
            }

            if (composite && (parsed.codes.empty() || parsed.codeBytes <= 0)) continue;
            if (parsed.codes.empty() || parsed.codeBytes <= 0) continue;

            const std::string cmap = BuildZeroWidthCmap(parsed.codes, parsed.codeBytes);
            pdf_obj* ref = MakeStream(ctx_, doc_, cmap, 1);
            pdf_dict_put_drop(ctx_, font, PDF_NAME(ToUnicode), ref);
            ++maskedFonts_;
            maskedCodes_ += static_cast<int>(parsed.codes.size());
        }
    }

    pdf_obj* xobjects = pdf_dict_get(ctx_, resources, PDF_NAME(XObject));
    if (pdf_is_dict(ctx_, xobjects)) {
        const int n = pdf_dict_len(ctx_, xobjects);
        for (int i = 0; i < n; ++i) {
            pdf_obj* xo = pdf_resolve_indirect(ctx_, pdf_dict_get_val(ctx_, xobjects, i));
            if (!pdf_is_dict(ctx_, xo)) continue;
            if (!pdf_name_eq(ctx_, pdf_dict_get(ctx_, xo, PDF_NAME(Subtype)), PDF_NAME(Form))) continue;
            pdf_obj* sub = pdf_dict_get(ctx_, xo, PDF_NAME(Resources));
            if (pdf_is_dict(ctx_, sub)) MaskResources(sub, depth + 1);
        }
    }
}

bool TextLayer::MaskOriginalText(const std::vector<int>& ocrPages, std::string* err) {
    if (!ctx_ || !doc_) {
        if (err) *err = "文本层未初始化";
        return false;
    }
    seenResources_.clear();
    seenFonts_.clear();
    protectedFonts_.clear();
    const auto wantPage = [&ocrPages](int page) {
        if (ocrPages.empty()) return true;  // 空 = 全部
        return std::find(ocrPages.begin(), ocrPages.end(), page) != ocrPages.end();
    };
    std::string message;
    fz_try(ctx_) {
        const int pages = pdf_count_pages(ctx_, doc_);
        // 第一步：先记下"这次不处理"的页用到的字体，它们必须原样保留。
        // 否则只 OCR 前几页时，后面没处理的页会把可复制文本一起清成零宽字符。
        const bool partial = !ocrPages.empty() && static_cast<int>(ocrPages.size()) < pages;
        if (partial) {
            std::vector<int> seenRes;
            for (int i = 0; i < pages; ++i) {
                if (wantPage(i)) continue;
                pdf_page* page = pdf_load_page(ctx_, doc_, i);
                pdf_obj* res = nullptr;
                fz_try(ctx_) { res = pdf_page_resources(ctx_, page); }
                fz_always(ctx_) { fz_drop_page(ctx_, reinterpret_cast<fz_page*>(page)); }
                fz_catch(ctx_) { fz_rethrow(ctx_); }
                CollectFontNums(res, 0, &protectedFonts_, &seenRes);
            }
        }
        // 第二步：只对本次 OCR 的页做屏蔽。
        for (int i = 0; i < pages; ++i) {
            if (!wantPage(i)) continue;
            pdf_page* page = pdf_load_page(ctx_, doc_, i);
            pdf_obj* res = nullptr;
            fz_try(ctx_) { res = pdf_page_resources(ctx_, page); }
            fz_always(ctx_) { fz_drop_page(ctx_, reinterpret_cast<fz_page*>(page)); }
            fz_catch(ctx_) { fz_rethrow(ctx_); }
            MaskResources(res, 0);
        }
    }
    fz_catch(ctx_) {
        if (err) *err = fz_caught_message(ctx_);
        return false;
    }
    return true;
}

bool TextLayer::AddPage(int pageIndex, const std::vector<ocr::TextLine>& lines, std::string* err) {
    if (!ctx_ || !doc_ || !fontRef_) {
        if (err) *err = "文字层未初始化";
        return false;
    }
    if (lines.empty()) return true;

    pdf_page* page = nullptr;
    fz_buffer* buf = nullptr;
    fz_output* out = nullptr;
    pdf_obj* stream = nullptr;
    pdf_obj* contents = nullptr;
    pdf_obj* pageObj = nullptr;

    fz_try(ctx_) {
        page = pdf_load_page(ctx_, doc_, pageIndex);
        pageObj = pdf_lookup_page_obj(ctx_, doc_, pageIndex);

        fz_rect mediabox;
        fz_matrix ctm;
        pdf_page_transform(ctx_, page, &mediabox, &ctm);

        pdf_obj* res = pdf_page_resources(ctx_, page);
        if (!pdf_is_dict(ctx_, res)) res = pdf_dict_put_dict(ctx_, pageObj, PDF_NAME(Resources), 6);
        pdf_obj* fonts = pdf_dict_get(ctx_, res, PDF_NAME(Font));
        if (!pdf_is_dict(ctx_, fonts)) fonts = pdf_dict_put_dict(ctx_, res, PDF_NAME(Font), 6);

        // 选一个本页未占用的资源名，避免覆盖原有字体名导致原文变形
        std::string fontName = "OCR0";
        for (int suffix = 1; pdf_dict_gets(ctx_, fonts, fontName.c_str()); ++suffix) {
            fontName = "OCR" + std::to_string(suffix);
        }
        pdf_dict_puts(ctx_, fonts, fontName.c_str(), fontRef_);

        const fz_matrix inv = fz_invert_matrix(ctm);
        buf = fz_new_buffer(ctx_, 16384);
        out = fz_new_output_with_buffer(ctx_, buf);
        fz_write_printf(ctx_, out, "q\n");
        fz_write_printf(ctx_, out, "%.6f %.6f %.6f %.6f %.6f %.6f cm\n", inv.a, inv.b, inv.c, inv.d, inv.e,
                  inv.f);
        fz_write_printf(ctx_, out, "BT\n3 Tr\n/%s 1 Tf\n", fontName.c_str());

        for (const auto& line : lines) {
            const float boxW = (line.x1 - line.x0) * scale_;
            const float boxH = (line.y1 - line.y0) * scale_;
            if (!(boxW > 0.f) || !(boxH > 0.f)) continue;
            const float fontSize = boxH / emHeight_;
            if (fontSize <= 0.5f) continue;

            std::vector<uint32_t> cps;
            DecodeUtf8(line.text, &cps);
            std::string hex;
            float widthEm = 0.f;
            bool any = false;
            for (uint32_t cp : cps) {
                if (cp == '\n' || cp == '\r' || cp == '\t') continue;
                const int gid = GlyphId(cp);
                if (gid <= 0) continue;
                char pair[8];
                std::snprintf(pair, sizeof(pair), "%04X", gid & 0xFFFF);
                hex += pair;
                widthEm += fz_advance_glyph(ctx_, font_, gid, 0);
                any = true;
            }
            if (!any || widthEm <= 0.f) continue;

            const float natural = widthEm * fontSize;
            if (!(natural > 0.f)) continue;
            float squeeze = boxW / natural;
            if (squeeze < kSqueezeMin) squeeze = kSqueezeMin;
            if (squeeze > kSqueezeMax) squeeze = kSqueezeMax;

            const float x = line.x0 * scale_;
            const float baseline = line.y0 * scale_ - topRatio_ * fontSize;
            fz_write_printf(ctx_, out, "%.4f 0 0 %.4f %.3f %.3f Tm\n", fontSize * squeeze, fontSize, x,
                      baseline);
            fz_write_printf(ctx_, out, "<%s> Tj\n", hex.c_str());
            ++insertedLines_;
        }

        fz_write_printf(ctx_, out, "ET\nQ\n");
        fz_close_output(ctx_, out);
        fz_drop_output(ctx_, out);
        out = nullptr;
        fz_terminate_buffer(ctx_, buf);

        stream = pdf_add_stream(ctx_, doc_, buf, nullptr, 1);

        // /Contents 要原样保留"间接引用"：数组元素和单个流都必须是 xref 引用。
        // 若把 pdf_resolve_indirect 之后的流对象直接塞进数组，保存时它只能写成字典
        // （reading 时报 object is not a stream），页面内容随之丢失，成品 PDF 变成空白页。
        pdf_obj* contentsRef = pdf_dict_get(ctx_, pageObj, PDF_NAME(Contents));
        contents = pdf_resolve_indirect(ctx_, contentsRef);
        pdf_obj* newContents = pdf_new_array(ctx_, doc_, 8);
        pdf_array_push(ctx_, newContents, stream);
        if (pdf_is_array(ctx_, contents)) {
            const int n = pdf_array_len(ctx_, contents);
            for (int i = 0; i < n; ++i) pdf_array_push(ctx_, newContents, pdf_array_get(ctx_, contents, i));
        } else if (contents && !pdf_is_null(ctx_, contents)) {
            pdf_array_push(ctx_, newContents,
                           pdf_is_indirect(ctx_, contentsRef) ? contentsRef : contents);
        }
        pdf_dict_put_drop(ctx_, pageObj, PDF_NAME(Contents), newContents);
    }
    fz_always(ctx_) {
        if (out) fz_drop_output(ctx_, out);
        if (buf) fz_drop_buffer(ctx_, buf);
        if (page) fz_drop_page(ctx_, reinterpret_cast<fz_page*>(page));
    }
    fz_catch(ctx_) {
        if (err) *err = fz_caught_message(ctx_);
        return false;
    }
    return true;
}

void TextLayer::SubsetOcrFont() {
    if (!fontRef_ || !ctx_ || !doc_) return;

    std::vector<int> gids;
    gids.reserve(gidCache_.size() + 1);
    for (const auto& kv : gidCache_) {
        if (kv.second > 0) gids.push_back(kv.second);
    }
    gids.push_back(0);  // 保留 .notdef
    std::sort(gids.begin(), gids.end());
    gids.erase(std::unique(gids.begin(), gids.end()), gids.end());
    if (gids.size() <= 1) return;

    pdf_obj* descendants = pdf_dict_get(ctx_, fontRef_, PDF_NAME(DescendantFonts));
    pdf_obj* cidfont = pdf_array_get(ctx_, descendants, 0);
    pdf_obj* descriptor = pdf_dict_get(ctx_, cidfont, PDF_NAME(FontDescriptor));
    pdf_obj* file = pdf_dict_get(ctx_, descriptor, PDF_NAME(FontFile2));
    if (!pdf_is_stream(ctx_, file)) return;

    fz_buffer* buf = nullptr;
    fz_buffer* sub = nullptr;
    fz_try(ctx_) {
        buf = pdf_load_stream(ctx_, file);
        sub = fz_subset_ttf_for_gids(ctx_, buf, gids.data(), static_cast<int>(gids.size()), 0, 1);
        pdf_update_stream(ctx_, doc_, file, sub, 1);
        pdf_dict_put_int(ctx_, file, PDF_NAME(Length1), static_cast<int64_t>(sub->len));
    }
    fz_always(ctx_) {
        if (sub) fz_drop_buffer(ctx_, sub);
        if (buf) fz_drop_buffer(ctx_, buf);
    }
    fz_catch(ctx_) { fz_rethrow(ctx_); }
}

bool TextLayer::Save(const std::string& outPath, std::string* err) {    if (!ctx_ || !doc_) {
        if (err) *err = "文字层未初始化";
        return false;
    }
    // 字体子集化：失败也不致命，只是文件会大一些
    // 只对 OCR 字体做子集化：MuPDF 自带的 pdf_subset_fonts 会重写文档里所有字体，
    // 实测会把公式字体重渲染出细微差异；这里按已用字形精确裁剪我们自己嵌入的字体。
    fz_try(ctx_) { SubsetOcrFont(); }
    fz_catch(ctx_) {
        fz_report_error(ctx_);
    }

    // 只压缩未压缩的流；不改动已有图像（重编码既慢又可能改变外观），
    // 不重写原有内容流（do_clean 会显著拖慢大文件）。
    pdf_write_options opts = pdf_default_write_options;
    opts.do_compress = 1;
    opts.do_compress_fonts = 1;
    opts.do_compress_images = 0;
    opts.do_garbage = 2;  // 2=重编号压缩；3(去重) 在大文件上要跑几分钟
    opts.do_clean = 0;
    opts.do_use_objstms = 1;


    fz_try(ctx_) { pdf_save_document(ctx_, doc_, outPath.c_str(), &opts); }
    fz_catch(ctx_) {
        if (err) *err = fz_caught_message(ctx_);
        return false;
    }
    return true;
}

}  // namespace pdf
