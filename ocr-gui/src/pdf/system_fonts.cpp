#include "pdf/system_fonts.h"

#include <cstring>
#include <string>

#include <windows.h>

#include "mupdf/fitz.h"

namespace pdf {
namespace {

/// 把 C:\Windows\Fonts\xxx.ttf 组织成 UTF-8 路径（Windows 目录不一定在 C:）。
std::string FontPath(const char* file) {
    wchar_t dir[MAX_PATH] = {};
    UINT n = GetWindowsDirectoryW(dir, MAX_PATH);
    std::wstring w;
    if (n > 0 && n < MAX_PATH) {
        w.assign(dir, n);
    } else {
        w = L"C:\\Windows";
    }
    w += L"\\Fonts\\";
    for (const char* p = file; p && *p; ++p) w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));

    const int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return std::string();
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}

/// 试着从系统字体目录加载一个字体文件；不存在或解析失败都返回 nullptr（不抛）。
fz_font* TryLoadFromSystem(fz_context* ctx, const char* file, int index, const char* name) {
    if (!file || !*file) return nullptr;
    const std::string path = FontPath(file);
    if (path.empty()) return nullptr;

    fz_font* font = nullptr;
    fz_try(ctx) {
        font = fz_new_font_from_file(ctx, name, path.c_str(), index, 0);
    }
    fz_catch(ctx) {
        font = nullptr;
    }
    return font;
}

fz_font* TryList(fz_context* ctx, const char* const* files, int count, const char* name) {
    for (int i = 0; i < count; ++i) {
        if (fz_font* font = TryLoadFromSystem(ctx, files[i], 0, name)) return font;
    }
    return nullptr;
}

bool EqualsNoCase(const char* a, const char* b) {
    if (!a || !b) return false;
    return _stricmp(a, b) == 0;
}

bool HasPrefixNoCase(const char* s, const char* prefix) {
    if (!s || !prefix) return false;
    const size_t n = std::strlen(prefix);
    return _strnicmp(s, prefix, n) == 0;
}

struct SimpleFont {
    const char* name;
    const char* regular;
    const char* bold;
    const char* italic;
    const char* boldItalic;
};

// 常见西文字体名 -> 系统字体文件。找不到就按 serif/sans 兜底。
const SimpleFont kSimpleFonts[] = {
    {"arial", "arial.ttf", "arialbd.ttf", "ariali.ttf", "arialbi.ttf"},
    {"helvetica", "arial.ttf", "arialbd.ttf", "ariali.ttf", "arialbi.ttf"},
    {"times new roman", "times.ttf", "timesbd.ttf", "timesi.ttf", "timesbi.ttf"},
    {"times", "times.ttf", "timesbd.ttf", "timesi.ttf", "timesbi.ttf"},
    {"courier new", "cour.ttf", "courbd.ttf", "couri.ttf", "courbi.ttf"},
    {"courier", "cour.ttf", "courbd.ttf", "couri.ttf", "courbi.ttf"},
    {"calibri", "calibri.ttf", "calibrib.ttf", "calibrii.ttf", "calibriz.ttf"},
    {"cambria", "cambria.ttc", "cambriab.ttf", "cambriai.ttf", "cambriaz.ttf"},
    {"segoe ui", "segoeui.ttf", "segoeuib.ttf", "segoeuii.ttf", "segoeuiz.ttf"},
    {"tahoma", "tahoma.ttf", "tahomabd.ttf", nullptr, nullptr},
    {"verdana", "verdana.ttf", "verdanab.ttf", "verdanai.ttf", "verdanabi.ttf"},
};

// 微软雅黑、宋体、黑体：中文文档最常见的三个替代品。
const char* const kCjkSans[] = {"msyh.ttc", "msyhbd.ttc", "simhei.ttf"};
const char* const kCjkSerif[] = {"simsun.ttc", "simsunb.ttf", "msyh.ttc"};
const char* const kJapanese[] = {"YuGothM.ttc", "msgothic.ttc", "meiryo.ttc"};
const char* const kJapaneseSerif[] = {"yumin.ttf", "msmincho.ttc", "YuMincho.ttc"};
const char* const kKorean[] = {"malgun.ttf", "gulim.ttc", "batang.ttc"};
const char* const kKoreanSerif[] = {"batang.ttc", "malgun.ttf"};

/// 普通字体名缺失时的钩子（例如 PDF 里写了 Arial 但没嵌入）。
fz_font* LoadSystemFont(fz_context* ctx, const char* name, int bold, int italic,
                        int needs_exact_metrics) {
    // 度量必须精确匹配的场景不要拿相似字体硬顶，交给 MuPDF 自己处理。
    if (needs_exact_metrics) return nullptr;

    for (const SimpleFont& entry : kSimpleFonts) {
        const bool match = EqualsNoCase(name, entry.name) || HasPrefixNoCase(name, entry.name);
        if (!match) continue;
        const char* file = nullptr;
        if (bold && italic) file = entry.boldItalic;
        else if (bold) file = entry.bold;
        else if (italic) file = entry.italic;
        if (!file) file = entry.regular;
        if (fz_font* font = TryLoadFromSystem(ctx, file, 0, name)) return font;
        break;
    }
    // 兜底：按 serif/sans 给个系统默认字体，比直接渲染失败要好。
    const char* const serifFallback[] = {"times.ttf", "georgia.ttf"};
    const char* const sansFallback[] = {"arial.ttf", "segoeui.ttf"};
    const bool serif = HasPrefixNoCase(name, "times") || HasPrefixNoCase(name, "georgia") ||
                       HasPrefixNoCase(name, "cambria") || HasPrefixNoCase(name, "garamond");
    return serif ? TryList(ctx, serifFallback, 2, name) : TryList(ctx, sansFallback, 2, name);
}

/// CJK 回退字体：内嵌字体已被剔除，这里全部改从系统字体目录取。
fz_font* LoadSystemCjkFont(fz_context* ctx, const char* name, int ordering, int serif) {
    switch (ordering) {
        case FZ_ADOBE_JAPAN:
            return serif ? TryList(ctx, kJapaneseSerif, 3, name) : TryList(ctx, kJapanese, 3, name);
        case FZ_ADOBE_KOREA:
            return serif ? TryList(ctx, kKoreanSerif, 2, name) : TryList(ctx, kKorean, 3, name);
        case FZ_ADOBE_GB:
        case FZ_ADOBE_CNS:
        default:
            return serif ? TryList(ctx, kCjkSerif, 3, name) : TryList(ctx, kCjkSans, 3, name);
    }
}

/// 缺字形时的通用回退。MuPDF 只在极少数文档上用到，这里交给 CJK 字体兜底。
fz_font* LoadFallbackFont(fz_context* ctx, int /*script*/, int /*language*/, int serif,
                          int /*bold*/, int /*italic*/) {
    return serif ? TryList(ctx, kCjkSerif, 3, "fallback") : TryList(ctx, kCjkSans, 3, "fallback");
}

}  // namespace

void InstallSystemFontHooks(fz_context* ctx) {
    if (!ctx) return;
    fz_try(ctx) {
        fz_install_load_system_font_funcs(ctx, LoadSystemFont, LoadSystemCjkFont, LoadFallbackFont);
    }
    fz_catch(ctx) {
        // 钩子装不上不影响主流程（只是回退字体少一些）
    }
}

}  // namespace pdf