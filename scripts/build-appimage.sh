#!/usr/bin/env bash
# Build a self-contained Trowel AppImage for the host architecture.
#
#   scripts/build-appimage.sh [ARCH]
#
# ARCH defaults to the host (`uname -m`): x86_64 or aarch64. Produces
# dist/Trowel-<version>-<arch>.AppImage bundling Qt (via linuxdeploy-plugin-qt),
# the pinned Turmeric toolchain (staged by CMake next to the binary), and
# tur's libedit dependency chain so the REPL runs on hosts without libedit2.
#
# Requires: cmake, ninja, a Qt6 dev install, curl, and FUSE *or* a kernel that
# allows AppImage extract-and-run (the script forces extract-and-run so it works
# in containers/CI without FUSE).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

ARCH="${1:-$(uname -m)}"
case "$ARCH" in
    x86_64|aarch64) ;;
    arm64) ARCH=aarch64 ;;
    *) echo "unsupported arch: $ARCH (expected x86_64 or aarch64)" >&2; exit 2 ;;
esac

TROWEL_VERSION="$(cat VERSION)"
BUILD_DIR="build/appimage-${ARCH}"
APPDIR="${BUILD_DIR}/AppDir"
TOOLS_DIR="build/appimage-tools"

# Run downloaded AppImage tools without FUSE (containers/CI).
export APPIMAGE_EXTRACT_AND_RUN=1
# Let linuxdeploy stamp the version into the output filename.
export VERSION="$TROWEL_VERSION"

echo "==> Configuring + building Trowel (Release, ${ARCH})"
cmake -S . -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    ${CMAKE_PREFIX_PATH:+-DCMAKE_PREFIX_PATH="$CMAKE_PREFIX_PATH"}
cmake --build "$BUILD_DIR"

echo "==> Installing into AppDir"
rm -rf "$APPDIR"
DESTDIR="$PWD/$APPDIR" cmake --install "$BUILD_DIR" --prefix /usr >/dev/null

echo "==> Fetching linuxdeploy + qt plugin (${ARCH})"
mkdir -p "$TOOLS_DIR"
# Fetch a tool, and SAY WHY if it fails.
#
# This was `curl -fsSL -o "$dest" "$url"` under `set -euo pipefail`. `-f` makes
# curl exit non-zero on an HTTP error and `-s` silences the message explaining
# it, so a failure here killed the script with nothing in the log but
# `##[error]Process completed with exit code 1.` -- which is exactly how two
# v0.3.0 release runs failed on both architectures, 0.7-1.2s after the fetch
# started, while all six asset URLs answered 200 from outside CI. The cause was
# not diagnosable after the fact, because it had been thrown away.
#
# So: keep -f (a 404 must still fail the build), drop -s, retry transient
# errors, and on failure re-probe once to put the server's actual answer in the
# log. A 403, a DNS failure and a rate limit read very differently and want
# different fixes.
fetch() {  # fetch <url> <dest>
    local url="$1" dest="$2" code=0
    [ -x "$dest" ] && return 0
    echo "    GET $url"
    curl -fL --retry 3 --retry-all-errors --retry-delay 2 \
         --connect-timeout 20 --max-time 600 -o "$dest" "$url" || code=$?
    if [ "$code" -ne 0 ]; then
        echo "    curl exited $code fetching $url" >&2
        curl -sS -o /dev/null -L --max-time 30 \
             -w "    server said HTTP %{http_code} (dns %{time_namelookup}s, connect %{time_connect}s)\n" \
             "$url" >&2 || true
        return 1
    fi
    chmod +x "$dest"
}
BASE="https://github.com/linuxdeploy"
fetch "${BASE}/linuxdeploy/releases/download/continuous/linuxdeploy-${ARCH}.AppImage" \
      "${TOOLS_DIR}/linuxdeploy-${ARCH}.AppImage"
fetch "${BASE}/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-${ARCH}.AppImage" \
      "${TOOLS_DIR}/linuxdeploy-plugin-qt-${ARCH}.AppImage"
fetch "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-${ARCH}.AppImage" \
      "${TOOLS_DIR}/appimagetool-${ARCH}.AppImage"

# Help the Qt plugin find the right Qt (qmake6 on Debian/Ubuntu).
if [ -z "${QMAKE:-}" ]; then
    QMAKE="$(command -v qmake6 || command -v qmake || true)"
fi
export QMAKE

# Bundle tur's libedit dependency chain (libedit -> libtinfo/libbsd/libmd) so
# the bundled REPL runs on hosts that don't ship libedit2. linuxdeploy pulls in
# transitive deps automatically.
LIBEDIT="$(ldd "$APPDIR/usr/bin/turmeric/tur" 2>/dev/null | awk '/libedit/{print $3}')"
EXTRA_LIB_ARGS=()
if [ -n "$LIBEDIT" ] && [ -e "$LIBEDIT" ]; then
    EXTRA_LIB_ARGS+=(--library "$LIBEDIT")
    echo "==> Bundling tur runtime lib: $LIBEDIT"
else
    echo "==> WARNING: libedit for tur not found; REPL may fail on minimal hosts" >&2
fi

echo "==> Deploying Qt + libraries into AppDir (linuxdeploy)"
# Deploy only (no --output): re-running linuxdeploy over an already-deployed
# AppDir is unreliable, so we package separately with appimagetool below.
"${REPO_ROOT}/${TOOLS_DIR}/linuxdeploy-${ARCH}.AppImage" \
    --appdir "$APPDIR" \
    --executable "$APPDIR/usr/bin/trowel" \
    --desktop-file "$APPDIR/usr/share/applications/trowel.desktop" \
    --icon-file "$APPDIR/usr/share/icons/hicolor/256x256/apps/trowel.png" \
    "${EXTRA_LIB_ARGS[@]}" \
    --plugin qt

# linuxdeploy-plugin-qt bundles the xcb platform plugin but not offscreen. Add
# it so the AppImage can run headlessly (CI smoke tests, control-socket
# automation). Its Qt deps resolve via the AppRun-set LD_LIBRARY_PATH.
QT_PLUGINS="$("$QMAKE" -query QT_INSTALL_PLUGINS 2>/dev/null || true)"
OFFSCREEN="${QT_PLUGINS}/platforms/libqoffscreen.so"
if [ -e "$OFFSCREEN" ]; then
    cp -f "$OFFSCREEN" "$APPDIR/usr/plugins/platforms/"
    echo "==> Bundled offscreen platform plugin"
else
    echo "==> WARNING: offscreen platform plugin not found; AppImage won't run headless" >&2
fi

echo "==> Packaging AppImage (appimagetool)"
mkdir -p dist
ARCH="$ARCH" "${REPO_ROOT}/${TOOLS_DIR}/appimagetool-${ARCH}.AppImage" \
    "$APPDIR" "dist/Trowel-${TROWEL_VERSION}-${ARCH}.AppImage"

echo "==> Built dist/Trowel-${TROWEL_VERSION}-${ARCH}.AppImage"
