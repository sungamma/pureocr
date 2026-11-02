#!/usr/bin/env python
"""准备 ocr-gui 需要的 PP-OCR 模型（v3 / v4 / v5 + 方向分类 + v5 字典）。

优先从 rapidocr_onnxruntime 本地包里拷，缺的从 HuggingFace 下载
（国内可用 HF_ENDPOINT=https://hf-mirror.com）。

    python tools/fetch_models.py            # v3 + v4 + v5 移动版
    python tools/fetch_models.py --server   # 额外下 v4 服务器版（更准但慢）
"""
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DST = os.path.join(os.path.dirname(HERE), "models")

# (本地文件名, RapidOCR 包内文件名, HF 下载地址)
FROM_PKG = [
    ("ch_PP-OCRv3_det_infer.onnx", "ch_PP-OCRv3_det_infer.onnx"),
    ("ch_PP-OCRv3_rec_infer.onnx", "ch_PP-OCRv3_rec_infer.onnx"),
    ("ch_ppocr_mobile_v2.0_cls_infer.onnx", "ch_ppocr_mobile_v2.0_cls_infer.onnx"),
]
RAPIDOCR_HF = "https://huggingface.co/SWHL/RapidOCR/resolve/main/"
PADDLE_HF = "https://huggingface.co/PaddlePaddle/"
FROM_HF = [
    ("ch_PP-OCRv4_det_infer.onnx", RAPIDOCR_HF + "PP-OCRv4/ch_PP-OCRv4_det_infer.onnx"),
    ("ch_PP-OCRv4_rec_infer.onnx", RAPIDOCR_HF + "PP-OCRv4/ch_PP-OCRv4_rec_infer.onnx"),
    ("ch_PP-OCRv5_det_infer.onnx", PADDLE_HF + "PP-OCRv5_mobile_det_onnx/resolve/main/inference.onnx"),
    ("ch_PP-OCRv5_rec_infer.onnx", PADDLE_HF + "PP-OCRv5_mobile_rec_onnx/resolve/main/inference.onnx"),
    # v5 的字典和 v3/v4 完全不同，rec 模型里没有 metadata，必须配这个文件
    ("ppocrv5_dict.txt",
     "https://raw.githubusercontent.com/PaddlePaddle/PaddleOCR/main/ppocr/utils/dict/ppocrv5_dict.txt"),
]
FROM_HF_SERVER = [
    ("ch_PP-OCRv4_det_server_infer.onnx",
     RAPIDOCR_HF + "PP-OCRv4/ch_PP-OCRv4_det_server_infer.onnx"),
    ("ch_PP-OCRv4_rec_server_infer.onnx",
     RAPIDOCR_HF + "PP-OCRv4/ch_PP-OCRv4_rec_server_infer.onnx"),
]


def download(url, dst):
    import urllib.request
    urllib.request.urlretrieve(url, dst + ".part")
    os.replace(dst + ".part", dst)


def main():
    os.makedirs(DST, exist_ok=True)
    src = None
    try:
        import rapidocr_onnxruntime
        src = os.path.join(os.path.dirname(rapidocr_onnxruntime.__file__), "models")
    except ImportError:
        print("提示：未安装 rapidocr-onnxruntime，v3 模型将改为网络下载")

    for dst_name, pkg_name in FROM_PKG:
        dst = os.path.join(DST, dst_name)
        if os.path.exists(dst) and os.path.getsize(dst) > 100000:
            print("exists", dst_name)
            continue
        cand = os.path.join(src, pkg_name) if src else None
        if cand and os.path.exists(cand):
            shutil.copy2(cand, dst)
            print("ok", dst_name)
        elif "cls" in dst_name:
            print("跳过（可选模型）", dst_name)
        else:
            print("缺少", dst_name, "（请手动放入 models/）")

    files = list(FROM_HF) + (FROM_HF_SERVER if "--server" in sys.argv else [])
    for dst_name, url in files:
        dst = os.path.join(DST, dst_name)
        if os.path.exists(dst) and os.path.getsize(dst) > 100000:
            print("exists", dst_name)
            continue
        print("下载", dst_name)
        try:
            download(url, dst)
            print("ok", dst_name)
        except Exception as e:  # 网络不通时给出明确提示
            print("下载失败", dst_name, e)
            print("可手动下载 %s 放到 %s" % (url, DST))
            return 1
    print("模型已就位:", DST)
    return 0


if __name__ == "__main__":
    sys.exit(main())
