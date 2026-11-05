#pragma once

struct fz_context;

namespace pdf {

/// 给 MuPDF 装上"从 Windows 系统字体加载"的钩子。
///
/// slim 版（libmupdf-slim.lib）剔除了内嵌的 CJK 字体，PDF 里若出现没有嵌入的
/// 中文/日文/韩文字体，MuPDF 会走 fz_load_system_cjk_font 钩子找替代字体。
/// 这里把钩子指到 C:\Windows\Fonts 下的微软雅黑 / 宋体 / 黑体等。
///
/// 可以重复调用（每次换掉前一套钩子）；ctx 为空时什么都不做。
void InstallSystemFontHooks(fz_context* ctx);

}  // namespace pdf