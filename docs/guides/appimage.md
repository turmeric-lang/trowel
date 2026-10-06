# Building an AppImage

The AppImage is Trowel's self-contained Linux distribution: a single file that
bundles Qt, the pinned Turmeric toolchain, and the REPL's `libedit` dependency,
so it runs on hosts that have none of those installed. The build is driven by
`scripts/build-appimage.sh`.

## Prerequisites

- CMake, Ninja, and a Qt 6 dev install (same as a [normal build](../../README.md#linux)).
- `curl` (to fetch the packaging tools).
- FUSE is **not** required — the script forces extract-and-run mode
  (`APPIMAGE_EXTRACT_AND_RUN=1`), so it works in containers and CI without a
  FUSE mount.

## Building

```
scripts/build-appimage.sh          # builds for the host arch (x86_64 or aarch64)
scripts/build-appimage.sh aarch64  # build for a specific arch
```

The script:

1. Configures and builds Trowel in Release mode.
2. Installs into a staging `AppDir`.
3. Fetches `linuxdeploy`, `linuxdeploy-plugin-qt`, and `appimagetool` for the
   target arch (cached under `build/appimage-tools/`).
4. Bundles Qt and libraries into the AppDir via `linuxdeploy` + the Qt plugin.
5. Bundles `tur`'s `libedit` dependency chain so the REPL runs on minimal hosts.
6. Adds the offscreen Qt platform plugin so the AppImage can run headlessly
   (CI smoke tests, control-socket automation).
7. Packages the AppDir into `dist/Trowel-<version>-<arch>.AppImage`.

The version comes from the `VERSION` file. If `CMAKE_PREFIX_PATH` is set in the
environment, it is passed through to CMake so you can point at a non-default Qt.

## Running the result

```
chmod +x dist/Trowel-*-x86_64.AppImage
./dist/Trowel-*-x86_64.AppImage
```

The AppImage is self-contained — Qt and the Turmeric toolchain are bundled, so
there is nothing else to install.

### Integrating with the desktop

To add a menu entry and register `.tur` / `.sweet` file associations, use a
tool like [Gear Lever](https://github.com/mijorus/gearlever) or
[AppImageLauncher](https://github.com/TheAssassin/AppImageLauncher). These
integrate the AppImage into your application menu and handle file-type
registration.

### Running headless

The offscreen platform plugin is bundled, so the AppImage runs without a
display — useful for CI and the [control socket](scripting.md):

```
./dist/Trowel-*-x86_64.AppImage --control-socket
```

If the offscreen plugin was not found at build time, the script prints a
warning and the AppImage will not run headless.

## Troubleshooting

- **`tur` not found under AppDir** — the script probes both the prefix layout
  (`bin/tur`) and the flat layout (`tur`) that older Turmeric archives used.
  If neither is present, it warns and skips `libedit` bundling; the REPL may
  fail on minimal hosts.
- **`libedit` not found** — if `ldd` cannot resolve `libedit` for the bundled
  `tur`, the script warns. The AppImage still builds, but the REPL may fail on
  hosts without `libedit2`.
- **Tool fetch failures** — the script retries transient errors and, on
  failure, re-probes the URL once to log the server's actual HTTP status, so a
  403, DNS failure, and rate limit are distinguishable in the build log.

## See also

- [Scripting Trowel](scripting.md) — driving the AppImage headlessly via the
  control socket.
- [Signing and notarization](signing-and-notarization.md) — macOS release
  signing (AppImages are not signed; Linux has no equivalent step).
