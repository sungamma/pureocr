#!/usr/bin/env python
"""从 rapidocr_onnxruntime 自带模型拷 PP-OCR 模型到 ocr-gui/models/。

v4 移动版识别率明显高于 v3，推荐日常使用；本地包里没有 v4 时自动从
RapidOCR 的 HuggingFace 仓库下载（国内可用 HF_ENDPOINT=https://hf-mirror.com）。

模型体积较大，首次使用前执行一次：
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
    "ch_PP-OCRv4_det_infer.onnx",
    "ch_PP-OCRv4_rec_infer.onnx",
]
# 可选：v4 服务器版（更准但慢 3~10 倍）
SERVER_FILES = [
    "ch_PP-OCRv4_det_server_infer.onnx",
    "ch_PP-OCRv4_rec_server_infer.onnx",
]
HF_BASE = "https://huggingface.co/SWHL/RapidOCR/resolve/main/PP-OCRv4/"


def download(url, dst):
    import urllib.request
    urllib.request.urlretrieve(url, dst + ".part")
    os.replace(dst + ".part", dst)


def main():
    try:
        import rapidocr_onnxruntime
    except ImportError:
        print("未安装 rapidocr-onnxruntime：pip install rapidocr-onnxruntime")
        return 1
    src = os.path.join(os.path.dirname(rapidocr_onnxruntime.__file__), "models")
    os.makedirs(DST, exist_ok=True)
    files = list(FILES)
    if "--server" in sys.argv:
        files += SERVER_FILES
    for f in files:
        dst = os.path.join(DST, f)
        if os.path.exists(dst) and os.path.getsize(dst) > 100000:
            print("exists", f)
            continue
        s = os.path.join(src, f)
        if os.path.exists(s):
            shutil.copy2(s, dst)
            print("ok", f)
            continue
        print("下载", f)
        try:
            download(HF_BASE + f, dst)
            print("ok", f)
        except Exception as e:  # 网络不通时给出明确提示
            print("下载失败", f, e)
            print("可手动把 %s 放到 %s" % (f, DST))
            return 1
    print("模型已就位:", DST)
    return 0


if __name__ == "__main__":
    sys.exit(main())
