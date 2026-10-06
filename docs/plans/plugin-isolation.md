# Per-plugin isolation — plan

> **Status:** Not started. Split from
> [`plugin-system.md`](plugin-system.md) §4.4, where it was a single
> paragraph marked "future." The plugin system itself (P0–P7) has shipped;
> this plan is the next security boundary.
> **Related:** [`plugin-system.md`](plugin-system.md) (the plugin host this
> builds on), [`PLAN.md`](PLAN.md) (architecture).

## 0. Context

v1 uses a single shared `TuriEnv` for all plugins. This is simple and lets
plugins share definitions — one plugin's `defmodule` is visible to another's
`(import ...)`. The trade-off: a plugin that calls `turi_env_reset` would
wipe every other plugin's state, and a buggy or hostile plugin has
unrestricted access to every native and every other plugin's globals.

The v1 trust model (see [`plugin-system.md`](plugin-system.md) §8) accepts
this: plugins are local files the user chose to install, same trust boundary
as a `.vimrc`. Per-plugin isolation is the next step — not because v1 is
unsafe, but because it is the prerequisite for loading untrusted or
third-party plugins without that assumption.

## 1. Goals

- One `TuriEnv` per plugin, each sandboxed with `turi_env_new_sandboxed`
  plus `turi_env_allow` grants for the capabilities the plugin's manifest
  declares.
- A shared "API env" holds the Trowel natives (`trowel-register-command`,
  `trowel-buffer-text`, etc.); each plugin env imports from it rather than
  re-registering the natives.
- A plugin manifest (`plugin.tur` frontmatter or a sibling `manifest.tur`)
  declares the capabilities the plugin needs (file I/O, network, process
  spawn). The host checks the manifest against a permission prompt on first
  load, like VSCode's "This extension wants to access X."
- A plugin that crashes, throws, or calls `turi_env_reset` affects only
  itself.

## 2. Architecture

### 2.1 Per-plugin env lifecycle

```
PluginHost
├── apiEnv_  (TuriEnv*)          ← shared, holds all trowel-* natives
├── PluginRecord
│   ├── name: QString
│   ├── env: TuriEnv*            ← sandboxed, one per plugin
│   ├── manifest: PluginManifest
│   └── closures: QVector<TuriValue>  ← for cleanup on unload
└── PluginRecord
    ├── ...
```

`PluginHost::loadAll()` changes from "eval every `plugin.tur` into the same
env" to:

1. Create (or reuse) the shared API env. Register all `trowel-*` natives
   into it once.
2. For each plugin directory:
   a. Read the manifest (capabilities, dependencies).
   b. Create a sandboxed env via `turi_env_new_sandboxed`.
   c. Grant the declared capabilities via `turi_env_allow`.
   d. Set the module base dir to the plugin's own directory (so
      `(import ...)` resolves sibling modules within the plugin, not
      other plugins).
   e. Inject the API env's exports into the plugin env (via
      `turi_env_set` for each native, or a shared spice image — see §3).
   f. Eval `plugin.tur` in the plugin env.

### 2.2 The API env

The API env is a `TuriEnv` created once at startup. All `trowel-*` natives
are registered into it. Each plugin env gets access to them through one of:

- **Option A: re-export.** The host iterates the API env's native table and
  re-registers each native into the plugin env. Simple, but duplicates the
  registration and means the `ud` pointer (PluginContext) must be per-plugin
  or shared.
- **Option B: shared spice image.** `turi_env_set_shared_spice_image` lets
  one env's compiled definitions be visible to another. The API env's
  natives are compiled once; each plugin env imports them through the
  shared image. This is the model the API was designed for.

Option B is preferred. The spice image is the mechanism the Turmeric
runtime already provides for cross-env sharing; re-exporting is a fallback
if the spice image API is not stable in the pinned version.

### 2.3 Plugin manifest

A `manifest.tur` (or frontmatter at the top of `plugin.tur`) declares:

```turmeric
;; manifest.tur
(defplugin "my-plugin"
  :capabilities [fs:read fs:write]
  :description "A file-watcher plugin.")
```

Capabilities map to `TuriCaps` flags:

| Manifest keyword | `TuriCaps` flag |
|---|---|
| `fs:read` | `TURI_CAP_FS` |
| `fs:write` | `TURI_CAP_FS` |
| `io` (network) | `TURI_CAP_IO` |
| `proc` (process spawn) | `TURI_CAP_PROC` |
| `ffi` | `TURI_CAP_FFI` |
| `async` | `TURI_CAP_ASYNC` |
| `import` (cross-plugin imports) | `TURI_CAP_IMPORT` |
| `env` (environment variables) | `TURI_CAP_ENV` |

Plugins that declare no capabilities run with `TURI_CAP_NONE` — they can
still call `trowel-*` natives (which are the host's own API, not the stdlib's
I/O surface) but cannot use the Turmeric stdlib's file, network, or process
functions.

### 2.4 Permission prompt

On first load of a plugin that declares capabilities beyond `TURI_CAP_NONE`,
the host shows a dialog:

> "The plugin `my-plugin` wants to: read and write files, spawn processes.
> Allow?"

The result is cached in `~/.trowel/plugin-permissions.json`. Subsequent
loads skip the prompt unless the manifest changes.

## 3. Implementation phases

### I1 — Per-plugin env creation (1–2 days)

- `PluginHost` holds a `QHash<QString, PluginRecord>` instead of a single
  env.
- `loadAll()` creates one sandboxed env per plugin.
- The API env is created once; natives are registered into it.
- Plugin envs get the API env's natives via shared spice image (or
  re-export fallback).
- `reloadPlugin(name)` resets only that plugin's env, not all of them.

### I2 — Manifest parsing + capability grants (1–2 days)

- Parse `manifest.tur` (or `plugin.tur` frontmatter).
- Map declared capabilities to `TuriCaps` flags.
- Call `turi_env_allow(env, cap)` for each declared capability.
- Plugins without a manifest run with `TURI_CAP_NONE`.

### I3 — Permission prompt + persistence (1–2 days)

- First-load dialog for plugins with capabilities.
- `~/.trowel/plugin-permissions.json` cache.
- Manifest-hash invalidation: re-prompt if the manifest changed.

### I4 — Cross-plugin imports (1 day, optional)

- `TURI_CAP_IMPORT` grants a plugin env the ability to `(import ...)`
  from other plugins' directories.
- Without it, `(import ...)` resolves only within the plugin's own
  directory (the per-plugin module base dir).

## 4. Risks

- **Shared spice image stability.** `turi_env_set_shared_spice_image` is
  the designed mechanism but may not be stable in the pinned Turmeric
  version. Fallback: re-export natives per plugin env.
- **PluginContext lifetime.** The `ud` pointer passed to natives is
  currently a single heap-allocated `PluginContext`. With per-plugin envs,
  the context must either be shared (all plugins see the same
  `MainWindow*`) or per-plugin. Sharing is simpler and correct for v1 —
  the natives are the host's API, not the plugin's own state.
- **Hook dispatch across envs.** `HookBus` calls closures via
  `turi_call(env, closure, args, n)`. With per-plugin envs, each closure
  must be called in the env it was created in. `HookBus` needs to store
  the env alongside each closure.

## 5. Testing

- **Unit tests:** per-plugin env creation, capability grant/deny, manifest
  parsing.
- **Integration test:** load two plugins, verify one plugin's `defmodule`
  is not visible to the other, verify a plugin that calls
  `turi_env_reset` does not affect the other.
- **Permission prompt test:** load a plugin with capabilities, verify the
  prompt appears, verify the permission is cached.
