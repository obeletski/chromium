# navigator.digitclassifier: file inventory

Every file added or modified to build the custom Chromium APK that exposes
`navigator.digitclassifier`. Nothing outside this list was touched.

> **Source links.** Paths below link into
> [github.com/obeletski/chromium](https://github.com/obeletski/chromium) on the
> `digitclassifier` branch. Files under `third_party/dawn/` are deliberately not
> linked: dawn is a git submodule, so its contents are not in this repository.

Baseline: `f6fd8f0cdc96a` ("Roll Chrome Mac Arm PGO Profile"), the detached HEAD
the `digitclassifier` branch was cut from.

    git diff --stat f6fd8f0cdc96a..digitclassifier
    26 files changed, 2699 insertions(+)

Five pre-existing files, twenty-one new ones. The cumulative diff has **no
deletions at all**: the five files it touches only gain lines. (That count
excludes this inventory, which is the 27th file.)

## Contents

- [Modified: pre-existing files](#modified-pre-existing-files)
- [Added: the module](#added-the-module)
- [Added: bundled weights](#added-bundled-weights)
- [Added: offline tools](#added-offline-tools)
- [Added: documentation](#added-documentation)
- [Not in the branch](#not-in-the-branch)
- [Build configuration](#build-configuration)
- [Commit breakdown](#commit-breakdown)

## Modified: pre-existing files

Five files, fifteen added lines between them. Four are the registration points —
a new Blink module is invisible to the build until every one of them knows about
it — and the fifth ships the weights.

| File | + | What was added |
|---|---|---|
| [`third_party/blink/renderer/modules/BUILD.gn`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/modules/BUILD.gn) | 1 | `//third_party/blink/renderer/modules/digitclassifier` in the `modules` component's `deps`. Without it the module compiles but never links. |
| [`third_party/blink/renderer/bindings/idl_in_modules.gni`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/bindings/idl_in_modules.gni) | 3 | The three `.idl` files, so the bindings generator reads them. |
| [`third_party/blink/renderer/bindings/generated_in_modules.gni`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/bindings/generated_in_modules.gni) | 4 | The four generated `v8_digit_classifier*.{cc,h}` paths. This list is *not* derived from the one above; both must be edited or the generated files are produced and then dropped. |
| [`third_party/blink/renderer/platform/runtime_enabled_features.json5`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/platform/runtime_enabled_features.json5) | 4 | The `DigitClassifier` feature, `status: "test"` — off unless the browser is started with `--enable-blink-features=DigitClassifier`. |
| [`third_party/blink/public/blink_resources.grd`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/public/blink_resources.grd) | 3 | `IDR_DIGIT_CLASSIFIER_WEIGHTS`, `type="BINDATA"`, deliberately **uncompressed** (float32 weights are incompressible, and this keeps the model load allocation-free). This is how the weights reach the renderer at all — a renderer cannot open APK assets. |

## Added: the module

`third_party/blink/renderer/modules/digitclassifier/` — 13 files, ~1000 lines.

| File | Lines | Role |
|---|---|---|
| `BUILD.gn` | 23 | `blink_modules_sources("digitclassifier")`. Deps: `//gpu/command_buffer/client:webgpu_interface` and `//third_party/blink/public:resources`. |
| [`navigator_digit_classifier.idl`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/modules/digitclassifier/navigator_digit_classifier.idl) | 11 | The `NavigatorDigitClassifier` mixin; `Navigator includes NavigatorDigitClassifier`. |
| [`navigator_digit_classifier.h`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/modules/digitclassifier/navigator_digit_classifier.h) / `.cc` | 34 / 33 | `Supplement<Navigator>` holding the lazily-created `DigitClassifier`. |
| [`digit_classifier.idl`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/modules/digitclassifier/digit_classifier.idl) | 15 | `interface DigitClassifier { Promise<DigitClassifierModel> getModel(); }`. |
| [`digit_classifier.h`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/modules/digitclassifier/digit_classifier.h) / `.cc` | 74 / 227 | Acquires the WebGPU context provider, adapter and device, then resolves with a model. **Both runtime bugs lived here** — see the commit breakdown. |
| [`digit_classifier_model.idl`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/modules/digitclassifier/digit_classifier_model.idl) | 15 | `interface DigitClassifierModel { Promise<DOMString> classify(Float32Array); }`. |
| [`digit_classifier_model.h`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/modules/digitclassifier/digit_classifier_model.h) / `.cc` | 91 / 308 | The WGSL shader source, pipeline construction, buffer uploads, dispatch and readback. The single largest file. |
| [`digit_classifier_weights.h`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/modules/digitclassifier/digit_classifier_weights.h) / `.cc` | 57 / 118 | Loads and validates `IDR_DIGIT_CLASSIFIER_WEIGHTS`, exposing typed spans over the four tensors. |

## Added: bundled weights

| File | Size | Notes |
|---|---|---|
| `.../digitclassifier/resources/digit_classifier_weights.bin` | 1,209,928 B | Raw float32, extracted from the `.tflite`. Ships inside the APK via the `.grd` entry above and adds ~1.2 MB to it. |

## Added: offline tools

`third_party/blink/renderer/modules/digitclassifier/tools/` — none of this is
compiled into the browser; it is what produced and checked the weights.

| File | Size | Role |
|---|---|---|
| [`tflite_dump.py`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/modules/digitclassifier/tools/tflite_dump.py) | 5.0 KB | Parses the TFLite flatbuffer and prints its tensors — used to work out the layout before trusting anything. |
| [`extract_weights.py`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/modules/digitclassifier/tools/extract_weights.py) | 3.5 KB | Produces [`digit_classifier_weights.bin`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/modules/digitclassifier/resources/digit_classifier_weights.bin) from the `.tflite`. |
| [`reference_infer.py`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/modules/digitclassifier/tools/reference_infer.py) | 4.4 KB | CPU NumPy forward pass. This is the **oracle** the on-device results were checked against (4/4). |
| [`smoke_test.html`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/modules/digitclassifier/tools/smoke_test.html) | 3.7 KB | The on-device page: calls `getModel()`, then `classify()` on four fixed inputs, and prints pass/fail. |
| [`serve_smoke_test.sh`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/modules/digitclassifier/tools/serve_smoke_test.sh) | 1.9 KB | Serves that page from the device's own `127.0.0.1` — the API is `[SecureContext]`, so `file://` and `adb reverse` both fail. |

## Added: documentation

All four live in `docs/digitclassifier/`.

| File | Size | Role |
|---|---|---|
| [`DigitClassifier-Handoff.md`](DigitClassifier-Handoff.md) | 16 KB | The reference doc: goal, model, every design decision with its evidence, verification table, resume commands, traps, open items. |
| [`DigitClassifier-Session-2026-08-23.md`](DigitClassifier-Session-2026-08-23.md) | 18 KB | The first-successful-run log. Supersedes parts of the handoff — it has its own "Corrections to the handoff doc" section. Carries both bug write-ups, the device access recipe, and the smoke-test steps. |
| [`WGSL-Compilation-Path.md`](WGSL-Compilation-Path.md) | 24 KB | Background research on the WGSL → Tint → SPIR-V path and Dawn's per-GPU workaround system. Written before the feature; not specific to it. |
| `DigitClassifier-File-Inventory.md` | 8.2 KB | This file. |

## Not in the branch

Two files sit in the working tree, untracked on purpose:

| File | Size | Why untracked |
|---|---|---|
| `CLAUDE.md` | 7.6 KB | Instructions for *this checkout* (Android-only, local-only builds, [`autotest.py`](https://github.com/obeletski/chromium/blob/digitclassifier/tools/autotest.py), the `git cl` flow, layering rules). Not upstream's business. |
| `digit_classifier.tflite` | 1.2 MB | The source model the weights were extracted from. The extracted `.bin` is what ships; keeping both in the tree would double the cost for nothing. |

Also absent, and required before this could land: an `OWNERS` file for the
module, replacements for the `TODO(crbug.com/None)` markers, and web tests under
`third_party/blink/web_tests/`.

## Build configuration

Not part of the diff, but the APK does not build without them.

| File | Config |
|---|---|
| `out/Default/args.gn` | `target_os="android"`, `target_cpu="arm64"`, `use_remoteexec=false`, `is_component_build=false`. Debug — this is where the feature was developed. |
| `out/Release/args.gn` | The same plus `is_debug=false`, `dcheck_always_on=false`, `symbol_level=1`. Added 2026-08-24. **Use this one for anything performance- or correctness-related**: the debug build was measured changing a GPU inference's *answer*, not just its timing. |

APK sizes: 715,197,274 B debug against 389,907,305 B release.

Target: `autoninja --quiet -C out/Release chrome_public_apk`.

## Commit breakdown

Which files each of the seven commits touched. Two are `fixup!`s awaiting a
`git rebase --autosquash`.

| Commit | Files |
|---|---|
| `3c414d12a8a4b` Add navigator.digitclassifier | 23 files, +2150 — the whole module, weights, tools, the 4 registration edits, and 2 of the docs |
| `a29ea95bd7e6c` Bind the WebGPU context provider… | [`digit_classifier.cc`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/modules/digitclassifier/digit_classifier.cc) (+5/−1) — the missing `BindToCurrentSequence()` that crashed the renderer on `DCHECK(bind_tried_)` |
| `f1b678daad675` Send the WebGPU execution context token… | [`digit_classifier.cc`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/modules/digitclassifier/digit_classifier.cc) (+14), `BUILD.gn` (+5/−1) — the missing `SetWebGPUExecutionContextToken()` that left `getModel()` pending forever |
| `5ee8c5bc3ba4c` Add the on-device smoke test and a session log | [`smoke_test.html`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/modules/digitclassifier/tools/smoke_test.html), [`serve_smoke_test.sh`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/modules/digitclassifier/tools/serve_smoke_test.sh), the session doc, handoff banner |
| `6f6fd79571756` **fixup!** of the above | [`smoke_test.html`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/modules/digitclassifier/tools/smoke_test.html) (+31/−2) |
| `9d9c1bb2b94d5` Record the first successful on-device run | both docs (+146/−50) |
| `4e4c9cab0c6e9` **fixup!** of the above | session doc (+6/−3) — corrected the claim that Dawn's Qualcomm workarounds were keyed to the wrong GPU; they are keyed by *vendor*, not model |

Only three commits contain shippable code. The other four are documentation and
test tooling.
