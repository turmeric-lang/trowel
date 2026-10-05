# Codesigning Plan for Trowel.app

> **Status:** Complete. `scripts/codesign-app.sh`,
> `resources/trowel.entitlements`, and the `TROWEL_CODESIGN_IDENTITY` /
> `TROWEL_CODESIGN_ENTITLEMENTS` CMake variables all landed. Guide at
> [`docs/guides/signing-and-notarization.md`](../guides/signing-and-notarization.md).
> Follow-up: [`notarization-plan.md`](notarization-plan.md) (also
> complete).

Sign the macOS `Trowel.app` bundle produced by the CMake build so it can be
distributed outside the local dev box without Gatekeeper quarantining it, and
so the bundled `tur` binary staged under `Contents/Resources/turmeric/`
doesn't trip "unidentified developer" prompts when Trowel spawns it.

## Signing identity

There is exactly one Developer ID Application identity available on the dev
machine:

```
C5E89348AACF6E5FD50B38432E4D410B322EA8A1 "Developer ID Application: Roger Bradley-Jungemann (N486ZZ4H36)"
Team ID: N486ZZ4H36
```

Verify locally with `security find-identity -v -p codesigning` before any
signing run.

## What has to be signed

The bundle currently looks like (after `cmake --build build`):

```
Trowel.app/
  Contents/
    MacOS/trowel                     <-- main executable (Qt6, links Scintilla)
    Frameworks/                      <-- Qt frameworks copied by macdeployqt (future)
    Resources/
      turmeric/
        bin/tur                      <-- bundled Turmeric CLI (prebuilt tarball)
        lib/*.dylib (if any)
        ... stdlib etc.
```

Every Mach-O inside the bundle must be signed with the same identity, in
inside-out order (nested binaries first, then the app itself). Concretely:

1. `Contents/Resources/turmeric/bin/tur` (and any dylibs shipped alongside).
2. Any bundled Qt frameworks under `Contents/Frameworks/` (once
   `macdeployqt` is wired in — see below).
3. `Contents/MacOS/trowel`.
4. The `.app` bundle itself (deep sign, `--options runtime`).

## Approach

Add a signing step to the CMake build that runs after the existing POST_BUILD
staging of the Turmeric tarball, gated on `APPLE` and on the signing identity
being present. Keep it opt-in via a cache variable so unsigned dev builds
still work without a keychain.

### CMake changes (`CMakeLists.txt`)

- Add options:
  - `TROWEL_CODESIGN_IDENTITY` — default empty string. When non-empty, sign.
    Accept either the SHA1 hash or the human-readable identity name.
  - `TROWEL_CODESIGN_ENTITLEMENTS` — path to an entitlements plist (default:
    `resources/trowel.entitlements`).
- After the existing `add_custom_command(TARGET trowel POST_BUILD ...)` that
  copies the Turmeric payload, append a second POST_BUILD command that runs
  the signing script (below) with the identity, entitlements, and bundle
  path.
- Skip the signing command entirely when `TROWEL_CODESIGN_IDENTITY` is empty,
  so plain `cmake --build build` on a fresh clone Just Works.

### Signing script (`scripts/codesign-app.sh`)

New helper. Arguments: `<identity> <entitlements-plist> <app-bundle-path>`.
Responsibilities:

1. Sign every Mach-O under `Contents/Resources/turmeric/` (find `-perm +111`
   filtered by `file` reporting `Mach-O`) with:
   ```
   codesign --force --timestamp --options runtime \
            --sign "$IDENTITY" \
            --entitlements "$ENTITLEMENTS" \
            "$path"
   ```
2. Sign any `Contents/Frameworks/*.framework` recursively (for the eventual
   `macdeployqt` output). Use `--deep` here is a footgun — sign frameworks
   individually.
3. Sign the main executable `Contents/MacOS/trowel`.
4. Sign the bundle itself:
   ```
   codesign --force --timestamp --options runtime \
            --sign "$IDENTITY" \
            --entitlements "$ENTITLEMENTS" \
            "$APP"
   ```
5. Verify with `codesign --verify --deep --strict --verbose=2 "$APP"` and
   `spctl --assess --type execute --verbose "$APP"`; fail the build if
   either check fails.

### Entitlements (`resources/trowel.entitlements`)

Minimum entitlements for a hardened-runtime Qt app that spawns a subprocess:

- `com.apple.security.cs.allow-unsigned-executable-memory` — probably NOT
  needed; Qt doesn't JIT. Start without it, add only if the app fails to
  launch under hardened runtime.
- `com.apple.security.cs.disable-library-validation` — needed if the bundled
  `tur` dlopens plugins that aren't signed by the same team. Start without;
  add if runtime signals it.
- `com.apple.security.cs.allow-jit` — not needed.
- `com.apple.security.get-task-allow` — dev-only; leave OFF for release.
- File access entitlements are inherited from the sandbox, and Trowel is
  not sandboxed. No `com.apple.security.app-sandbox`.

Start with an empty `<dict/>` and grow it based on Console.app failures.

## Notarization (follow-up, not this plan)

Signing is prerequisite to notarization but distinct. Notarization needs:

- An Apple ID + app-specific password (or App Store Connect API key)
  stored in the keychain via `xcrun notarytool store-credentials`.
- A `ditto -c -k --keepParent Trowel.app Trowel.zip` before submission.
- `xcrun notarytool submit ... --wait` and `xcrun stapler staple Trowel.app`.

Track notarization as a separate plan once signing lands and the CI story
for the identity/credentials is decided (probably: sign locally, notarize
locally, until there is a release pipeline worth automating).

## Rollout

1. Land the entitlements plist + `scripts/codesign-app.sh` + CMake hook,
   gated on `TROWEL_CODESIGN_IDENTITY`.
2. Local build with:
   ```
   cmake -S . -B build \
     -DTROWEL_CODESIGN_IDENTITY=C5E89348AACF6E5FD50B38432E4D410B322EA8A1
   cmake --build build
   ```
   Confirm `codesign --verify --deep --strict` clean and the bundled `tur`
   still runs from inside the app.
3. Document the identity + build invocation in `docs/guides/` alongside the
   existing build docs.
4. Follow-up plan: notarization + stapling.

## Open questions

- Do we want a single deep-sign of the bundle plus per-nested signing, or
  only per-nested + top-level? (Per-nested + top-level is the Apple-blessed
  path; `--deep` is discouraged and re-signs things you may not want to
  touch.)
- How do we handle the bundled Turmeric payload when it is upgraded — the
  signing step re-runs on every POST_BUILD, so upgrades are automatic as
  long as the identity is available. No extra work.
- Universal binary story: the current bundle only ships arm64. Signing works
  the same for x86_64 slices once they exist.
