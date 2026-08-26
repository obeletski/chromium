# navigator.digitclassifier — Handoff

Implementation state, design decisions, and how to resume. Written 2026-08-20 against `f6fd8f0cdc96a`.

> **Source links.** Paths below link into
> [github.com/obeletski/chromium](https://github.com/obeletski/chromium) on the
> `digitclassifier` branch. Files under `third_party/dawn/` are deliberately not
> linked: dawn is a git submodule, so its contents are not in this repository.

> [!IMPORTANT]
> **Partly superseded by [DigitClassifier-Session-2026-08-23.md](DigitClassifier-Session-2026-08-23.md).**
> The feature now works end to end on hardware: the WGSL compiles and runs on an
> **Adreno 830** and all four `classify()` answers match the CPU oracle. So the
> "nothing has ever executed on a GPU" line below is out of date, the Adreno 750
> this document targets is not the device that was used, and the
> `chrome_public_apk install` step in [How to resume](#how-to-resume) cannot work
> over the current adb tunnel. The design decisions and file inventory here
> remain accurate.

> **State in one line:** everything up to and including "the weights are byte-verified inside the built APK" is done and checked; **nothing has ever executed on a GPU** — no device has been attached, and the WGSL has never been through a compiler.

---

## Contents

- [The goal](#the-goal)
- [The model](#the-model)
- [Design decisions](#design-decisions)
- [What exists now](#what-exists-now)
- [Verification status](#verification-status)
- [How to resume](#how-to-resume)
- [Traps](#traps)
- [Open items](#open-items)

---

## The goal

Add a `navigator.digitclassifier` Web API to Chromium on Android that runs a bundled MNIST digit classifier on the Qualcomm GPU of a OnePlus 13R (Snapdragon 8 Gen 3, **Adreno 750**).

Requested JS surface:

```js
const model = await navigator.digitclassifier.getModel();
const digit = await model.classify(inputBuffer);   // "0".."9"
```

`inputBuffer` is a `Float32Array` of **2352** values (28 × 28 × 3, NHWC, each normalized to `[0,1]`), built from a canvas via `getImageData` and a grayscale conversion that writes the same value into all 3 channels.

**Deviation from the original ask, agreed with the user:** both calls return Promises. The original sketch had them synchronous, which is impossible — model load reads a resource and builds GPU pipelines; classify is a GPU dispatch plus readback.

---

## The model

Parsed from `digit_classifier.tflite` (1,212,797 bytes, flatbuffer schema 3, `"MLIR Converted."`). Verified by an in-tree parser, not assumed:

```
input  [1,28,28,3] f32
  RESHAPE                                        -> [1, 2352]
  FULLY_CONNECTED  W[128,2352]  b[128]  act=RELU -> [1, 128]
  FULLY_CONNECTED  W[10,128]    b[10]   act=NONE -> [1, 10]
  SOFTMAX                                        -> [1, 10]
```

A pure two-layer MLP. No conv, no pooling.

| Tensor | tflite name | Shape | Floats | Bytes |
|---|---|---|---|---|
| W1 | `sequential/dense/MatMul` | [128, 2352] | 301,056 | 1,204,224 |
| b1 | `dense/bias` | [128] | 128 | 512 |
| W2 | `sequential/dense_1/MatMul` | [10, 128] | 1,280 | 5,120 |
| b2 | `dense_1/bias` | [10] | 10 | 40 |
| | | | **302,474** | **1,209,896** |

Operand order was confirmed from the ops themselves, not guessed: `FULLY_CONNECTED in=[6,4,2]` → W=tensor 4, b=tensor 2; `in=[7,5,1]` → W=tensor 5, b=tensor 1. TFLite stores FC weights row-major `[units, input_size]`, which is the layout the shader assumes.

Total compute is ~302K MACs ≈ **0.6 MFLOP** per inference.

> ⚠️ **This model is far too small to benefit from GPU execution.** The GPU→CPU readback alone will likely cost more than running the whole thing on the CPU. The user was told this explicitly and chose GPU anyway — it is a stated requirement, not an oversight. Do not "optimize" by silently moving it to the CPU. If a benchmark shows the GPU losing, that is expected, not a bug.

---

## Design decisions

### 1. Execution backend: WGSL compute on Dawn

Four candidates were investigated in-tree. Three are **not available in this checkout**:

| Candidate | Verdict | Evidence |
|---|---|---|
| TFLite GPU delegate (OpenCL/GLES) | Not built | Source exists at `third_party/litert/src/tflite/delegates/gpu`, but `grep -c "delegates/gpu" third_party/litert/BUILD.gn` → **0** |
| Optimization Guide GPU delegate | Unavailable on Android | `webnn_use_chrome_ml_api = enable_ml_internal`; `enable_ml_internal` requires `use_on_device_model_service = is_win \|\| is_mac \|\| is_linux \|\| is_ios \|\| is_cbx` — **Android absent** ([`services/on_device_model/on_device_model.gni:17`](https://github.com/obeletski/chromium/blob/digitclassifier/services/on_device_model/on_device_model.gni#L17)) |
| Qualcomm QNN / NPU | Unavailable | [`third_party/litert/BUILD.gn`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/litert/BUILD.gn) builds only `litert_qualcomm_options.{h,cc}` (the options struct). The dispatch backend needs Qualcomm's proprietary `.so`, not in tree |
| WebNN's existing TFLite path | Wrong processor | Works on Android but via **XNNPACK, which is CPU** ([`services/webnn/tflite/graph_impl_litert.cc:58`](https://github.com/obeletski/chromium/blob/digitclassifier/services/webnn/tflite/graph_impl_litert.cc#L58)) |
| **WebGPU compute via Dawn** | **✅ Chosen** | Fully working on Android/Vulkan/Adreno today |

### 2. Process architecture: renderer + dawn wire

**User chose this** over a WebNN-style GPU-process service.

```
Blink DigitClassifierModel
  └─ DawnControlClientHolder  (platform/graphics/gpu)
       └─ wgpu::Device / Buffer / ComputePipeline
            └─ dawn wire ──► GPU process ──► Vulkan ──► Adreno 750
```

Execution still happens on the Adreno in the GPU process; only the *command origin* is the renderer. Cost: ~3 files in one subproject, **no Mojo**. The rejected alternative (`services/digit_classifier/` + viz `GpuClient` + `browser_interface_binders`, mirroring WebNN's path at [`content/browser/browser_interface_binders.cc:1142`](https://github.com/obeletski/chromium/blob/digitclassifier/content/browser/browser_interface_binders.cc#L1142)) was ~10 files across four subprojects.

### 3. Weights delivery: Blink resource pack, NOT APK assets

**This decision reversed an earlier one and matters a lot.** The first plan bundled the weights via `android_assets` + `base::android::OpenApkAsset()`. That would have failed on device.

[`base/android/apk_assets.h:19`](https://github.com/obeletski/chromium/blob/digitclassifier/base/android/apk_assets.h#L19) says *"Can be used from renderer process."* **That comment is misleading.** `OpenApkAsset` calls through JNI into the Java `AssetManager` ([`base/android/apk_assets.cc`](https://github.com/obeletski/chromium/blob/digitclassifier/base/android/apk_assets.cc)), and a sandboxed Android renderer is an `isolatedProcess` with no such access. How child processes really get file data:

- [`content/browser/child_process_launcher_helper_android.cc:290`](https://github.com/obeletski/chromium/blob/digitclassifier/content/browser/child_process_launcher_helper_android.cc#L290) — `OpenFileToShare()`, browser-side
- [`base/android/child_process_service.cc:46`](https://github.com/obeletski/chromium/blob/digitclassifier/base/android/child_process_service.cc#L46) — children receive pre-opened FDs into a `FileDescriptorStore` at launch

That is how ICU and the V8 snapshot reach the renderer. Using APK assets would have forced browser↔renderer IPC back into the design, defeating decision #2.

**The replacement is strictly better:** ship the blob as a Blink resource.

- `IDR_DIGIT_CLASSIFIER_WEIGHTS` in [`third_party/blink/public/blink_resources.grd`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/public/blink_resources.grd), `type="BINDATA"`, **no compression** (float32 weights don't compress, and it avoids decompress cost)
- Read with `UncompressResourceAsBinary()` ([`platform/data_resource_helper.h:26`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/platform/data_resource_helper.h#L26)), which works in the renderer
- `blink_resources.pak` is already packed into Chrome ([`chrome/chrome_paks.gni:155`](https://github.com/obeletski/chromium/blob/digitclassifier/chrome/chrome_paks.gni#L155))
- Precedent for a binary blob: `IDR_AUDIO_SPATIALIZATION_COMPOSITE`, a `.flac` with the same attributes
- Bonus: works on desktop too, not Android-specific

The [`chrome/android/BUILD.gn`](https://github.com/obeletski/chromium/blob/digitclassifier/chrome/android/BUILD.gn) asset target added for the first approach **was reverted**; that file is unmodified.

### 4. Blob format, not the .tflite

The runtime never parses flatbuffers. Weights are extracted offline into a flat, self-describing blob:

```
0   char[4]  magic "DCLS"
4   u32      version = 1
8   u32      input_size  = 2352
12  u32      hidden_size = 128
16  u32      output_size = 10
20  u32[3]   reserved
32  f32[128*2352] W1 (row-major)
    f32[128]      b1
    f32[10*128]   W2 (row-major)
    f32[10]       b2
```

Total 1,209,928 bytes. The loader rejects wrong size, bad magic, wrong version, **and shapes that disagree with the compiled-in constants** — the shader is compiled against 2352/128/10, so a different network must be rejected rather than reinterpreted.

### 5. Input stays 2352 (channel folding declined)

Because the JS writes the same grayscale value into all 3 channels, W1 could be pre-summed in groups of 3, giving an exact 784-input equivalent — 1.15 MiB → 398 KiB and 3× less compute. **The user chose to keep 2352 as-is.** Do not apply this without asking; it changes the public input contract.

### 6. Softmax omitted from the shader

`argmax(softmax(z)) == argmax(z)` because softmax is monotonic, and `classify()` only returns the winner. Saves a shader stage. Re-add only if confidence scores are ever exposed. [`reference_infer.py`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/modules/digitclassifier/tools/reference_infer.py) asserts this equivalence holds.

### 7. Single dispatch, single workgroup

At 0.6 MFLOP, dispatch and readback dominate, so minimizing round trips beats parallelism. The whole network is one `dispatchWorkgroups(1)` of `@workgroup_size(128)`:

- layer 1: one invocation per hidden unit → `workgroupBarrier()`
- layer 2: first 10 invocations → `workgroupBarrier()`
- invocation 0 does argmax and writes the result

Both barriers sit in uniform control flow. Workgroup storage is 552 bytes against a 16 KiB floor.

### 8. Qualcomm note: ReLU is deliberately scalar

The ReLU is `max(acc, 0.0)` on a **scalar**. `Toggle::ScalarizeMaxMinClamp` — which Dawn enables on Qualcomm for [crbug 407109052](https://crbug.com/407109052) — only fires on **vector** results (`core/ir/transform/builtin_scalarize.cc:69` bails otherwise). So it is currently a no-op here. **If anyone vectorizes the dot product with `vec4` loads, vector `max` will start being scalarized** and that tradeoff becomes live. See [`docs/digitclassifier/WGSL-Compilation-Path.md`](WGSL-Compilation-Path.md) for the full Qualcomm workaround map.

### 9. Fresh readback buffer per classify() call

Everything else is ordered by the queue, so shared input/result buffers are safe across overlapping calls. A mapped range is not — hence a new 4-byte readback buffer each call.

---

## What exists now

### New: `third_party/blink/renderer/modules/digitclassifier/`

| File | Role |
|---|---|
| `navigator_digit_classifier.{h,cc,idl}` | `Supplement<NavigatorBase>`, modelled on `modules/ml/navigator_ml` |
| `digit_classifier.{h,cc,idl}` | `getModel()`; owns the async context-provider → adapter → device chain |
| `digit_classifier_model.{h,cc,idl}` | `classify()`; buffers, WGSL, dispatch, readback |
| `digit_classifier_weights.{h,cc}` | Loads/validates `IDR_DIGIT_CLASSIFIER_WEIGHTS`, exposes 4 `base::span<const float>` |
| `BUILD.gn` | `blink_modules_sources`, deps on `//third_party/blink/public:resources` |
| [`resources/digit_classifier_weights.bin`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/modules/digitclassifier/resources/digit_classifier_weights.bin) | 1,209,928 bytes |
| [`tools/tflite_dump.py`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/modules/digitclassifier/tools/tflite_dump.py) | Stdlib-only TFLite flatbuffer reader (no `flatbuffers`/`numpy`/`schema.fbs` in this checkout) |
| [`tools/extract_weights.py`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/modules/digitclassifier/tools/extract_weights.py) | .tflite → .bin |
| [`tools/reference_infer.py`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/modules/digitclassifier/tools/reference_infer.py) | **CPU oracle** — pure-python forward pass, the GPU must match it |

### Modified (5 files, small edits)

| File | Change |
|---|---|
| `blink_resources.grd` | `IDR_DIGIT_CLASSIFIER_WEIGHTS` (id **45307**) |
| [`bindings/idl_in_modules.gni`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/bindings/idl_in_modules.gni) | 3 `.idl` paths |
| [`bindings/generated_in_modules.gni`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/bindings/generated_in_modules.gni) | 4 generated `v8_digit_classifier*` paths |
| [`modules/BUILD.gn`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/modules/BUILD.gn) | dep on the new module |
| [`platform/runtime_enabled_features.json5`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/platform/runtime_enabled_features.json5) | `DigitClassifier`, `status: "test"` |

### Untracked, not part of the feature

`digit_classifier.tflite` at repo root — a build *input* only. Extraction is done; it does not need to ship. `CLAUDE.md` is also untracked. For the complete list of what this branch adds and modifies, see [DigitClassifier-File-Inventory.md](DigitClassifier-File-Inventory.md).

---

## Verification status

Be precise about this; do not overstate it.

| Step | State | How it was checked |
|---|---|---|
| Model architecture matches spec | ✅ | [`tools/tflite_dump.py`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/modules/digitclassifier/tools/tflite_dump.py) |
| Weight extraction correct | ✅ | [`reference_infer.py`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/modules/digitclassifier/tools/reference_infer.py): synthetic "1"→1.0000, "7"→0.9593, "0"→0.9984, blank→near-uniform. A transposed W1 would produce noise |
| Blink module compiles | ✅ | `autoninja … modules/digitclassifier:digitclassifier` |
| IDL valid | ✅ | `bindings:validate_web_idl` |
| Generated bindings match C++ signatures | ✅ | Read `out/Default/gen/.../v8_digit_classifier*.cc` |
| Bindings file lists in sync | ✅ | `bindings:check_generated_file_list` |
| `chrome_public_apk` builds | ✅ | 715,197,274 bytes |
| Weights present in shipped APK | ✅ | Parsed `assets/resources.pak` from the APK: id 45307, offset 1911132, **size 1209928**, first bytes `DCLS\x01\0\0\0\x30\x09\0\0\x80\0\0\0` |
| **WGSL compiles** | ❌ | No `tint` CLI in Chromium builds (`tint_executable` gated off). Hand-verified only |
| **`navigator.digitclassifier` exists at runtime** | ❌ | No device attached |
| **Inference correct on Adreno** | ❌ | Never run |

---

## How to resume

### 1. Attach the OnePlus 13R

`adb` is **not** on PATH. Run this from the `src/` root of the checkout:

```sh
export PATH="$PWD/third_party/android_sdk/public/platform-tools:$PATH"
adb devices          # must list the device, not "unauthorized" and not empty
```

### 2. Install and launch

```sh
out/Default/bin/chrome_public_apk install
out/Default/bin/chrome_public_apk launch \
  --args="--enable-blink-features=DigitClassifier"
```

The APK is a 715 MB debug build; install takes minutes over USB.

### 3. Smoke test in DevTools

```js
typeof navigator.digitclassifier          // "object" if the flag took
const m = await navigator.digitclassifier.getModel();
```

`getModel()` rejecting is the expected first failure mode — most likely `CreateShaderModule` failing, since **the WGSL has never been compiled**. Get the real message from:

```sh
adb logcat | grep -iE "chromium|dawn|tint|digitclassifier"
```

### 4. Validate against the oracle

Run [`tools/reference_infer.py`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/modules/digitclassifier/tools/reference_infer.py), then feed the same synthetic digits through `classify()` in the page. They must agree: **"1", "7", "0"**. Only after that is the GPU path trustworthy.

### 5. Rebuild loop

```sh
autoninja --quiet -C out/Default third_party/blink/renderer/modules/digitclassifier:digitclassifier   # fast, C++ only
autoninja --quiet -C out/Default chrome_public_apk                                                     # slow, needed to deploy
```

If the weights blob ever changes, re-run [`tools/extract_weights.py`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/modules/digitclassifier/tools/extract_weights.py) and rebuild — the pak is regenerated automatically from the grd.

---

## Traps

Things that already cost time, or that will bite silently.

**Background build exit codes lie.** `autoninja … | tail` in a background task reports the *pipeline's* exit code, not ninja's. A failed build was reported as "exit code 0" once. **Always** verify with `grep -cE "FAILED|error:"` on the output file plus the artifact's mtime — not the notification.

**Two gni files, not one.** [`idl_in_modules.gni`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/bindings/idl_in_modules.gni) lists *source* `.idl` paths; [`generated_in_modules.gni`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/bindings/generated_in_modules.gni) lists *generated* `v8_*.{cc,h}` paths. Missing the second fails late, in `check_generated_file_list`, only during a full build. Grepping the second for a source path finds nothing and is misleading.

**`WrapCallbackInScriptScope` injects the resolver as the first *unbound* parameter.** All bound args must precede it. `OnResultMapped(readback, resolver, status, msg)` is correct; putting `resolver` first produces an inscrutable `Type mismatch between bound argument and bound functor's parameter` template wall.

**It's `blink::BindOnce`, not `WTF::BindOnce`.**

**`-Wunsafe-buffer-usage` rejects `memcpy`/`memcmp`/pointer arithmetic.** Use `base::span` + `copy_from`. And `float` fails `CanSafelyConvertToByteSpan` (non-unique object representations — NaN payloads, ±0), so byte conversions need the explicit `base::allow_nonunique_obj` tag.

**`DOMTypedArray::length()` returns `size_t`** (`%zu`, not `%u`). Use `AsSpan()` for the data.

**An emulator will not validate the Adreno path.** SwiftShader or the host GPU would run the compute instead, so none of the Qualcomm workarounds are exercised.

---

## Open items

- [ ] **Run it.** Everything below the line in [Verification status](#verification-status) is unproven.
- [ ] `OWNERS` file for the new module (presubmit will want one).
- [ ] Replace `TODO(crbug.com/None)` markers with a real bug number.
- [ ] Web tests under `third_party/blink/web_tests/` for the IDL surface.
- [ ] Per [`agents/ai_policy.md`](https://github.com/obeletski/chromium/blob/digitclassifier/agents/ai_policy.md): flag AI-assisted areas in the CL description; be able to explain every line.
- [ ] **Launch process.** `navigator.digitclassifier` is a non-standard web-exposed API. Behind `status: "test"` it's fine for experimentation; shipping needs spec/TAG/API-owner review. Scope was explicitly experimental.
- [ ] Decide whether `digit_classifier.tflite` and the `tools/` scripts belong in the CL or stay local.
