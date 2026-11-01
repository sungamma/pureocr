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
