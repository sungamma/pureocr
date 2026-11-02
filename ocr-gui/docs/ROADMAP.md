# ocr-gui 开发路线与交接备忘

> 最后更新：2026-11-02　适用分支：`c++_learn` / `main`
> 用途：换一个窗口/目录继续开发时，先读这份文档即可接上上下文。

## 1. 项目速览

| 项 | 值 |
|---|---|
| 仓库 | `D:\Dropbox\14.repo\git\c++_learn`（`ocr-gui/` 子目录） |
| 定位 | Dear ImGui 桌面界面 + 命令行，双入口；OCR 引擎为 ONNX Runtime 上的 PP-OCRv3（RapidOCR 同款流程） |
| 构建目录 | `C:\Users\xwtal\build\ocrgui`（**必须在 Dropbox 之外**，否则编译/链接报文件占用） |
| 产物 | `C:\Users\xwtal\build\ocrgui\Release\ocrgui.exe` |
| 模型 | `ocr-gui\models`：`ch_PP-OCRv3_det_infer.onnx`、`ch_PP-OCRv3_rec_infer.onnx`、`ch_ppocr_mobile_v2.0_cls_infer.onnx`、`ppocr_keys_v1.txt` |
| 测试样本 | `D:\Dropbox\14.repo\git\script\ocr\test\样本10页.pdf`（**不入库**） |
| 机器 | Windows 11 / 16 核 / 32 GB / **无 NVIDIA 显卡** |

### 构建与运行

```powershell
# 构建
cmake --build C:\Users\xwtal\build\ocrgui --config Release
# 配置（首次或改 CMakeLists 后）
cmake -S .\ocr-gui -B C:\Users\xwtal\build\ocrgui -G "Visual Studio 17 2022" -A x64
```

exe 是 **GUI 子系统**，PowerShell 里用 `&` 不会等待，必须：

```powershell
Start-Process -Wait -WindowStyle Hidden -FilePath $exe -ArgumentList @('--pdf', $pdf, '--out-pdf', $out)
```

### 命令行参数

```
ocrgui --pdf <in.pdf> [--out-pdf <out.pdf>] [--dpi 300] [--threads 4]
       [--pages 1-10,15] [--font <ttf>] [--no-mask] [--min-score 0.5]
       [--csv <导出.csv>] [--review-score 0.75] [--resume] [--cache <缓存.tsv>]
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
| M3-⑤ | **断点续跑**：页级缓存 `*_ocrcache.tsv`（逐页 flush，dpi 校验）+ `--resume` |
| M3-⑥ | **任务锁**：`<输出>.lock` 独占句柄（`FILE_FLAG_DELETE_ON_CLOSE`），同输出并发直接拒绝 |

### 断点续跑要点

- 缓存文件：`<输出>.pdf` → `<输出>_ocrcache.tsv`，格式 `# ocrgui-dpi v1 dpi=300` / `p	页	宽	高` / `l	x0	y0	x1	y1	score	文字`
- 仅当 `--resume`（GUI「断点续跑」）时读写；dpi 变化或缓存头缺失 → 视为无缓存，整份重跑
- 每页识别完成即 `fflush`，中途杀进程后重跑只补未完成页
- 缓存不随成功删除（便于反复调试）；要强制重跑就删 `*_ocrcache.tsv` 或换输出名

## 3. 待办（用户指定顺序 2 → 3 → 1 → 4；2、3 已完成）

### ① 模型可切换（下一步，未开始）

- `ocr::EngineOptions`（`src/ocr/engine.h`）目前写死 v3 三个文件名，改动点：
  - 增加 `detModel / recModel / clsModel` 可选文件名 + `keysFile`
  - `Init` 里做路径探测：先按显式名，再回退 v3/v4 目录约定
  - 加 `--det-model / --rec-model` CLI 参数与 GUI 下拉框
- 目标：PP-OCRv4 mobile / server 可切；识别人眼可见地优于 v3（用户主观要求「至少达到 Python 版水平」）
- 注意：v4 的 rec 字典、输入尺寸（3x48x320 等）与 v3 一致，但 server 版 `rec` 更大更慢，需实测 4 线程耗时
- 验收：同一 2 页样本，对比 v3/v4 的低置信度行数与肉眼正确率

### ② GUI 交互增强（未开始）

- 拖拽 PDF 到窗口直接打开（ImGui + Win32 `WM_DROPFILES`）
- 主题切换（浅色/深色）、字号缩放
- 记住上次输入/输出目录、线程数、DPI（写 `%LOCALAPPDATA%\ocrgui\settings.ini`）
- 复核闭环：CSV 改完文字后回填重生成 PDF（现在是「导出了但回不去」）

### ③ 其他 backlog

- **文字层与视觉位置微小偏移**：曾发现输出 PDF 的文字位置与实际行有偏差，需 `tools/compare_page.py` 做像素级对比，检查 baseline 与字号换算（`pdf/textlayer.cpp`）
- **PaddleOCR-VL 评估**：用户想提升识别率，但本机无 NVIDIA，VL 模型显存/体积门槛高，暂时搁置（结论：走 PP-OCRv4 + 复核更实际）
- 性能：16 线程无收益（受 ORT 单 session 线程数限制），默认 4 线程；卡顿→2~3
- 日志轮转：CLI `--log` 目前是覆盖写，可改成按日期分文件
- 图片输入分支（`--image`）尚未接入 GUI

## 4. 约定与坑

- **不提交**：`test/`、`tests/`、`ocrwork/`、`*.csv`、`*.tsv`、`*.lock`、`*_复核.csv`、`*_抽样.pdf`
- 源码统一 **UTF-8 无 BOM**；`pdf/ocr_pages.*` 为 CRLF，`cli.cpp / main.cpp / pdf/lines_io.*` 为 LF（改文件时保持原换行）
- 中文路径：所有路径 string 按 UTF-8 传，用 `util::InsertBeforeExtension` / `ParentDir`；**不要** `std::filesystem::path(std::string)`（按 ACP 解释，中文路径抛 `filesystem_error` 直接闪退）
- 每个 worker 独立 `ort::Session`；`perWorker = hardware_concurrency() / threads`，上限 8
- 调试小技巧：PowerShell 输出中文常乱码，读写文件请走 Python（`io.open(..., encoding='utf-8')` + `PYTHONIOENCODING=utf-8`）
- 基线：2 页样本 → **126 行**、待复核 3 行（置信度 < 0.75）；10 页 → 812 行、65 行待复核。改代码后先跑这个对比有没有回归
- 别留常驻 python 进程（用户机器上曾残留并占 CPU）

## 5. 验证清单（改完必跑）

```
[ ] Release 构建通过（cmake --build）
[ ] 2 页样本：126 行，输出 PDF 可复制文字
[ ] --resume 首次跑建缓存，二次跑「缓存命中 2 页 / 新识别 0 页」且耗时 <1s
[ ] 改 --dpi 后 --resume 应「缓存命中 0 页」
[ ] 中途 kill 进程 → 重跑应命中已落盘的页
[ ] 同输出并发第二个进程应报「已有任务正在处理同一输出」
[ ] 预览页签识别框与文字位置对齐
