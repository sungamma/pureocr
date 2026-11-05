# 第三方组件与许可（Third-Party Notices）

本项目（PDF OCR / `ocrgui`）自身以 **AGPL-3.0** 发布（原因见文末），并使用了下列第三方组件。
分发二进制（`dist/*.zip`）或源码时，请随附本文件与各组件的许可文本。

## 链接 / 随包分发的组件

| 组件 | 版本 | 许可 | 用途 | 许可文本 |
|---|---|---|---|---|
| MuPDF | 1.25.0 | **AGPL-3.0**（另有 Artifex 商业授权） | PDF 解析、渲染、文字层写回 | <https://www.gnu.org/licenses/agpl-3.0.txt>；源码 <https://github.com/ArtifexSoftware/mupdf> |
| ONNX Runtime | 1.20.1 | MIT | CPU 推理 | `third_party/onnxruntime-win-x64-1.20.1/LICENSE` |
| ONNX Runtime 第三方声明 | — | 各组件原许可 | — | `third_party/onnxruntime-win-x64-1.20.1/ThirdPartyNotices.txt` |
| Dear ImGui | 1.91.5 | MIT | 界面 | <https://github.com/ocornut/imgui/blob/master/LICENSE.txt> |
| GLFW | 3.3.9 | zlib/libpng | 窗口与 OpenGL 上下文 | <https://www.glfw.org/license> |
| stb_image / stb_image_write | 2.30 | Public Domain（也可按 MIT 使用） | 渲染结果读写 | `third_party/stb_image.h`、`third_party/stb_image_write.h` |

> Dear ImGui 与 GLFW 通过 CMake `FetchContent` 在构建时拉取，不随本仓库分发。

## 模型（运行期下载，不入库）

| 模型 | 来源 | 许可 |
|---|---|---|
| PP-OCRv5 det/rec（移动版、服务器版） | [PaddlePaddle](https://huggingface.co/PaddlePaddle) 官方 ONNX 导出；服务器版为 [bukuroo/PPOCRv5-ONNX](https://huggingface.co/bukuroo/PPOCRv5-ONNX) 转换 | Apache-2.0（上游 PaddleOCR 许可，转换版未单独声明） |
| PP-OCRv3 / v4 det/rec、方向分类 cls | [SWHL/RapidOCR](https://huggingface.co/SWHL/RapidOCR)、`rapidocr_onnxruntime` 内置模型 | Apache-2.0 |
| `ppocrv5_dict.txt`（字典） | [PaddleOCR](https://github.com/PaddlePaddle/PaddleOCR) | Apache-2.0 |

模型文件不随仓库分发，由程序首次运行时（或 `--fetch-models`）按 `models/registry.ini`
中的地址下载并用 SHA256 校验。仓库内仅保留字典与清单（Apache-2.0，附来源说明）。

## 字体

程序不打包任何字体文件：CJK 渲染使用系统已安装字体（微软雅黑 / 宋体），MuPDF 内置的
CJK 字体在构建时被剔除（`tools/slim_mupdf.ps1`），因此不存在字体资源的再分发问题。

## 为什么本项目必须是 AGPL-3.0

MuPDF 采用 AGPL-3.0（另可向 Artifex 购买商业授权）。本项目把 MuPDF **静态链接**进
`ocrgui.exe`，整个可执行文件因此被视为 AGPL-3.0 作品的组合/衍生作品。对外分发时：

1. 必须以 AGPL-3.0 提供**整个程序**的完整对应源码（不只本项目目录，还包括使其可构建的全部脚本）；
2. 必须保留各方的版权声明与许可声明（本文件 + `LICENSE`）；
3. 若通过网络向他人提供服务，按 AGPL §13 还需向使用者提供源码获取方式。

若将来希望改用 MIT / Apache-2.0 等宽松许可发布，需要先取得 Artifex 的 MuPDF 商业授权
（或换用其它 PDF 引擎，如 PDFium/BSD-3）。本项目的自有代码版权在自己手里，届时可重新许可。
