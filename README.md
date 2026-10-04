# Trowel

A native editor for the [Turmeric](https://turmeric-lang.com)
programming language, for macOS and Linux.

<img src="docs/images/screenshot.png" alt="Trowel editing a Turmeric source file" width="400">

**Latest release:** `v0.3.1` — Turmeric v0.62.0 with dependency errors traced through `load` chains, and a diagnosable AppImage build.

## Install

### macOS

```
brew install --cask rjungemann/trowel/trowel
```

> **Note the three-part name.** Use either the short token (`brew install --cask trowel`,
> once the tap is added) or the fully-qualified `rjungemann/trowel/trowel`. The two-part
> form `rjungemann/trowel` **will not work** — Homebrew reads it as a bare `user/repo` tap
> name rather than a cask, and fails with `Cask 'trowel' is unavailable: No Cask with this
> name exists.` The same applies to `brew reinstall` and `brew upgrade`.

Or download the notarized `.zip` from
[the releases page](https://github.com/rjungemann/trowel/releases) and
drag `Trowel.app` into `/Applications`.

### Linux

Download the AppImage for your architecture (`x86_64` or `aarch64`) from
[the releases page](https://github.com/rjungemann/trowel/releases), make it
executable, and run it:

```
chmod +x Trowel-*-x86_64.AppImage
./Trowel-*-x86_64.AppImage
```

The AppImage is self-contained — Qt and the Turmeric toolchain are bundled, so
there is nothing else to install. To add a menu entry and register `.tur` /
`.sweet` files, integrate it with a tool like
[Gear Lever](https://github.com/mijorus/gearlever) or AppImageLauncher.

## Using Trowel

- [Windows, tabs & the REPL](docs/guides/windows-tabs-and-repl.md) — where a
  file opens, what drag-and-drop does, session restore, and where each
  window's REPL is rooted.
- [Keyboard shortcuts](docs/guides/keyboard-shortcuts.md)

## Build from source

### macOS

Requires macOS, Qt 6, CMake, and Ninja.

```
brew install cmake ninja qt@6
cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$(brew --prefix qt@6)"
cmake --build build
open build/trowel.app
```

For Developer ID signing and notarization, see
[`docs/guides/signing-and-notarization.md`](docs/guides/signing-and-notarization.md).

### Linux

Requires a C++20 compiler, Qt 6 (Widgets, Network, Core5Compat), CMake, and
Ninja. On Debian/Ubuntu:

```
sudo apt install cmake ninja-build g++ \
    qt6-base-dev qt6-5compat-dev libqt6core5compat6
cmake --preset linux-release
cmake --build --preset linux-release
./build/linux-release/trowel
```

The build fetches and stages a pinned Turmeric toolchain next to the binary,
so the REPL and run-file commands work out of the box.

To package a self-contained AppImage (bundling Qt, Turmeric, and the REPL's
`libedit` dependency):

```
scripts/build-appimage.sh          # builds for the host arch (x86_64 or aarch64)
```

The result is `dist/Trowel-<version>-<arch>.AppImage`.

## Releasing

Use one of the Claude Code slash commands:

- `/cut-patch-release` — bug fixes only (`x.y.Z`)
- `/cut-minor-release` — new features, backward-compatible (`x.Y.0`)
- `/cut-major-release` — breaking changes (`X.0.0`)

Each command bumps the version in `CMakeLists.txt`, updates `CHANGELOG.md`
and this README's "Latest release" line, commits, tags, and pushes. The
tag push fires `.github/workflows/release.yml`, which builds, signs, and
notarizes the macOS app, builds the Linux AppImages (`x86_64` and
`aarch64`), and publishes them all to the GitHub Release.

## License

TBD.
