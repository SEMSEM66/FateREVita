#!/usr/bin/env bash
set -euo pipefail

if [ -z "${VITASDK:-}" ]; then
  export VITASDK=/usr/local/vitasdk
  export PATH="$VITASDK/bin:$PATH"
fi

if ! command -v arm-vita-eabi-g++ >/dev/null 2>&1; then
  echo "ERROR: VitaSDK not found in PATH or \$VITASDK ($VITASDK)."
  echo "Install VitaSDK + vdpm first:"
  echo "  git clone https://github.com/vitasdk/vdpm && cd vdpm && ./bootstrap-vitasdk.sh"
  echo "  vdpm install vitaGL vitashark mathneon freetype libpng zlib libvorbis libogg"
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake .. -DBUNDLE_DATA=OFF
cmake --build . -j"$(nproc)"

echo ""
echo "Built: $BUILD_DIR/FateReVita.vpk"
echo "Copy FateReVita.vpk to your PS Vita and copy ../vita_data/* to ux0:data/FateReVita/"
