# c++_learn

C++ 学习与试验仓库（含 PDF OCR 工具 GUI 版的原型开发）。

## 目录规划

| 目录 | 内容 |
|---|---|
| `notes/` | 学习笔记、环境搭建记录 |
| `sandbox/` | 零散的小试验（每个子目录一个独立 CMake 工程） |
| `ocr-gui/` | PDF OCR 工具（Dear ImGui + ONNX Runtime/RapidOCR）原型 |

## 环境

- Windows 11 + VS 2022 Community（MSVC 14.44）
- CMake 3.31.6 / Ninja（VS 自带）
- Windows SDK 10.0.26100

## 构建约定

所有工程统一用：

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## ocr-gui（PDF OCR 工具 GUI 原型）

进度：M0 完成（窗口骨架 + 中文界面 + 打开 PDF 对话框）

依赖（FetchContent 自动拉取）：Dear ImGui v1.91.5、GLFW 3.3.9

构建（**务必用 Dropbox 外的构建目录**，Dropbox 同步会锁文件导致 MSB6003）：

```powershell
$cmake = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
& $cmake -S . -B C:\Users\xwtal\build\ocrgui -G "Visual Studio 17 2022" -A x64
& $cmake --build C:\Users\xwtal\build\ocrgui --config Release
```

产物：`C:\Users\xwtal\build\ocrgui\Release\ocrgui.exe`

待接入：ONNX Runtime + RapidOCR 模型、MuPDF 渲染与文字层写回（M1/M2）。
