# 跨平台现状（Linux / macOS）

**结论：Windows 10/11 x64 与 Linux x64 均已支持**（同一份代码，用 `#ifdef _WIN32` 分支）；
macOS 尚未完成，CI 里只有“编译探测”，失败不阻塞流水线。

## CI 现状

| 平台 | 状态 | 说明 |
|---|---|---|
| Windows x64 (MSVC) | 完整 | `tools/fetch_deps.ps1` + `tools/release.ps1`，产物 `ocrgui-<ver>-win-x64.zip` |
| Linux x64 (GCC) | 完整 | `tools/fetch_deps_linux.sh` + `tools/package_linux.sh`，产物 `ocrgui-<ver>-linux-x64.tar.gz` |
| macOS | 探测 | 只跑 configure/build，`continue-on-error: true` |

发布：推送 `v*` tag（且与 `VERSION` 一致）后，`release` 任务把 Windows 与 Linux 产物一起上传到同一个 GitHub Release。

## 平台适配层

| 位置 | Windows 实现 | POSIX 实现 |
|---|---|---|
| `src/util/platform.h` | 宽字符 Win32 API | `stat` / `mkdir` / `readlink("/proc/self/exe")` |
| `src/util/utf8_io.h` | UTF-8 转 UTF-16 后用 `_wfopen_s` | 直接 `fopen`（路径本身就是 UTF-8 字节） |
| `src/util/http_download.cpp` / `http_download_posix.cpp` | WinHTTP + CNG SHA256 | libcurl + OpenSSL SHA256 |
| `src/main.cpp` | `GetOpenFileNameW` / `SHBrowseForFolderW` / `ShellExecuteW` | `zenity` 文件选择 + `xdg-open` 打开目录 |
| `src/pdf/ocr_pages.cpp` | `CreateFileW` 独占锁 / `SetPriorityClass` | `flock` 独占锁 / `setpriority` |
| `src/pdf/system_fonts.cpp` | `C:\Windows\Fonts` 内置清单 | Noto CJK / 文泉驿 / DejaVu 常见路径 |
| `src/ocr/engine.cpp` | `ORTCHAR_T = wchar_t` | `ORTCHAR_T = char`（`PathStr` 抽象） |
| `src/util/model_registry.cpp` | `%LOCALAPPDATA%` 回退 | `$XDG_DATA_HOME` / `$HOME` 回退 |

## Linux 依赖

```bash
sudo apt-get install -y build-essential cmake ninja-build pkg-config \
     libcurl4-openssl-dev libssl-dev libgl1-mesa-dev libglu1-mesa-dev xorg-dev \
     libfontconfig1-dev fonts-noto-cjk zenity
```

MuPDF 与 ONNX Runtime 由 `tools/fetch_deps_linux.sh` 下载/编译到 `third_party/`（不入库）。
MuPDF 需要链接 `libmupdf.a` 与 `libmupdf-third.a`，CMake 已处理。

## 已知限制（Linux）

- 文件/文件夹对话框依赖 `zenity`（未安装时只能走命令行参数）。
- GUI 需要 X11 / Wayland 显示服务器；无头环境请用 `--pdf` / `--dir` 命令行模式。
- 内置 simhei 字体不存在，PDF 文字层改用系统 Noto CJK；缺失时会在日志中提示。
- 断点续跑的锁文件用 `flock`，NFS 等不支持 `flock` 的文件系统上可能失效。

## macOS 后续工作

`src/util/platform.h` 已预留 `_NSGetExecutablePath` 分支，剩余需要补的：

1. `http_download_posix.cpp` 可直接复用（libcurl + OpenSSL 或 Security.framework）。
2. 系统字体表补 `/System/Library/Fonts/`（PingFang / Hiragino）路径。
3. 文件对话框接 `osascript` 或原生 NSOpenPanel。
4. CI 去掉 `macos-probe` 的 `continue-on-error` 并接入打包。