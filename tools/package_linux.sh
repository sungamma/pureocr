#!/usr/bin/env bash
# 把 Linux 构建产物打成可分发 tar.gz。
#
# 用法：
#   bash tools/package_linux.sh [--build-dir DIR] [--out-dir DIR] [--version X.Y.Z] [--models none|basic|full]
#
# 产物：dist/ocrgui-<version>-linux-x64.tar.gz（含 ocrgui、libonnxruntime.so、models/、许可与说明）
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT/build"
OUT_DIR="$ROOT/dist"
VERSION=""
MODELS="none"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    --out-dir)   OUT_DIR="$2"; shift 2 ;;
    --version)   VERSION="$2"; shift 2 ;;
    --models)    MODELS="$2"; shift 2 ;;
    -h|--help)   sed -n '2,10p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "未知参数: $1" >&2; exit 2 ;;
  esac
done

if [[ -z "$VERSION" ]]; then
  VERSION="$(head -n1 "$ROOT/VERSION" | tr -d '[:space:]')"
fi
if [[ -z "$VERSION" ]]; then
  echo "无法确定版本号（VERSION 文件为空）" >&2
  exit 1
fi

BIN="$BUILD_DIR/ocrgui"
if [[ ! -f "$BIN" ]]; then
  BIN="$(find "$BUILD_DIR" -maxdepth 3 -type f -name ocrgui -perm -u+x 2>/dev/null | head -n1)"
fi
if [[ -z "${BIN:-}" || ! -f "$BIN" ]]; then
  echo "找不到 ocrgui 可执行文件（构建目录: $BUILD_DIR）" >&2
  exit 1
fi

ORT_LIB="$(dirname "$BIN")/libonnxruntime.so"
if [[ ! -f "$ORT_LIB" ]]; then
  ORT_LIB="$(find "$ROOT/third_party" -maxdepth 4 -type f -name libonnxruntime.so 2>/dev/null | head -n1)"
fi

PKG_NAME="ocrgui-$VERSION-linux-x64"
PKG_DIR="$OUT_DIR/$PKG_NAME"
TARBALL="$OUT_DIR/$PKG_NAME.tar.gz"

mkdir -p "$OUT_DIR"
rm -rf "$PKG_DIR"
mkdir -p "$PKG_DIR/models"

install -m 0755 "$BIN" "$PKG_DIR/ocrgui"
if [[ -n "${ORT_LIB:-}" && -f "$ORT_LIB" ]]; then
  install -m 0644 "$ORT_LIB" "$PKG_DIR/libonnxruntime.so"
else
  echo "警告：未找到 libonnxruntime.so，产物需要用户自行提供" >&2
fi

for f in LICENSE THIRD_PARTY_NOTICES.md; do
  if [[ -f "$ROOT/$f" ]]; then cp "$ROOT/$f" "$PKG_DIR/"; fi
done

for f in ppocrv5_dict.txt registry.ini README.md; do
  if [[ -f "$ROOT/models/$f" ]]; then cp "$ROOT/models/$f" "$PKG_DIR/models/"; fi
done

case "$MODELS" in
  basic)
    for f in ch_PP-OCRv5_det_infer.onnx ch_PP-OCRv5_rec_infer.onnx ch_ppocr_mobile_v2.0_cls_infer.onnx; do
      [[ -f "$ROOT/models/$f" ]] && cp "$ROOT/models/$f" "$PKG_DIR/models/"
    done
    ;;
  full)
    cp "$ROOT"/models/*.onnx "$PKG_DIR/models/" 2>/dev/null || true
    ;;
  none) ;;
esac

cat > "$PKG_DIR/run-cli.sh" <<'EOS'
#!/usr/bin/env bash
# 命令行包装：./run-cli.sh --pdf book.pdf [其它 ocrgui 参数]
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$HERE/ocrgui" "$@"
EOS
chmod 0755 "$PKG_DIR/run-cli.sh"

cat > "$PKG_DIR/使用说明.txt" <<EOS
PDF OCR（Linux x64 版）
==========================

运行图形界面：
  ./ocrgui

命令行（第一次会自动下载缺失模型，或手动放到 models/）：
  ./run-cli.sh --pdf book.pdf
  ./run-cli.sh --pdf book.pdf --pages 1-10 --threads 2
  ./run-cli.sh --dir ./samples --out-dir ./out --recurse
  ./ocrgui --fetch-models --model-dir ./models     # 预下载模型
  ./ocrgui --version

运行环境依赖（Debian/Ubuntu）：
  sudo apt-get install -y libgl1 libglx-mesa0 libcurl4 openssl \
       libfontconfig1 fonts-noto-cjk zenity
  # 无显示服务器时请用命令行模式（--pdf/--dir）；图形界面需要 X11/Wayland。

说明：
- 输出为“原 PDF + 不可见 OCR 文字层”，可直接选中复制；默认不覆盖原始文字（--mask 才替换）。
- 每个 PDF 生成 原名_OCR.pdf，已存在则自动改名，避免覆盖。
- 模型放在 models/ 下，可共用 ppocrv5_dict.txt。
- 断点续跑：同目录写 *_ocrcache.tsv，可中途中断后再跑。
- 许可：AGPL-3.0，见 LICENSE / THIRD_PARTY_NOTICES.md。
EOS

COMMIT="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
{
  echo "ocrgui build info"
  echo "================="
  echo "VERSION  : $VERSION"
  echo "built    : $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
  echo "commit   : $COMMIT"
  echo "models   : $MODELS (none = download on first run)"
  echo "platform : Linux x86_64 (CPU, ONNX Runtime)"
  echo "uname    : $(uname -srm)"
} > "$PKG_DIR/BUILD.txt"

tar -C "$OUT_DIR" -czf "$TARBALL" "$PKG_NAME"
( cd "$OUT_DIR" && sha256sum "$PKG_NAME.tar.gz" > SHA256SUMS.txt )

echo "打包完成：$TARBALL"
echo "SHA256  ：$(cut -d' ' -f1 "$OUT_DIR/SHA256SUMS.txt")"
