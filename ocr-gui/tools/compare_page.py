#!/usr/bin/env python
"""M1 验收工具：同一页图像分别用 C++ 与 Python(rapidocr) 识别，对齐比较.

用法：python tools/compare_page.py <pdf> <page1based> [--dpi 300]
"""
import argparse, difflib, json, os, subprocess, sys, tempfile, time

import fitz
import numpy as np
from rapidocr_onnxruntime import RapidOCR

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
EXE = os.path.join(os.environ.get("OCRGUI_EXE", r"C:\Users\xwtal\build\ocrgui\Release\ocrgui.exe"))
MODELS = os.path.join(ROOT, "models")


def overlap(a, b):
    x0 = max(a["x0"], b["x0"]); y0 = max(a["y0"], b["y0"])
    x1 = min(a["x1"], b["x1"]); y1 = min(a["y1"], b["y1"])
    return max(0.0, x1 - x0) * max(0.0, y1 - y0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pdf")
    ap.add_argument("page", type=int, help="第几页（从 1 开始）")
    ap.add_argument("--dpi", type=int, default=300)
    args = ap.parse_args()

    doc = fitz.open(args.pdf)
    pix = doc[args.page - 1].get_pixmap(dpi=args.dpi)
    tmp = tempfile.mkdtemp(prefix="ocrgui_")
    img = os.path.join(tmp, "page.png")
    out = os.path.join(tmp, "cpp.json")
    pix.save(img)

    t0 = time.time()
    r = subprocess.run([EXE, "--image", img, "--model-dir", MODELS, "--out", out],
                       capture_output=True, text=True)
    t_cpp = time.time() - t0
    if r.returncode != 0:
        print("C++ 失败:", r.returncode, r.stderr)
        return 1
    cpp = json.load(open(out, encoding="utf-8"))["lines"]

    arr = np.frombuffer(pix.samples, dtype=np.uint8).reshape(pix.height, pix.width, pix.n)[:, :, :3]
    t0 = time.time()
    res, _ = RapidOCR()(arr)
    t_py = time.time() - t0
    py = [{"x0": min(p[0] for p in box), "y0": min(p[1] for p in box),
           "x1": max(p[0] for p in box), "y1": max(p[1] for p in box), "text": t.strip()}
          for box, t, s in (res or []) if float(s) >= 0.5]

    print(f"页 {args.page} @ {args.dpi}dpi  {pix.width}x{pix.height}")
    print(f"  行数      C++ {len(cpp):4d}   Python {len(py):4d}")
    print(f"  用时      C++ {t_cpp:5.2f}s  Python {t_py:5.2f}s")
    print(f"  总字符数  C++ {sum(len(l['text']) for l in cpp):4d}   Python {sum(len(t['text']) for t in py):4d}")

    # 每个 Python 框找覆盖最好的 C++ 框，比较文本是否一致
    same = 0
    dx, dy = [], []
    for p in py:
        best, bi = None, 0.0
        for c in cpp:
            o = overlap(p, c)
            if o > bi:
                bi, best = o, c
        if best is not None and bi > 0:
            if best["text"] == p["text"]:
                same += 1
            dx.append(best["x0"] - p["x0"]); dy.append(best["y0"] - p["y0"])
    n = len(py)
    print(f"  文本一致  {same}/{n} ({same / max(n, 1):.1%})")
    sims = []
    for p in py:
        best, bi = None, 0.0
        for c in cpp:
            o = overlap(p, c)
            if o > bi:
                bi, best = o, c
        if best is not None and bi > 0:
            sims.append(difflib.SequenceMatcher(None, p["text"], best["text"]).ratio())
    if sims:
        sims.sort(); k = len(sims)
        print(f"  相似度    中位 {sims[k // 2]:.3f}  >=0.95 占比 {sum(1 for x in sims if x >= 0.95) / k:.1%}")
    if dx:
        dx.sort(); dy.sort(); k = len(dx)
        print(f"  框偏差    dx 中位 {dx[k // 2]:+.1f}px   dy 中位 {dy[k // 2]:+.1f}px")
    return 0


if __name__ == "__main__":
    sys.exit(main())
