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

## M1 进度（2026-11-02）

onnxruntime + PP-OCRv3 已接入，C++ 侧可独立完成「图像 -> 检测 -> 方向分类 -> 识别」全流程。

```powershell
# 首次：拷模型
python ocr-gui/tools/fetch_models.py

# 构建
& $cmake --build C:\Users\xwtal\build\ocrgui --config Release

# CLI 识别单张图片，输出 JSON
C:\Users\xwtal\build\ocrgui\Release\ocrgui.exe --image page.png `
    --model-dir ocr-gui/models --out result.json

# 与 Python 版逐页对比（M1 验收工具）
python ocr-gui/tools/compare_page.py "样本.pdf" 5
```

实测（GB/T 151-2026 第 5 页 @300dpi，2481x3509）：

| 指标 | C++ | Python(rapidocr) |
|---|---|---|
| 文本框偏差 | dx -0.1px / dy -0.3px | 基准 |
| 字符总数 | 767 | 750 |
| 单页耗时 | 5.4 s | 10.6 s |

已知差异：C++ 用轴对齐框裁剪，Python 用四点透视裁剪，导致个别行少/多一两个字符（多为空格）。
下一步 M1.5：改成透视裁剪 + 合并行策略对齐 rapidocr；然后再进 M2（MuPDF 渲染与文字层写回）。

## M1.5 进度（2026-11-02）

- 裁剪改为**四点透视裁剪**（对齐 rapidocr `get_rotate_crop_image`，含高宽比 >=1.5 转 90°）
- 角点排序对齐 PaddleOCR `get_mini_boxes`（按 x 排序 + y 关系定 tl/tr/br/bl）
- unclip 外扩补偿 1px（pyclipper 用取整后的四点算面积周长，连续值略大）
- 每张裁剪图独立定宽（batch=1），结果不受批内邻居影响
- 新增 `--dump-crops <dir>`：导出每张裁剪图，便于和 Python 逐张比对

最新实测（第 5 页）：行偏移 +0.9/+0.7px，字符数 769 vs 750，文本相似度中位 **1.000**、
≥0.95 占比 72.8%、字节级一致 65%；单页 4.4s（Python 11.2s）。

已知差异（非 bug）：C++ 识别结果与 rapidocr 的 `resize_norm_img + session` 底层路径**完全一致**
（同图同分数），但 rapidocr 的 `__call__` 包装层给出的分数系统性偏低 ~0.2，
于是它在 0.5 阈值下过滤掉更多小框（165 检出 → 117），C++ 保留更多（→158）。
