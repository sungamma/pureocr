# 模型目录

程序默认从 `ocr-gui/models`（或 exe 同级的 `models/`）加载，文件名固定，放哪个版本就识别哪一版；
`auto` 模式按 **v5 → v4 → v3** 顺序探测。重新拉取：`python tools/fetch_models.py`。

| 文件 | 用途 | 大小 | 来源 |
|---|---|---|---|
| `ch_PP-OCRv5_det_infer.onnx` | v5 文本检测 | 4.6 MB | [PaddlePaddle/PP-OCRv5_mobile_det_onnx](https://huggingface.co/PaddlePaddle/PP-OCRv5_mobile_det_onnx) |
| `ch_PP-OCRv5_rec_infer.onnx` | v5 文本识别 | 15.8 MB | [PaddlePaddle/PP-OCRv5_mobile_rec_onnx](https://huggingface.co/PaddlePaddle/PP-OCRv5_mobile_rec_onnx) |
| `ppocrv5_dict.txt` | v5 字典（18383 行） | 72 KB | [PaddleOCR ppocrv5_dict.txt](https://github.com/PaddlePaddle/PaddleOCR) |
| `ppocrv5_inference.yml` | v5 预处理配置（备查） | 145 KB | 同上 rec 仓库 `inference.yml` |
| `ch_PP-OCRv4_det_infer.onnx` | v4 文本检测 | 4.5 MB | [SWHL/RapidOCR](https://huggingface.co/SWHL/RapidOCR) `PP-OCRv4/` |
| `ch_PP-OCRv4_rec_infer.onnx` | v4 文本识别 | 10.4 MB | 同上 |
| `ch_PP-OCRv3_det_infer.onnx` | v3 文本检测 | 2.3 MB | `rapidocr_onnxruntime` 内置模型包 |
| `ch_PP-OCRv3_rec_infer.onnx` | v3 文本识别 | 10.2 MB | 同上 |
| `ch_ppocr_mobile_v2.0_cls_infer.onnx` | 方向分类（0/180°） | 0.6 MB | 同上 |

合计约 48 MB（已入库，方便换机器直接跑；如需瘦身可只保留一个版本）。

## 说明

- v3/v4 的 rec 模型自带 `character` metadata 字典；**v5 没有**，必须配 `ppocrv5_dict.txt`
  （输出维度 18385 = blank + 18383 字典项 + 末尾空格）
- v4 服务器版（`*_server_infer.onnx`，更准但慢 3~10 倍）未入库，需要时 `python tools/fetch_models.py --server`
- 模型版权归 PaddleOCR 项目（Apache-2.0），此处仅为便于本机运行而随仓库分发
