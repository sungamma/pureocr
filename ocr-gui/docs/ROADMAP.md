# ocr-gui 开发路线与交接备忘

> 最后更新：2026-11-02　分支：`c++_learn` / `main`
> 用途：换一个窗口/目录继续开发时，先读这份文档即可接上上下文。

## 1. 项目速览

| 项 | 值 |
|---|---|
| 仓库 | `D:\Dropbox\14.repo\git\c++_learn`（`ocr-gui/` 子目录） |
| 定位 | Dear ImGui 桌面界面 + 命令行双入口；OCR 引擎为 ONNX Runtime 上的 PP-OCR（RapidOCR 同款流程） |
| 构建目录 | `C:\Users\xwtal\build\ocrgui`（**必须在 Dropbox 之外**，否则编译/链接报文件占用） |
| 产物 | `C:\Users\xwtal\build\ocrgui\Release\ocrgui.exe` |
| 模型 | `ocr-gui\models`（不入库）：v3 / v4 / v5 移动版 + 方向分类 + `ppocrv5_dict.txt` |
| 测试样本 | `D:\Dropbox\14.repo\git\script\ocr\test\样本10页.pdf`（**不入库**） |
| 机器 | Windows 11 / 16 核 / 32 GB / **无 NVIDIA 显卡** |

### 构建与运行

```powershell
cmake --build C:\Users\xwtal\build\ocrgui --config Release
# 首次或改 CMakeLists 后：
cmake -S .\ocr-gui -B C:\Users\xwtal\build\ocrgui -G "Visual Studio 17 2022" -A x64
python tools\fetch_models.py            # 拉 v3/v4/v5（缺的自动从 HF 下载）
python tools\fetch_models.py --server   # 额外拉 v4 服务器版
```

exe 是 **GUI 子系统**，PowerShell 里 `&` 不会等待，必须：

```powershell
Start-Process -Wait -WindowStyle Hidden -FilePath $exe -ArgumentList @('--pdf', $pdf, '--out-pdf', $out)
```

### 命令行参数

```
ocrgui --pdf <in.pdf> [--out-pdf <out.pdf>] [--dpi 300] [--threads 4]
       [--pages 1-10,15] [--font <ttf>] [--no-mask] [--min-score 0.5]
       [--csv <导出.csv>] [--review-score 0.75] [--resume] [--cache <缓存.tsv>]
       [--preset auto|v5|v4|v3|v4s] [--det-model <文件名|绝对路径>]
       [--rec-model <文件名|绝对路径>] [--cls-model <文件名>]
       [--merge-gap 0.8]   # 同行碎片合并阈值（×行高），0 = 关闭
       [--model-dir <模型目录>] [--log <日志文件>]
ocrgui --image <图片> [--out <json>] [--dump-crops <目录>] [--threads N]
```

## 2. 已完成

| 里程碑 | 内容 |
|---|---|
| M1 | 引擎骨架 + 单图识别 + CLI + `--dump-crops` |
| M2 | PDF → 可复制文字 PDF：逐页渲染 → OCR → 屏蔽原文层 → 叠加隐形文字层；并发 worker（每 worker 一个 ORT session） |
| M3-① | 页范围 `--pages`、另存为、打开输出目录、`tools/package.ps1` 打包 |
| M3-② | 低置信度复核：`--review-score` / `--csv` 导出、GUI 复核阈值滑块、`tools/review_sheet.py` 抽样拼版 PDF |
| M3-③ | 预览页签（渲染 + 识别框叠加 + 只看低置信度）、进度剩余时间、主窗口铺满 |
| M3-④ | 中文路径闪退修复（`util/path_util.h`，勿用 `fs::path` 隐式转换） |
| M3-⑤ | **断点续跑**：页级缓存 `*_ocrcache.tsv`（逐页 flush、dpi+模型+合并策略校验）+ `--resume` |
| M3-⑥ | **任务锁**：`<输出>.lock` 独占句柄（`FILE_FLAG_DELETE_ON_CLOSE`），同输出并发直接拒绝 |
| M4-① | **模型可切换**：`--preset auto/v5/v4/v3/v4s`、`--det-model/--rec-model/--cls-model`；GUI「模型」下拉；自动探测 v5→v4→v3 |
| M4-② | rec 无 metadata 时回退读字典文件（v5 用 `ppocrv5_dict.txt`；v3/v4 用 `ppocr_keys_v1.txt`） |
| M4-③ | 缓存头写入 `models=det+rec` 与 `m=<合并阈值>`，换模型/换策略自动作废 |
| M5-① | **同行碎片合并** `MergeSameRow`：溢出重叠 ≥0.6 行高、间隙 ≤ `mergeGapRatio`(0.8) 行高即合并，间隙 >0.35 行高补空格；**纯数字片段之间不合并**（避免图表轴标签 `1.05`+`1.02` 粘连） |
| M5-② | **PP-OCRv5 支持**：v5 移动版 det/rec + v5 字典，v5 成为自动探测首选 |

### 同行碎片合并要点

- 目的：v4/v5 的 det 常把一句话切成几段，直接在文字层里变成多行，复制出来是断的
- 实测合并率（v5，10 页样本）：735 → 634 行（-14%）；按页 1%~31%，表格页（3/4/9）仅 1%~9%，说明没把表格单元格粘起来
- 目录页收益最大：`B.6` + `基本传热方程` → `B.6 基本传热方程`
- 关闭方式：`--merge-gap 0` 或 GUI 取消勾选「合并同行碎片」

### 模型选择要点

- `auto`（默认）= v5 → v4 → v3 依次探测；`v4s` 为 v4 服务器版（更准但慢）
- 模型文件放 `modelDir` 即可切换；也可给绝对路径
- v3/v4/v5 的 det 输出结构、rec 解码方式一致，只是输入名不同（`p2o.*` / `DynamicDimension.*`）
- **v5 的 rec 没有 metadata 字典**，必须配 `ppocrv5_dict.txt`（18383 行，输出维度 18385 = blank + 字典 + 末尾空格）
- 缺 cls 模型会自动关闭方向分类，不影响主流程

### 实测对比（10 页样本 / 300 dpi / 4 线程 / 合并开启）

| 指标 | v3 | v4 | v5 |
|---|---|---|---|
| 行数 | 801 | 693 | 637 |
| 平均置信度 | 0.9071 | **0.9573** | 0.9499 |
| 置信度 <0.9 的行 | 269 | **98** | 114 |
| 待复核（<0.75） | 60 | **31** | 34 |
| 用时 | **29.3 s** | 50.5 s | 43.9 s |

置信度不是唯一标准，肉眼比对（第 3 页正文）：

- v5 中文标点更准（`，` 全角），v4 有 `,` 半角
- v5 没出现 v4 的重复字（v4 曾读出 `1）1设计所依据…`）
- v5 在目录页原生就把 `B.1 概述` 认成一行，v4 需要靠合并兜底

结论：**日常默认 v5**（更准且比 v4 快）；v3 保留作速度兜底；v4 置信度略高可作对照。

## 3. 待办（用户指定顺序 2 → 3 → 1 → 4；2、3、1 已完成）

### ✅ GUI 交互增强（第 4 项，2026-11-02 完成）

- 拖拽打开：`glfwSetDropCallback`，只认 `.pdf`，顺手填好默认输出路径
- 主题切换：OCR 页签「深色主题」复选框，`ApplyTheme()` 立即生效，`glClearColor` 跟随主题
- 参数记忆：`%LOCALAPPDATA%\ocrgui\settings.ini`，记住输入/输出、模型目录、DPI、线程、`--min-score`、复核阈值、预设、合并开关、页码、主题（开跑与退出时各存一次）
- 复核回灌：GUI「回灌复核 CSV」按钮 / CLI `--fix-csv <csv>`，按 `page`+`line` 覆盖文字（缓存页同样生效），配合 `--resume` 秒出
- 实测：2 页 v5 合并后 **87 行**（`--merge-gap 0` 为 125 行）；10 页 v5 637 行；CSV 回灌 2 行后复导出为修正值

### 其他 backlog

- **文字层与视觉位置微小偏移**：用 `tools/compare_page.py` 做像素级比对，检查 baseline 与字号换算（`pdf/textlayer.cpp`）
- **合并阈值自适应**：现在固定 0.8×行高；可按行内平均字宽估计，进一步减少图表/公式误粘
- **PaddleOCR-VL**：本机无 NVIDIA，显存/体积门槛高，暂不部署（结论：走 PP-OCRv5 + 复核更实际）
- 性能：16 线程无收益（受 ORT 单 session 线程限制），默认 4；卡顿→2~3
- 日志轮转：CLI `--log` 目前覆盖写，可按日期分文件
- 图片输入分支（`--image`）尚未接入 GUI

## 4. 约定与坑

- **不提交**：`test/`、`tests/`、`ocrwork/`、`*.csv`、`*.tsv`、`*.lock`、`*.onnx`、`*.yml`、`*_复核.csv`、`*_抽样.pdf`
- 源码统一 **UTF-8 无 BOM**；`pdf/ocr_pages.*`、`ocr/engine.*` 为 CRLF，`cli.cpp / main.cpp / pdf/lines_io.*` 为 LF（改文件时保持原换行）
- 中文路径：路径按 UTF-8 传，用 `util::InsertBeforeExtension` / `ParentDir`；**不要** `std::filesystem::path(std::string)`（按 ACP 解释，中文路径抛 `filesystem_error` 闪退）
- 每个 worker 独立 `ort::Session`；`perWorker = hardware_concurrency() / threads`，上限 8
- PowerShell 输出中文常乱码，读写文件、跑测试请走 Python（`io.open(..., encoding='utf-8')` + `PYTHONIOENCODING=utf-8`）
- 基线（10 页 / 300dpi / 4 线程 / v5 / 合并开）：**637 行**、待复核 34、约 44 s
- 别留常驻 python 进程（曾残留占 CPU）

## 5. 验证清单（改完必跑）

```
[ ] Release 构建通过（cmake --build）
[ ] 10 页 v5：637 行 / 待复核 34，约 44 s（4 线程）
[ ] --preset v3/v4/v5/auto 都能跑；auto 选中 v5；错的模型名报「找不到 det 模型」
[ ] --merge-gap 0 时行数应回到 735（v5 10 页）；合并只影响行数，不影响字符总数
[ ] 图表页（第 6 页）轴标签 1.05 / 1.02 不能被粘成一行
[ ] --resume 首发建缓存、二次命中；换模型或换 --merge-gap 后缓存应失效
[ ] 中途 kill → 重跑命中已落盘页
[ ] 同输出并发第二个进程报「已有任务正在处理同一输出」
[ ] 预览页签识别框与文字位置对齐
```
