#!/usr/bin/env python3
"""M2 验收：把 C++ 产出的「可复制文字 PDF」与原始 PDF / Python 版产物做对比。

用法：
    python verify_pdf.py <原始.pdf> <C++产物.pdf> [--reference <Python产物.pdf>] [--dpi 150]

检查项：
    1. 页数一致
    2. 渲染逐像素一致（对比原始文件）
    3. 文字层可提取，字符数与 Python 版对比
    4. 词框位置偏差（与 Python 版对齐后的中位数 / P95）
"""
from __future__ import annotations

import argparse
import difflib
import sys
from statistics import median

import fitz
import numpy as np

ZERO_WIDTH = "\u200b"


def render(doc: fitz.Document, index: int, dpi: int) -> np.ndarray:
    pm = doc[index].get_pixmap(dpi=dpi, colorspace=fitz.csRGB)
    return np.frombuffer(pm.samples, dtype=np.uint8).reshape(pm.height, pm.width, pm.n)


def clean_text(s: str) -> str:
    return s.replace(ZERO_WIDTH, "").replace("\r", "").strip()


def compact(s: str) -> str:
    """去掉全部空白，用于消除 "3 392 241.60" / "3392241.60" 这类空格差异"""
    return "".join(clean_text(s).split())


def words(doc: fitz.Document, index: int) -> list[tuple[str, float, float, float, float]]:
    out = []
    for w in doc[index].get_text("words"):
        t = clean_text(w[4])
        if t:
            out.append((t, w[0], w[1], w[2], w[3]))
    return out


def compare_boxes(a, b):
    """按顺序对齐两页的词序列，返回 (匹配数, 各方向偏差的 median/p95)"""
    ta = [w[0] for w in a]
    tb = [w[0] for w in b]
    sm = difflib.SequenceMatcher(None, ta, tb, autojunk=False)
    dx0, dy0, dx1, dy1 = [], [], [], []
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag != "equal":
            continue
        for i, j in zip(range(i1, i2), range(j1, j2)):
            ra, rb = a[i], b[j]
            dx0.append(abs(ra[1] - rb[1]))
            dy0.append(abs(ra[2] - rb[2]))
            dx1.append(abs(ra[3] - rb[3]))
            dy1.append(abs(ra[4] - rb[4]))
    if not dx0:
        return 0, {}
    p95 = lambda v: float(np.percentile(v, 95))
    return len(dx0), {
        "dx0_med": median(dx0), "dx0_p95": p95(dx0),
        "dy0_med": median(dy0), "dy0_p95": p95(dy0),
        "dx1_med": median(dx1), "dx1_p95": p95(dx1),
        "dy1_med": median(dy1), "dy1_p95": p95(dy1),
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("source")
    ap.add_argument("output")
    ap.add_argument("--reference", default=None, help="Python 版产物，用于对比文字层")
    ap.add_argument("--dpi", type=int, default=150)
    ap.add_argument("--terms", default="热交换器,导流筒,防冲结构,换热管,管板,折流板",
                    help="关键术语，逗号分隔")
    args = ap.parse_args()

    src = fitz.open(args.source)
    out = fitz.open(args.output)
    ref = fitz.open(args.reference) if args.reference else None

    ok = True
    print(f"页数: 原始 {src.page_count} / 产物 {out.page_count}"
          + (f" / Python {ref.page_count}" if ref else ""))
    if src.page_count != out.page_count:
        print("  ✗ 页数与原始文件不一致")
        ok = False

    terms = [t for t in args.terms.split(",") if t]
    ref_corpus = ""
    out_corpus = ""

    for i in range(out.page_count):
        a = render(src, i, args.dpi) if i < src.page_count else None
        b = render(out, i, args.dpi)
        line = f"[页 {i + 1:>3}] "
        if a is not None and a.shape == b.shape:
            diff = np.abs(a.astype(np.int16) - b.astype(np.int16)).max(axis=2)
            bad = int((diff > 8).sum())
            line += f"渲染差异像素 {bad}/{diff.size}  "
            if bad:
                line += "(需检查)"
        elif a is not None:
            line += f"尺寸不一致 {a.shape} vs {b.shape}  "

        t_out = clean_text(out[i].get_text("text"))
        out_corpus += t_out
        if ref is not None and i < ref.page_count:
            t_ref = clean_text(ref[i].get_text("text"))
            ref_corpus += t_ref
            ratio = difflib.SequenceMatcher(None, t_ref, t_out).ratio()
            ratio_c = difflib.SequenceMatcher(None, compact(t_ref), compact(t_out)).ratio()
            matched, stat = compare_boxes(words(ref, i), words(out, i))
            line += f"字符 {len(t_ref)}→{len(t_out)}  相似度 {ratio:.3f}/紧凑 {ratio_c:.3f}  "
            if stat:
                line += ("框偏差中位 dx{:.2f} dy{:.2f} pt，P95 dx{:.2f} dy{:.2f} pt，匹配 {} 词").format(
                    stat["dx0_med"], stat["dy0_med"], stat["dx0_p95"], stat["dy0_p95"], matched)
        elif not ref:
            line += f"提取字符 {len(t_out)}"
        print(line)

    if ref is not None:
        print()
        ratio_all = difflib.SequenceMatcher(None, ref_corpus, out_corpus).ratio()
        ratio_all_c = difflib.SequenceMatcher(None, compact(ref_corpus), compact(out_corpus)).ratio()
        print(f"整体文本相似度: {ratio_all:.4f}  紧凑（忽略空白）: {ratio_all_c:.4f}")
        for t in terms:
            print(f"术语「{t}」: Python {ref_corpus.count(t)} / C++ {out_corpus.count(t)}")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
