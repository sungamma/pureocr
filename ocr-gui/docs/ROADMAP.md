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
| 模型 | `ocr-gui\models`（不入库）：v3 / v4 / v5 移动版 + 方向分类 + `ppocrv5_dict.txt + v5 服务器版（`--preset v5s`） |
| 测试样本 | `D:\Dropbox\14.repo\git\script\ocr\test\样本10页.pdf`（**不入库**） |
| 机器 | Windows 11 / 16 核 / 32 GB / **无 NVIDIA 显卡** |

### 构建与运行

```powershell
cmake --build C:\Users\xwtal\build\ocrgui --config Release
# 首次或改 CMakeLists 后：
cmake -S .\ocr-gui -B C:\Users\xwtal\build\ocrgui -G "Visual Studio 17 2022" -A x64
ocrgui.exe --fetch-models            # 拉 v3/v4/v5（缺的自动从 HF 下载）
ocrgui.exe --fetch-models --preset v5s   # 额外拉 v4 服务器版
```

exe 是 **GUI 子系统**，PowerShell 里 `&` 不会等待，必须：

```powershell
Start-Process -Wait -WindowStyle Hidden -FilePath $exe -ArgumentList @('--pdf', $pdf, '--out-pdf', $out)
```

### 命令行参数

```
ocrgui --pdf <in.pdf> [--out-pdf <out.pdf>] [--dpi 300] [--threads 2]
       [--pages 1-10,15] [--font <ttf>] [--mask] [--min-score 0.55]
       [--csv <导出.csv>] [--review-score 0.75] [--resume] [--cache <缓存.tsv>]
       [--preset auto|v5|v5s|v4|v4s|v3] [--det-model <文件名|绝对路径>]
       [--rec-model <文件名|绝对路径>] [--cls-model <文件名>]
       [--merge-gap 0.8]   # 同行碎片合并阈值（×行高），0 = 关闭
       [--model-dir <模型目录>] [--log <日志文件>]
       # 省略 --out-pdf 时输出为 <原名>_OCR.pdf
ocrgui --pdf <目录>（也可 --dir / -f） [--out-dir <输出目录>] [--recurse|-r] [--overwrite]
       # 批量：目录下所有 PDF -> <原名>_OCR.pdf
       # 跳过 *_OCR.pdf 本身，以及已存在 <原名>_OCR.pdf 的原文件（源目录/输出目录都算）
ocrgui --image <图片> [--out <json>] [--dump-crops <目录>] [--threads N]
```

## 2. 已完成

| 里程碑 | 内容 |
|---|---|
| M1 | 引擎骨架 + 单图识别 + CLI + `--dump-crops` |
| M2 | PDF → 可复制文字 PDF：逐页渲染 → OCR → 叠加隐形文字层（默认不屏蔽原文层，`--mask` 才屏蔽）；并发 worker（每 worker 一个 ORT session） |
| M3-① | 页范围 `--pages`、另存为、打开输出目录、`tools/package.ps1` 打包 |
| M3-② | 低置信度复核：`--review-score` / `--csv` 导出、GUI 复核阈值滑块、`tools/review_sheet.py` 抽样拼版 PDF |
| M3-③ | 预览页签（渲染 + 识别框叠加 + 只看低置信度）、进度剩余时间、主窗口铺满 |
| M3-④ | 中文路径闪退修复（`util/path_util.h`，勿用 `fs::path` 隐式转换） |
| M3-⑤ | **断点续跑**：页级缓存 `*_ocrcache.tsv`（逐页 flush、dpi+模型+合并策略校验）+ `--resume` |
| M3-⑥ | **任务锁**：`<输出>.lock` 独占句柄（`FILE_FLAG_DELETE_ON_CLOSE`），同输出并发直接拒绝 |
| M4-① | **模型可切换**：`--preset auto/v5/v4/v3/v4s`、`--det-model/--rec-model/--cls-model`；GUI「模型」下拉；自动探测 v5→v4→v3 |
| M4-② | rec 无 metadata 时回退读字典文件（v5 用 `ppocrv5_dict.txt`；v3/v4 用 `ppocr_keys_v1.txt`） |
| M4-③ | 缓存头写入 `models=det+rec` 与 `m=<合并阈值>`，换模型/换策略自动作废 |
| M4-④ | **批量模式**：`--pdf <目录>` / `--dir` 整目录处理（`--recurse` 递归、`--out-dir` 指定输出目录、`--overwrite` 重做）；默认参数定为 2 线程 / 300 dpi / 置信度 0.55 / 合并 0.8 |
| M4-⑤ | **GUI 批量**：侧栏「选择文件夹…」或直接把文件夹拖进窗口；「含子目录」「覆盖已有结果」开关；结果写各源文件同目录 `<原名>_OCR.pdf`，跳过 `*_OCR.pdf` 与已存在的产物 |
| M5-① | **同行碎片合并** `MergeSameRow`：溢出重叠 ≥0.6 行高、间隙 ≤ `mergeGapRatio`(0.8) 行高即合并，间隙 >0.35 行高补空格；**纯数字片段之间不合并**（避免图表轴标签 `1.05`+`1.02` 粘连） |
| M5-② | **PP-OCRv5 支持**：v5 移动版 det/rec + v5 字典，v5 成为自动探测首选 |
| M6-① | **界面布局修正**：两个选择按钮等宽平分；参数两列改整数列坐标（文字不再半像素发虚）；滑块数值移到右侧（把手不再压住 0.55）；勾选框行对齐；日志按宽度自动折行；右上角「OCR 产物/原文件」不再画到窗口外被裁掉 |
| M6-② | **字形覆盖**：字体只打包常用 2500 字 + 符号区间，文案里的生僻字（屏/崩/溃/嵌/帧…）以前会画成方框（看着像乱码），现在已补进 glyph ranges |

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

### PP-OCRv5 服务器版（`--preset v5s`，可选）

| 指标（10 页 / 300dpi / 4 线程 / 合并开） | v5 移动版 | v5 服务器版 |
|---|---|---|
| 行数 | **637** | 614 |
| 平均置信度 | 0.9499 | **0.9523** |
| 置信度 <0.9 | 114 | **101** |
| 待复核（<0.75） | 34 | **25** |
| 用时 | **43.9 s** | 290.5 s（×6.6） |

- 2 页样本：v5 移动版 87 行 / 0.9776 / 2 个待复核 / 9.6 s；v5s 93 行 / 0.9811 / 0 个 / 36.4 s
- 结论：**定稿可用 v5s，批量仍用 v5**；CPU 上服务器版代价约 4~7 倍
- 模型来源与校验详见 `models/README.md`（与 PaddleX 权重数值一致）

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
- **PDF `/Contents` 必须保留"间接引用"**：把 `pdf_resolve_indirect` 之后的流对象塞进新的 Contents 数组，保存时它只能被写成字典（读取报 `object is not a stream`），整页内容随之丢失 → 成品全白。单流 `/Contents` 的 PDF 必现（`测试/test.pdf` 就是）。改这段必须「数组按元素拷贝、单流塞回原引用」
- **屏蔽原文字体只能作用于本次 OCR 的页**：`MaskOriginalText` 原来遍历整个文档，只 OCR 前几页会把其余页的可复制文本一起清成 U+200B（看得见、复制全空）。现在先收集「本次不处理页」用到的字体并跳过；几页共享同一字体对象时仍无法拆开，属于既有取舍
- **默认不改原文字体，只叠加隐形文本层**：唯一写点是页面的 `/Contents`（追加一个内容流），原字体/编码/字形一律不碰。历史两次翻车（整页空白、非 OCR 页文本被清空）都出在改原文字体上，所以默认零改动。需要“替换”效果时显式打开 `--mask`（GUI 复选框「用 OCR 文本替换原文」），且仅当**整篇都 OCR** 时生效；只 OCR 部分页会自动跳过屏蔽并在日志提示。
- **自检两条线**：墨迹对比抓空白页；未开 `--mask` 时额外比对成品与原页的可见字符数（原页 ≥20 字且成品 <50% 判 textLoss），防止叠层把原文挤掉。
- **别用 `freopen_s` 重定向日志**：它按 ANSI 代码页解释 `char*` 路径，中文日志名打不开；更糟的是打开失败时 `stdout` 会被关掉，之后 `printf` 触发「无效参数」直接 `abort`，进程闪退（`0xC0000409`）。现在走 `util::OpenUtf8` + `_dup2`。
- **断点续跑缓存是中间产物**：`<输出前缀>_ocrcache.tsv` 只在「中途挂掉」时有用，整篇成功跑完会自动删除（`--keep-cache`、只处理部分页、导出/回灌复核 CSV 时保留）。GUI 里这就是「断点续跑」勾选框的行为。
- **控件坐标不能带小数**：ImGui 里文字落在 x.5 上会被抗锯齿摊到两列像素，看起来又糊又灰（像乱码）。中文标签宽度普遍是小数，用 `SameLine` 接在控件后面很容易累积出半像素坐标；两列布局改成手算整数列坐标 + `BeginGroup/SameLine`。
- **滑块数值别写在滑块里**：把手跟着值走，正好压住居中的数字（0.55 看着像 0.5）。数值改成画在滑块右侧。
- **字体只打包「常用 2500 字」**：文案里出现表外字会画成方框。新加文案时跑一下字形检查（`AddText` 白名单里补字，符号用整段区间）。
- 凡是"写回 PDF"的改动**必须有输出自检兜底**（见第 5 节），历史上出过一次静默产出空白 PDF


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
[ ] `--preset v5s` 可用（两个服务器版模型，已校验）
[ ] `--preset v5 --det-model/--rec-model` 显式指定时不被预设覆盖
[ ] 预览页签识别框与文字位置对齐
[ ] 单流 /Contents 的 PDF（`测试/test.pdf`，3 页）跑完：成品有字，日志出现「输出自检：3 页内容完整」
[ ] 只 OCR 部分页（`--pages 1`）：未处理页文本仍可正常提取（不能出现 U+200B）
- [x] 整目录批量（`--dir`/`-f`，2 线程 / 300dpi / 0.55 / 0.8）跑通，中文文件名与中文日志名都不崩
[ ] 批量模式：`--pdf <目录> --recurse` 处理子目录；二次运行全部「跳过」；`*_OCR.pdf` 不被当成输入；`--out-dir` 生效
[ ] GUI 批量：选文件夹（可勾「含子目录」）连跑两个 PDF 不崩；第二次运行因产物已存在全部跳过
[ ] 断点续跑：整篇跑完缓存 `*_ocrcache.tsv` 自动删除；`--keep-cache` / 部分页 / 复核回灌时保留
[ ] 自检能抓坏输出：拿历史空白产物做墨迹对比，应判 LOST 并删掉坏文件
[ ] 默认（无 `--mask`）：成品页上原文与 OCR 文本并存，原文页可见字符数不下降（自检不报 textLoss）
[ ] `--mask --pages 1`：日志出现「部分页处理：已自动跳过 --mask」，未处理页字符数与原件逐页一致

```
