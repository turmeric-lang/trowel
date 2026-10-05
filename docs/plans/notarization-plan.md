# Notarization & Stapling Plan for Trowel.app

> **Status:** Complete. `scripts/notarize-app.sh` landed. Guide at
> [`docs/guides/signing-and-notarization.md`](../guides/signing-and-notarization.md)
> covers the full sign → notarize → staple flow.
> Prerequisite: [`codesigning-plan.md`](codesigning-plan.md) (also
> complete).

Follow-up to `codesigning-plan.md`. Signing is landed; the bundle is a valid
Developer ID signed app and passes `spctl --assess`. Notarization is what
lets a downloaded `Trowel.app` launch on someone else's Mac without the
"cannot be opened because Apple cannot check it for malicious software"
Gatekeeper dialog.

## Prerequisites

- Signed bundle at `build/macos-debug/trowel.app` (or wherever the build
  places it) — already covered by `TROWEL_CODESIGN_IDENTITY`.
- Hardened runtime is already on (`--options runtime` in
  `scripts/codesign-app.sh`).
- Apple developer credentials stored in the local keychain via
  `xcrun notarytool store-credentials`. Two supported forms:
  1. **App-specific password** — Apple ID + team ID + app-specific password
     generated at appleid.apple.com. Simplest for a solo dev box.
  2. **App Store Connect API key** — a `.p8` key + key ID + issuer ID.
     Better for CI, unnecessary locally.
- Keychain profile name: `trowel-notary`. All tooling references this name
  rather than raw credentials, so it never appears in the repo or CLI
  history.

Store once:

```
xcrun notarytool store-credentials trowel-notary \
    --apple-id "rtaljun@gmail.com" \
    --team-id N486ZZ4H36 \
    --password "<app-specific-password>"
```

## Steps for a single release

1. Build a signed bundle (existing flow).
2. Zip the bundle preserving symlinks and the top-level `.app` directory:
   ```
   ditto -c -k --keepParent build/macos-debug/trowel.app dist/Trowel.zip
   ```
   Use `ditto`, not `zip` — the latter mangles symlinks in frameworks and
   Apple has rejected notarization submissions built with it.
3. Submit and wait synchronously:
   ```
   xcrun notarytool submit dist/Trowel.zip \
       --keychain-profile trowel-notary \
       --wait
   ```
   The command blocks until Apple returns `Accepted` or `Invalid`. On
   `Invalid`, fetch the log for the submission ID:
   ```
   xcrun notarytool log <submission-id> --keychain-profile trowel-notary
   ```
4. Staple the notarization ticket to the bundle so Gatekeeper can verify
   offline:
   ```
   xcrun stapler staple build/macos-debug/trowel.app
   ```
5. Verify:
   ```
   xcrun stapler validate build/macos-debug/trowel.app
   spctl --assess --type execute --verbose build/macos-debug/trowel.app
   ```
   `spctl` should now print `source=Notarized Developer ID`.
6. Re-zip (or dmg) the stapled bundle for distribution — the ticket lives
   inside the bundle, so any archive of the stapled `.app` is what ships.

## Where this lives

Notarization is not part of `cmake --build`. It runs against an already-built
signed bundle and takes minutes (Apple round-trip). Wire it up as:

- `scripts/notarize-app.sh` — arguments `<app-bundle> <keychain-profile>`.
  Runs steps 2–5 above and exits non-zero on any failure. Idempotent: safe
  to re-run on an already-stapled bundle.
- No CMake hook. Invoke manually or from a release script (future).

### Script responsibilities

1. Refuse to run if the bundle isn't signed with the hardened runtime
   (check `codesign --display --verbose=2` for `flags=0x10000(runtime)`).
2. Create `dist/` if missing; write the zip there.
3. `xcrun notarytool submit ... --wait` and fail loudly on non-Accepted
   status, printing the log URL/ID so the operator can drill in.
4. `xcrun stapler staple` + `stapler validate`.
5. Final `spctl --assess` gate.

## Rollout

1. Store keychain profile once (documented in the guide, not the repo).
2. Land `scripts/notarize-app.sh`.
3. Dry run against a fresh signed build; confirm the stapled bundle
   launches from a quarantined location (`xattr -w com.apple.quarantine
   "0083;$(printf '%x' $(date +%s));Safari;" Trowel.app` then double-click
   from Finder).
4. Document the full sign → notarize → staple flow in
   `docs/guides/signing-and-notarization.md` alongside the signing docs.
5. Follow-up: DMG packaging and, eventually, a CI release job. The API-key
   auth form and a hosted signing identity become relevant then.

## Open questions

- Do we want a `.dmg` around the stapled `.app`, or ship the zip? DMG gives
  a nicer install UX but adds a step; zip is fine for early releases.
- Universal binary: notarization works per-slice, but the current bundle is
  arm64-only. Revisit when x86_64 support lands.
- CI story: deferred until there's a release pipeline. Local-only for now.
