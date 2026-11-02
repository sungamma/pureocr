#!/usr/bin/env python
"""把 rapidocr_onnxruntime 自带的 PP-OCRv3 模型拷到 ocr-gui/models/.

模型不入库（体积 + 许可），首次运行前执行一次：
    python tools/fetch_models.py
"""
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DST = os.path.join(os.path.dirname(HERE), "models")
FILES = [
    "ch_PP-OCRv3_det_infer.onnx",
    "ch_PP-OCRv3_rec_infer.onnx",
    "ch_ppocr_mobile_v2.0_cls_infer.onnx",
]


def main():
    try:
        import rapidocr_onnxruntime
    except ImportError:
        print("未安装 rapidocr-onnxruntime：pip install rapidocr-onnxruntime")
        return 1
    src = os.path.join(os.path.dirname(rapidocr_onnxruntime.__file__), "models")
    os.makedirs(DST, exist_ok=True)
    for f in FILES:
        s = os.path.join(src, f)
        if not os.path.exists(s):
            print("缺少", s)
            return 1
        shutil.copy2(s, os.path.join(DST, f))
        print("ok", f)
    print("模型已就位:", DST)
    return 0


if __name__ == "__main__":
    sys.exit(main())
