# 跨平台现状（Linux / macOS）

**结论：当前只支持 Windows 10/11 x64。** CI 里的 Linux / macOS 任务只是“编译探测”，失败不影响流水线。

## CI 现状

| 平台 | 状态 | 说明 |
|---|---|---|
| Windows x64 (MSVC) | ✅ 完整 | `tools/fetch_deps.ps1` + `tools/release.ps1`，产物是可直接双击运行的 zip |
| Ubuntu | ⚠️ 探测 | 只跑 `cmake configure` / `build`，预期失败 |
| macOS | ⚠️ 探测 | 同上 |

移植完成后，把 `.github/workflows/build.yml` 里 `unix-probe` 任务的 `continue-on-error: true` 去掉即可。

## 为什么要移植（Windows 专有代码清单）

| 位置 | Windows 专有内容 | 移植方案 |
|---|---|---|
| `src/util/http_download.cpp` | WinHTTP 下载模型（34 处 API） | 换成 libcurl（`find_package(CURL)`），Win 上也可复用 |
| `src/main.cpp` | `GetOpenFileNameW` / `GetSaveFileNameW` / `ShellExecuteW` | 抽一个 `platform_dialogs` 接口；macOS 用 NSOpenPanel，Linux 用 GTK/zenity，或退化成“拖拽 + 命令行” |
| `src/pdf/system_fonts.cpp` | 注册表 + `C:\Windows\Fonts` 找 CJK 字体 | macOS 用 CoreText，Linux 用 fontconfig（`FcFontMatch`） |
| `src/util/utf8_io.h`、`src/cli.cpp`、`src/ocr/engine.cpp`、`src/pdf/ocr_pages.cpp`、`src/util/model_registry.cpp` | `MultiByteToWideChar` / 宽字符路径 | POSIX 上路径本来就是 UTF-8，抽象成 `path_from_utf8()` 后直接用窄字符串；ORT 的 `ORT_TSTR` 在非 Windows 上就是 `char` |
| `CMakeLists.txt` | `add_executable(... WIN32 ...)`、`resources/app.rc`（图标/版本）、`winhttp/comdlg32/shell32/ole32/bcrypt` | 条件化：非 Windows 去掉 `WIN32` 与 `.rc`；`opengl32`→`OpenGL`/`Cocoa` 由 GLFW 自带 |
| `libmupdf.lib` + `tools/slim_mupdf.ps1` + `src/pdf/mupdf_font_stubs.cpp` | 静态库名与 `lib.exe /REMOVE` 瘦身 | macOS/Linux 用 `make build=release libs` 编 MuPDF（产物 `libmupdf.a`），需要用 `ar d` 做同样的字体剔除 |
| 依赖目录 `third_party/*` | 只放了 win-x64 的 ONNX Runtime 与 MSVC 版 MuPDF | 非 Windows 需要各自平台（linux-x64 / osx-arm64 等）的 ORT 包，见 `tools/fetch_deps.ps1` |

## 移植顺序建议

1. 先抽 `src/util/platform.h`：UTF-8 路径、系统字体、对话框、HTTP 四组接口，Windows 实现保持现状，其它平台先给“最小可用”实现（对话框可以只留拖拽）。
2. 换 HTTP 后端（libcurl）——这是唯一会阻塞 “下载模型” 的部分，也是命令行批处理必需的。
3. 改 CMake：`if (WIN32)` 包住 `.rc` / `WIN32` 子系统 / win 库；MuPDF 库名按平台区分。
4. 编 MuPDF：macOS/Linux 上 `cd third_party/mupdf-1.25.0/src && make build=release libs`。
5. 在 CI 里打开 `unix-probe` 的失败拦截，确认矩阵全绿后再发版。

> 注意：拖拽文件在 GLFW 上是跨平台的，可以先把 Linux/macOS 的“选择文件”降级成拖拽，再做原生对话框。