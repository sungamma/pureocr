# PureOCR

把扫描版 PDF 变成**可搜索、可复制**的 PDF：逐页渲染 → PP-OCRv5/v4/v3 识别 → 只向页面追加
一层**隐形文字层**，原始页面（图像、字体、编码）一律不动。

- **Windows 10/11 x64**，纯 CPU（ONNX Runtime），无需显卡
- 图形界面 + 命令行双入口，支持**整目录批量**（可递归）、断点续跑、复核 CSV 导出/回灌
- 输出自检：墨迹对比 + 可见字符比对，发现空白页/文字丢失会直接报错并删除坏产物
- **Linux x64**：CLI 与 GUI 均可编译运行（需 libcurl/OpenSSL 与 CJK 字体；GUI 需 X11/Wayland）
- **macOS**：尚未支持，仅有编译探测（见 `docs/CROSS_PLATFORM.md`）
- 版本号唯一来源：仓库根目录 `VERSION`，构建时注入 exe 资源、窗口标题与 `--version`

## 构建

```powershell
# 1) 依赖（不入库）：下载 ONNX Runtime 1.20.1 + 编译 MuPDF 1.25.0 到 third_party
pwsh -NoProfile -File tools\fetch_deps.ps1

# 2) 模型（不入库，约 40 MB）：首次运行时在线下载，也可以提前拉
#    dist\pureocr-*\pureocr.exe --fetch-models --preset v5

# 3) 一键发布（编译 + 打包 + 生成 SHA256）；版本默认读 VERSION，可用 -Version 覆盖
pwsh -NoProfile -File tools\release.ps1 -Clean -Models basic
```

Linux x64：

```bash
# 1) 依赖（下载 ONNX Runtime linux-x64 + 静态编译 MuPDF 到 third_party/）
bash tools/fetch_deps_linux.sh

# 2) 系统库
sudo apt-get install -y build-essential cmake ninja-build pkg-config \
     libcurl4-openssl-dev libssl-dev libgl1-mesa-dev xorg-dev \
     libfontconfig1-dev fonts-noto-cjk

# 3) 构建 + 打包 tar.gz（含 pureocr 与 libonnxruntime.so）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
bash tools/package_linux.sh --build-dir build --models basic
```

需要的工具：Visual Studio 2022（桌面 C++）、CMake ≥ 3.20、PowerShell 7、Git。
GLFW 3.3.9 与 Dear ImGui 1.91.5 由 CMake `FetchContent` 在配置阶段自动拉取。

CI：`.github/workflows/build.yml`（GitHub Actions）——Windows 与 Linux 双平台编译 + 打包；
macOS 目前只是编译探测（不阻塞），见 `docs/CROSS_PLATFORM.md`。

## 用法

界面：双击 `pureocr.exe`，选 PDF 或文件夹（可直接拖入），设好参数后「开始识别」。

命令行：

```powershell
pureocr.exe --pdf <in.pdf> [--out-pdf <out.pdf>]     # 单文件
pureocr.exe -f <目录> [-r] [--out-dir <输出目录>]     # 整目录批量（-r 含子目录）
pureocr.exe --fetch-models --preset v5               # 下载模型
```

默认参数：2 线程 / 300 dpi / 最低置信度 0.55 / 同行碎片合并 0.8。

## 版本号与发布

- 版本号只写在 `VERSION`（如 `0.13.0`），CMake 会生成 `version.h`，供 exe 资源、窗口标题与 `--version` 使用。
- 本地打包：`pwsh -File tools\release.ps1 -Clean`（Windows）、`bash tools/package_linux.sh`（Linux）。
- 发布到 GitHub Release：改好 `VERSION` 提交，然后打 tag 并推送：

```bash
git tag v0.13.0
git push origin v0.13.0
```

  CI 会校验 tag 与 `VERSION` 一致，构建 Windows zip 与 Linux tar.gz，并一起上传到该 Release。
  也可在 Actions 页面手动触发 `build` 工作流并勾选发布。

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
- `docs/HISTORY.md`：早期开发历程（历史记录）
- `models/README.md`：模型来源与校验值
- `使用说明.txt`：发布包内的用户说明（打包时生成）

## 许可

- 本项目：**AGPL-3.0**（见 `LICENSE`）
- 原因：静态链接了 **MuPDF**（AGPL-3.0），因此整个程序按 AGPL-3.0 发布
- 第三方组件、模型与字体说明：见 `THIRD_PARTY_NOTICES.md`

如果需要在闭源或 MIT/Apache 许可下使用，需先取得 Artifex 的 MuPDF 商业授权。