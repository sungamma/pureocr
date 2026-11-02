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
| 模型 | `ocr-gui\models`（不入库）：v3 移动版、v4 移动版、cls 方向分类；v4 server 可选 |
| 测试样本 | `D:\Dropbox\14.repo\git\script\ocr\test\样本10页.pdf`（**不入库**） |
| 机器 | Windows 11 / 16 核 / 32 GB / **无 NVIDIA 显卡** |

### 构建与运行

```powershell
cmake --build C:\Users\xwtal\build\ocrgui --config Release
# 首次或改 CMakeLists 后：
cmake -S .\ocr-gui -B C:\Users\xwtal\build\ocrgui -G "Visual Studio 17 2022" -A x64
python tools\fetch_models.py          # 拉取模型（含 v4，缺 v4 时自动从 HF 镜像下载）
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
       [--preset auto|v3|v4|v4s] [--det-model <文件名|绝对路径>]
       [--rec-model <文件名|绝对路径>] [--cls-model <文件名>]
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
| M3-⑤ | **断点续跑**：页级缓存 `*_ocrcache.tsv`（逐页 flush、dpi+模型校验）+ `--resume` |
| M3-⑥ | **任务锁**：`<输出>.lock` 独占句柄（`FILE_FLAG_DELETE_ON_CLOSE`），同输出并发直接拒绝 |
| M4-① | **模型可切换**：`--preset auto/v3/v4/v4s`、`--det-model/--rec-model/--cls-model`；GUI「模型」下拉；自动探测 v4→v3 |
| M4-② | rec 无 metadata 时回退读字典文件（`ppocr_keys_v1.txt` 等，Paddle 约定 blank + 字典 + 末尾空格） |
| M4-③ | 缓存头写入模型标识（`models=det+rec`），换模型/换 dpi 时旧缓存自动作废 |

### 断点续跑要点

- 缓存文件：`out.pdf` → `out_ocrcache.tsv`，头部 `# ocrgui-cache v1 dpi=300 models=<det>+<rec>`
  （`p\t页\t宽\t高` 起一页，`l\tx0\ty0\tx1\ty1\tscore\t文字` 每行文字）
- 仅 `--resume` / GUI「断点续跑」时读写；dpi 或模型不符 → 删缓存重跑
- 每页识别完成即 `fflush`，中途杀进程后重跑只补未完成页；成功后缓存保留（便于复跑对比）

### 模型选择要点

- `auto`（默认）= v4 移动版优先，找不到再退回 v3；`v4s` 为服务器版（更准但慢 3~10 倍）
- 模型文件放 `modelDir` 即可切换；也可给绝对路径
- v4 与 v3 的 det 输出结构、rec 输出维度（6625）与 metadata 字典一致，仅输入名不同（`p2o.DynamicDimension.*`），引擎无需分支

### 2 页样本实测（300 dpi / 2 线程）

| 指标 | v3 | v4 |
|---|---|---|
| 行数 | 126 | 127 |
| 平均置信度 | 0.9396 | **0.9953** |
| 待复核（<0.75） | 3 | **0** |
| 置信度 <0.9 的行 | 32 | **0** |
| 用时 | 6.5 s | 10.3 s |
| 肉眼 | 漏「中」、`华人民共和国国家标准` | 全中，且补回标题、目录漏行 |

结论：v4 明显更强，默认用 v4；v3 作为速度/兼容兜底。

## 3. 待办（用户指定顺序 2 → 3 → 1 → 4；2、3、1 已完成）

### ④ GUI 交互增强（下一步）

- 拖拽 PDF 到窗口直接打开（Win32 `WM_DROPFILES`）
- 主题切换（浅色/深色）、字号缩放
- 记住上次输入/输出目录、线程数、DPI、模型预设（`%LOCALAPPDATA%\ocrgui\settings.ini`）
- 复核闭环：CSV 改完文字后回填重生成 PDF（目前「导出但没有回灌」）

### 其他 backlog

- **同行碎片合并**：v4 的 det 更碎，会把 `热交换器` 切成 `热交换`+`器`、`B.1`+`概述` 两行。
  实测同一行相邻片段的 `gap/行高` 分布（2 页样本）：标题 0.16、目录「编号-标题」0.78~1.48、
  目录「标题-页码」29~37。若要合并，阈值取 **0.6~0.8** 并插入空格，
  但必须在真实表格页上验证不会把不同单元格粘在一起
- **文字层与视觉位置微小偏移**：用 `tools/compare_page.py` 做像素级比对，检查 baseline 与字号换算（`pdf/textlayer.cpp`）
- **PaddleOCR-VL**：本机无 NVIDIA，显存/体积门槛高，暂不部署（结论：走 PP-OCRv4 + 复核更实际）
- 性能：16 线程无收益（受 ORT 单 session 线程限制），默认 4；卡顿→2~3
- 日志轮转：CLI `--log` 目前覆盖写，可按日期分文件
- 图片输入分支（`--image`）尚未接入 GUI

## 4. 约定与坑

- **不提交**：`test/`、`tests/`、`ocrwork/`、`*.csv`、`*.tsv`、`*.lock`、`*.onnx`、`*_复核.csv`、`*_抽样.pdf`
- 源码统一 **UTF-8 无 BOM**；`pdf/ocr_pages.*`、`ocr/engine.*` 为 CRLF，`cli.cpp / main.cpp / pdf/lines_io.*` 为 LF（改文件时保持原换行）
- 中文路径：路径按 UTF-8 传，用 `util::InsertBeforeExtension` / `ParentDir`；**不要** `std::filesystem::path(std::string)`（按 ACP 解释，中文路径抛 `filesystem_error` 闪退）
- 每个 worker 独立 `ort::Session`；`perWorker = hardware_concurrency() / threads`，上限 8
- PowerShell 输出中文常乱码，读写文件、跑测试请走 Python（`io.open(..., encoding='utf-8')` + `PYTHONIOENCODING=utf-8`）
- 基线（2 页 / 300dpi / v4）：**127 行**、待复核 0；v3 为 126 行、待复核 3
- 别留常驻 python 进程（曾残留占 CPU）

## 5. 验证清单（改完必跑）

```
[ ] Release 构建通过（cmake --build）
[ ] 2 页样本 v4：127 行，待复核 0；v3：126 行，待复核 3
[ ] --preset v3/v4/auto 都能跑；auto 选中 v4；错的模型名给出「找不到 det 模型」
[ ] --resume 首发建缓存、二次「缓存命中 2 页 / 新识别 0 页」且 <1s
[ ] 换模型或换 dpi 后 --resume 应「缓存命中 0 页」并重写缓存头
[ ] 中途 kill → 重跑命中已落盘页
[ ] 同输出并发第二个进程报「已有任务正在处理同一输出」
[ ] 预览页签识别框与文字位置对齐
```
