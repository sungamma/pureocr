// MuPDF 内置字体的零长度桩（只在 OCR_MUPDF_SLIM 下编译）。
//
// 官方 libmupdf.lib 把每个内置字体单独编成一个 .obj，导出
//     _binary_<name> / _binary_<name>_size
// 两个 C 符号；其中三个 CJK 字体（思源宋体 TTC + 两个 Droid 回退字体）合计约 31.9 MB，
// 会被整块链接进 exe 的 .data 段。tools/slim_mupdf.ps1 把这三个 obj 从归档里摘掉，
// 这里补上同名零长度符号让链接继续通过；真正的 CJK 回退由 src/pdf/system_fonts.cpp
// 改用系统字体（msyh/simsun）提供。
//
// 注意：符号名必须与 tools/slim_mupdf.ps1 的 -Remove 列表一一对应，改一处要改两处。

#if defined(OCR_MUPDF_SLIM)

#include <cstddef>

// 长度符号按 size_t 尺寸定义（8 字节）：即使库那边按 unsigned int 读，低 4 字节同样是 0。
#define OCRGUI_EMPTY_FONT_BLOB(name)             \
    extern "C" {                                 \
    extern const unsigned char _binary_##name[]; \
    const unsigned char _binary_##name[] = {0};  \
    extern const std::size_t _binary_##name##_size; \
    const std::size_t _binary_##name##_size = 0; \
    }

OCRGUI_EMPTY_FONT_BLOB(SourceHanSerif_Regular_ttc)
OCRGUI_EMPTY_FONT_BLOB(DroidSansFallbackFull_ttf)
OCRGUI_EMPTY_FONT_BLOB(DroidSansFallback_ttf)

#undef OCRGUI_EMPTY_FONT_BLOB

#endif  // OCR_MUPDF_SLIM