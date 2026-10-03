#!/usr/bin/env bash
# Regenerate Trowel's app-icon assets from a single 1024x1024 master PNG.
#
#   scripts/generate-icons.sh [MASTER.png]
#
# MASTER defaults to "Simple Icon Dark.png" (the shipping icon; the original
# white-t "Simple Icon.png" is kept alongside it as the previous artwork).
#
# Writes:
#   resources/trowel.icns                                  macOS bundle icon
#   resources/linux/icons/hicolor/<N>x<N>/apps/trowel.png  XDG hicolor set
#
# CMakeLists.txt and scripts/build-appimage.sh reference those paths by name,
# so refreshing the icon is purely a matter of rerunning this — no build-file
# edits. Commit the regenerated binaries along with the new master.
#
# Requires: python3 with Pillow. The .icns additionally needs `iconutil`, which
# is macOS-only; elsewhere that step is skipped and the committed .icns stands.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

MASTER="${1:-Simple Icon Dark.png}"

if [[ ! -f "$MASTER" ]]; then
    echo "generate-icons: master image not found: $MASTER" >&2
    exit 1
fi

if ! python3 -c 'import PIL' 2>/dev/null; then
    echo "generate-icons: python3 with Pillow is required (pip install Pillow)" >&2
    exit 1
fi

# Linux hicolor sizes installed by CMakeLists.txt. 256x256 is also what
# build-appimage.sh hands to linuxdeploy as --icon-file.
LINUX_SIZES=(32 48 64 128 256 512)

# .iconset entries iconutil expects, as "<filename> <pixel size>".
ICONSET_ENTRIES=(
    "icon_16x16.png 16"
    "icon_16x16@2x.png 32"
    "icon_32x32.png 32"
    "icon_32x32@2x.png 64"
    "icon_128x128.png 128"
    "icon_128x128@2x.png 256"
    "icon_256x256.png 256"
    "icon_256x256@2x.png 512"
    "icon_512x512.png 512"
    "icon_512x512@2x.png 1024"
)

# Lanczos, deliberately: resampling the master with it reproduces the icon set
# that shipped before this script existed to within a few 8-bit levels, so
# rerunning here never silently restyles the edges of the artwork.
resize_to() {
    local size="$1" dest="$2"
    python3 - "$MASTER" "$size" "$dest" <<'PY'
import sys
from PIL import Image

master, size, dest = sys.argv[1], int(sys.argv[2]), sys.argv[3]
im = Image.open(master).convert("RGB")
if im.size != (1024, 1024):
    sys.exit(f"generate-icons: expected a 1024x1024 master, got {im.size[0]}x{im.size[1]}")
if size != 1024:
    im = im.resize((size, size), Image.LANCZOS)
im.save(dest)
PY
}

echo "==> Master: $MASTER"

echo "==> Writing Linux hicolor PNGs"
for size in "${LINUX_SIZES[@]}"; do
    dest="resources/linux/icons/hicolor/${size}x${size}/apps/trowel.png"
    mkdir -p "$(dirname "$dest")"
    resize_to "$size" "$dest"
    echo "    $dest"
done

if ! command -v iconutil >/dev/null 2>&1; then
    echo "==> Skipping resources/trowel.icns (iconutil is macOS-only)"
    exit 0
fi

echo "==> Writing resources/trowel.icns"
ICONSET="$(mktemp -d)/trowel.iconset"
trap 'rm -rf "$(dirname "$ICONSET")"' EXIT
mkdir -p "$ICONSET"

for entry in "${ICONSET_ENTRIES[@]}"; do
    name="${entry%% *}"
    size="${entry##* }"
    resize_to "$size" "$ICONSET/$name"
done

iconutil -c icns -o resources/trowel.icns "$ICONSET"
echo "    resources/trowel.icns"
