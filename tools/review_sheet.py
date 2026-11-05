#!/usr/bin/env python3
"""从复核 CSV 抽样，生成「看图核对」拼版 PDF（人工抽检识别准确率用）。

用法：
    python review_sheet.py <原始.pdf> <复核.csv> [-n 30] [-o 拼版.pdf] [--only-low] [--seed 0]

说明：
    - 默认一半名额给「待复核」里分数最低的行，其余随机抽（都来自 OCR 结果）
    - 每格显示：序号、页码、置信度、识别文字，配该行在原文中的裁图
    - 同时把抽中的清单打印到终端（序号/页码/分数/文字），便于记录对错
"""
from __future__ import annotations

import argparse
import csv
import io
import random
import sys

import fitz


def load_rows(path: str) -> list[dict]:
    with io.open(path, encoding="utf-8-sig", newline="") as f:
        return list(csv.DictReader(f))


def pick(rows: list[dict], n: int, only_low: bool, seed: int) -> list[dict]:
    low = sorted([r for r in rows if r.get("待复核") == "1"], key=lambda r: float(r["置信度"]))
    rest = [r for r in rows if r.get("待复核") != "1"]
    rnd = random.Random(seed)
    rnd.shuffle(rest)
    if only_low:
        return low[:n]
    take_low = min(len(low), max(1, n // 2))
    return low[:take_low] + rest[: max(0, n - take_low)]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("pdf")
    ap.add_argument("csv_file")
    ap.add_argument("-n", type=int, default=30, help="抽样行数（默认 30）")
    ap.add_argument("-o", "--out", default="", help="输出拼版 PDF")
    ap.add_argument("--only-low", action="store_true", help="只抽待复核行")
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--dpi", type=int, default=200, help="裁图渲染 dpi")
    args = ap.parse_args()

    rows = load_rows(args.csv_file)
    if not rows:
        print("CSV 没有数据行", file=sys.stderr)
        return 1
    picked = pick(rows, args.n, args.only_low, args.seed)
    low_total = sum(1 for r in rows if r.get("待复核") == "1")

    src = fitz.open(args.pdf)
    out = fitz.open()

    # 版式：A4 竖版，2 列 x 6 行
    PAGE_W, PAGE_H = 595.0, 842.0
    MARGIN, GAP = 28.0, 10.0
    COLS, ROWS = 2, 6
    HEADER = 46.0
    cell_w = (PAGE_W - 2 * MARGIN - (COLS - 1) * GAP) / COLS
    cell_h = (PAGE_H - 2 * MARGIN - HEADER - (ROWS - 1) * GAP) / ROWS

    def new_page() -> fitz.Page:
        p = out.new_page(width=PAGE_W, height=PAGE_H)
        return p

    page = new_page()
    page.insert_text((MARGIN, MARGIN + 14), "OCR 复核抽样", fontname="china-s", fontsize=15)
    page.insert_text(
        (MARGIN, MARGIN + 30),
        f"共 {len(rows)} 行，待复核 {low_total} 行；本次抽 {len(picked)} 行（seed={args.seed}）",
        fontname="china-s",
        fontsize=9,
    )
    page.insert_text((MARGIN, MARGIN + 42), args.csv_file, fontname="china-s", fontsize=7)
    slot = 0

    for idx, r in enumerate(picked, start=1):
        if slot >= COLS * ROWS:
            page = new_page()
            page.insert_text((MARGIN, MARGIN + 10), f"OCR 复核抽样（续 {idx}）", fontname="china-s",
                             fontsize=11)
            slot = 0
        col, row = slot % COLS, slot // COLS
        x0 = MARGIN + col * (cell_w + GAP)
        y0 = MARGIN + HEADER + row * (cell_h + GAP)

        page_no = int(r["页"]) - 1
        score = float(r["置信度"])
        text = r.get("文字", "")
        clip = fitz.Rect(
            float(r["左pt"]) - 2,
            float(r["上pt"]) - 1,
            float(r["右pt"]) + 2,
            float(r["下pt"]) + 1,
        )
        label_h = 12.0
        try:
            pm = src[page_no].get_pixmap(dpi=args.dpi, clip=clip)
        except Exception as e:  # noqa: BLE001
            pm = None
            page.insert_text((x0, y0 + 10), f"#{idx} 渲染失败: {e}", fontname="china-s", fontsize=7)
        color = (0.75, 0.1, 0.1) if r.get("待复核") == "1" else (0.1, 0.45, 0.15)
        page.draw_rect(fitz.Rect(x0, y0, x0 + cell_w, y0 + cell_h), color=color, width=0.6)
        if pm is not None:
            img_rect = fitz.Rect(x0 + 3, y0 + 3, x0 + cell_w - 3, y0 + cell_h - label_h - 3)
            page.insert_image(img_rect, pixmap=pm, keep_proportion=True)
        head = f"#{idx}  P{page_no + 1}  s={score:.2f}"
        if r.get("待复核") == "1":
            head += "  待复核"
        page.insert_text((x0 + 4, y0 + cell_h - 3), head, fontname="china-s", fontsize=7.5,
                         color=color)
        short = text if len(text) <= 34 else text[:34] + "…"
        page.insert_text((x0 + cell_w * 0.42, y0 + cell_h - 3), short, fontname="china-s",
                         fontsize=7.5)
        slot += 1

        print(f"#{idx:02d}  P{page_no + 1:<4d} s={score:.4f}  {text}")

    out_pdf = args.out or (args.csv_file.rsplit(".", 1)[0] + "_抽样.pdf")
    out.save(out_pdf)
    out.close()
    src.close()
    print(f"\n拼版已生成：{out_pdf}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
