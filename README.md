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

## M2 完成（2026-11-02）：PDF -> 可复制文字 PDF

接入 MuPDF 1.25.0 做 PDF 渲染与写入，打通「PDF 输入 -> 逐页渲染 -> OCR -> 隐形文字层 -> 保存」。

### 1. 编译 MuPDF（只需一次）

`third_party/` 已被 gitignore，需要自己构建；仓库里不放二进制。

1. 下载源码：https://mupdf.com/downloads/archive/mupdf-1.25.0-source.tar.gz
2. 用 VS 自带 sln 编静态库（要用 v143 覆盖它的 v142）：

```powershell
$msbuild = "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
& $msbuild "<源码目录>\platform\win32\mupdf.sln" /t:libmupdf /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143 /m
```

3. 拷贝：`<源码目录>/include` -> `third_party/mupdf-1.25.0/include`，
   `<源码目录>/platform/win32/x64/Release/libmupdf.lib` -> `third_party/mupdf-1.25.0/lib/`。

CMake 检测到 `third_party/mupdf-1.25.0/lib/libmupdf.lib` 才会打开 `OCRGUI_HAS_MUPDF`；
没有该库也能编译，只是 PDF 功能不可用。

> MuPDF 是 AGPL 授权（个人/开源使用没问题，商业分发需 Artifex 商业许可）。

### 2. CLI 用法

```powershell
$exe = 'C:\Users\xwtal\build\ocrgui\Release\ocrgui.exe'

# 全文档
& $exe --pdf "输入.pdf" --out-pdf "输出.pdf" --threads 4 --model-dir ocr-gui/models --log run.log

# 只处理部分页（输出仍是整本，未选中的页不加文字层）
& $exe --pdf "输入.pdf" --pages 1-10,15 --dpi 300 --threads 4

# 不屏蔽原有文字层（默认屏蔽，保证复制出来的只有 OCR 结果）
& $exe --pdf "输入.pdf" --no-mask
```

参数：`--pdf`、`--out-pdf`（默认 `原名_可复制文字.pdf`）、`--dpi`（默认 300）、`--threads`（默认 4）、
`--pages`、`--font`（默认 simhei.ttf）、`--min-score`（默认 0.5）、`--no-mask`、`--log`。

> 这个 exe 是 GUI 子系统程序，从 PowerShell 直接 `&` 调用不会等它结束；
> 脚本里请用 `Start-Process -Wait -WindowStyle Hidden`，或者加 `--log` 看日志。
> 进程默认降到 below-normal 优先级，尽量不抢前台。

### 3. GUI

不带参数启动即为 GUI：选 PDF -> 设线程数/DPI/置信度 -> 开始识别，进度条 + 日志实时刷新。

### 4. 与 Python 版对齐的关键实现

| 环节 | Python 版 | C++ 版 |
|---|---|---|
| 字号/基线 | `fontsize = box_h / em_height`；`baseline = y0*scale - top_ratio*fontsize` | 相同；`top_ratio` 用 `fz_bound_glyph("国")` 量字形墨迹盒 |
| 横向对齐 | `insert_text(..., morph=Matrix(squeeze,1))` | 内容流 `Tm [fontsize*squeeze 0 0 fontsize x y]` |
| 隐形字形 | `render_mode=3` | 内容流 `BT 3 Tr ... ET` |
| 屏蔽原文 | 重写每个字体的 `/ToUnicode` 为零宽字符 | 同样，`MaskOriginalText()` 递归遍历 Resources / Form XObject |
| 字体子集 | `doc.subset_fonts()` | 只裁 OCR 字体本身（`fz_subset_ttf_for_gids`） |

两个踩过的坑：

1. 不能用 `pdf_subset_fonts()`：它会重写文档里所有字体，实测让公式字体渲染出现每页几十个像素差异；
   改成只裁剪新加的字体后逐像素差异归零。
2. `do_garbage = 3`（去重）在 40MB / 339 页文件上要 3 分钟；用 `2`（重编号）只要 0.4 秒。

### 5. 验收工具

```powershell
python ocr-gui/tools/verify_pdf.py "原始.pdf" "C++产物.pdf" --reference "Python产物.pdf"
```

实测（样本 10 页，300dpi，4 线程）：

| 指标 | 结果 |
|---|---|
| 页数 / 书签 | 10/10；339 页那本 176/176 完全一致 |
| 渲染逐像素 | 10/10 页差异像素 = 0 |
| 文字层位置 | 框偏差中位 dx 0.12~0.20 pt，dy 0.37~1.45 pt |
| 文本相似度 | 纯文字页 0.93~0.99（忽略空格 0.96~0.99） |
| 屏蔽原字体 | 4533 个字体 / 49487 个编码，与 Python 版完全一致 |
| 速度 | 10 页 38 s（≈3.8 s/页，4 线程）；Python 版约 11 s/页 |

第 6、8 页相似度偏低（0.31 / 0.45）是评分口径问题：那两页是插图页，Python 版识别出大量「....」点线，
C++ 反而识别出 `GB/T 151—2026`、`浮头式`、坐标轴数字等真实内容。


## M2.1 完成（2026-11-02）：预览校验、剩余时间、打包

### 1. GUI 拆成两个页签

- 「识别」：打开 PDF / 页范围 / 另存为 / 线程数 / DPI / 置信度；进度条带**已用时间 + 预计剩余**。
- 「预览」：渲染 PDF 某页并叠加 OCR 识别框（绿=正常，红=低于阈值），鼠标悬停显示该行文字与置信度。
  识别完成后预览的是**产物 PDF**，可直接肉眼确认文字层有没有错位；未识别时预览原文件。

预览按需渲染（页码 / DPI / 文件变化才重画），默认 120 dpi；识别完成点「查看识别框」直达该页签。

### 2. per-worker ORT 线程（默认 4 线程）

每个 worker 各自一个 ORT session。原先 `threads = 0` 让 ORT 用满物理核，N 个 worker 就是 N 倍超订。
改成 `hardware_concurrency / threadCount`（1..8 clamp）。实测 10 页样本（16 核，无 GPU）：

| 线程数 | 改前 | 改后 |
|---|---|---|
| 4 | 37.8 s | 34.8 s |
| 8 | 42.0 s | 35.1 s |

8 线程相对 4 线程已无收益，**建议 4 线程**（2~3 线程前台更流畅）。

### 3. 打包

```powershell
pwsh -NoProfile -File ocr-gui/tools/package.ps1          # 生成 ocr-gui/dist/ocrgui-YYYYMMDD 目录 + zip
pwsh -NoProfile -File ocr-gui/tools/package.ps1 -NoZip   # 只出目录
```

产物目录：`ocrgui.exe`、`onnxruntime.dll`、`models\*.onnx`、`使用说明.txt`、`run-cli.ps1`。
双击 exe 进 GUI；`run-cli.ps1 -Pdf <文件> [-Pages 1-10] [-Threads 4] [-NoMask]` 走命令行。

### 4. 踩坑：中文路径让 GUI 闪退（已修）

`std::filesystem::path::replace_filename(std::string)` 会按**系统 ACP(GBK)** 把窄字符串转 wchar，
而这里传入的是 UTF-8 的 `原名_可复制文字.pdf`；转换失败抛 `filesystem_error`，未捕获 ->
`std::terminate` -> 0xc0000409 闪退（GUI 打开中文名 PDF 必崩，CLI 只在没给 `--out-pdf` 时崩）。

修法：新增 `src/util/path_util.h`，后缀拼接**纯 UTF-8 字符串**完成，不经过 path 的窄字符转换，CLI/GUI 共用。
顺带修了 `WideToUtf8` / `Utf8ToWide` 写 NUL 到 `data()[size()]` 的 1 字节越界。
