#!/usr/bin/env bash
# Linux x64 依赖准备：ONNX Runtime + MuPDF（编译静态库）。
# 用法：bash tools/fetch_deps_linux.sh [--force]
set -euo pipefail

ORT_VERSION="${ORT_VERSION:-1.20.1}"
MUPDF_VERSION="${MUPDF_VERSION:-1.25.0}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
THIRD="$ROOT/third_party"
ORT_DIR="$THIRD/onnxruntime-linux-x64-$ORT_VERSION"
MUPDF_DIR="$THIRD/mupdf-$MUPDF_VERSION"
FORCE=0
[[ "${1:-}" == "--force" ]] && FORCE=1

mkdir -p "$THIRD"

if [[ $FORCE -eq 1 || ! -f "$ORT_DIR/lib/libonnxruntime.so" ]]; then
  echo "==> 下载 ONNX Runtime $ORT_VERSION (linux-x64)"
  tmp="$(mktemp -d)"
  curl -fL --retry 3 -o "$tmp/ort.tgz" \
    "https://github.com/microsoft/onnxruntime/releases/download/v$ORT_VERSION/onnxruntime-linux-x64-$ORT_VERSION.tgz"
  tar -xzf "$tmp/ort.tgz" -C "$tmp"
  rm -rf "$ORT_DIR"
  mv "$tmp/onnxruntime-linux-x64-$ORT_VERSION" "$ORT_DIR"
  rm -rf "$tmp"
else
  echo "==> ONNX Runtime 已存在：$ORT_DIR"
fi

if [[ $FORCE -eq 1 || ! -f "$MUPDF_DIR/lib/libmupdf.a" ]]; then
  echo "==> 编译 MuPDF $MUPDF_VERSION"
  tmp="$(mktemp -d)"
  curl -fL --retry 3 -o "$tmp/mupdf.tar.gz" \
    "https://mupdf.com/downloads/archive/mupdf-$MUPDF_VERSION-source.tar.gz"
  tar -xzf "$tmp/mupdf.tar.gz" -C "$tmp"
  src="$tmp/mupdf-$MUPDF_VERSION-source"
  make -C "$src" build=release libs HAVE_X11=no HAVE_GLUT=no
  rm -rf "$MUPDF_DIR"
  mkdir -p "$MUPDF_DIR/lib" "$MUPDF_DIR/include"
  cp "$src/build/release/libmupdf.a" "$src/build/release/libmupdf-third.a" "$MUPDF_DIR/lib/"
  cp -r "$src/include"/* "$MUPDF_DIR/include/"
  rm -rf "$tmp"
else
  echo "==> MuPDF 已存在：$MUPDF_DIR"
fi

echo "==> 完成"