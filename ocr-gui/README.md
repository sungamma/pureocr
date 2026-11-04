# PDF OCR（`ocrgui`）

把扫描版 PDF 变成**可搜索、可复制**的 PDF：逐页渲染 → PP-OCRv5/v4/v3 识别 → 只向页面追加
一层**隐形文字层**，原始页面（图像、字体、编码）一律不动。

- **Windows 10/11 x64**，纯 CPU（ONNX Runtime），无需显卡
- 图形界面 + 命令行双入口，支持**整目录批量**（可递归）、断点续跑、复核 CSV 导出/回灌
- 输出自检：墨迹对比 + 可见字符比对，发现空白页/文字丢失会直接报错并删除坏产物
- Linux / macOS **尚未支持**，现状与移植清单见 `docs/CROSS_PLATFORM.md`

## 构建

```powershell
# 1) 依赖（不入库）：下载 ONNX Runtime 1.20.1 + 编译 MuPDF 1.25.0 到 ..\third_party
pwsh -NoProfile -File tools\fetch_deps.ps1

# 2) 模型（不入库，约 40 MB）：首次运行时在线下载，也可以提前拉
#    dist\ocrgui-*\ocrgui.exe --fetch-models --preset v5

# 3) 一键发布（编译 + 打包 + 生成 SHA256，构建目录放 %TEMP%，完成后自动清理）
pwsh -NoProfile -File tools\release.ps1 -Clean -Version 20261104
```

需要的工具：Visual Studio 2022（桌面 C++）、CMake ≥ 3.20、PowerShell 7、Git。
GLFW 3.3.9 与 Dear ImGui 1.91.5 由 CMake `FetchContent` 在配置阶段自动拉取。

CI：`.github/workflows/build.yml`（GitHub Actions）——Windows 上编译 + 打包 zip；
Linux / macOS 目前只是编译探测，见 `docs/CROSS_PLATFORM.md`。

## 用法

界面：双击 `ocrgui.exe`，选 PDF 或文件夹（可直接拖入），设好参数后「开始识别」。

命令行：

```powershell
ocrgui.exe --pdf <in.pdf> [--out-pdf <out.pdf>]     # 单文件
ocrgui.exe -f <目录> [-r] [--out-dir <输出目录>]     # 整目录批量（-r 含子目录）
ocrgui.exe --fetch-models --preset v5               # 下载模型
```

默认参数：2 线程 / 300 dpi / 最低置信度 0.55 / 同行碎片合并 0.8。

## 仓库里有什么、没有什么

| 内容 | 是否入库 | 原因 |
|---|---|---|
| 源码、脚本、`docs/` | ✅ | — |
| `third_party/onnxruntime-win-x64-1.20.1/`（include + dll/lib + 许可） | ✅ | MIT，允许再分发 |
| `third_party/stb_image*.h` | ✅ | Public Domain / MIT |
| `models/registry.ini`、`models/ppocrv5_dict.txt` | ✅ | 清单与字典，Apache-2.0 |
| `models/*.onnx` | ❌ | 约 212 MB，仓库只放清单，运行期按 SHA256 下载 |
| `third_party/mupdf-1.25.0/`（`libmupdf.lib` 约 550 MB） | ❌ | 体积过大，`tools/fetch_deps.ps1` 现场编译 |
| GLFW / Dear ImGui | ❌ | CMake `FetchContent` 构建时拉取 |

## 文档

- `docs/ROADMAP.md`：实现细节、踩过的坑、验证清单
- `docs/CROSS_PLATFORM.md`：Linux / macOS 移植清单
- `models/README.md`：模型来源与校验值
- `使用说明.txt`：发布包内的用户说明（打包时生成）

## 许可

- 本项目：**AGPL-3.0**（见 `LICENSE`）
- 原因：静态链接了 **MuPDF**（AGPL-3.0），因此整个程序按 AGPL-3.0 发布
- 第三方组件、模型与字体说明：见 `THIRD_PARTY_NOTICES.md`

如果需要在闭源或 MIT/Apache 许可下使用，需先取得 Artifex 的 MuPDF 商业授权。