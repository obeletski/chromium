# navigator.digitclassifier — Session log, 2026-08-23

First session in which the feature ran on real hardware. Written against base
commit `f6fd8f0cdc96a`, branch `digitclassifier`.

> **Source links.** Paths below link into
> [github.com/obeletski/chromium](https://github.com/obeletski/chromium) on the
> `digitclassifier` branch. Files under `third_party/dawn/` are deliberately not
> linked: dawn is a git submodule, so its contents are not in this repository.

> **State in one line:** the feature works end to end on an Adreno 830 device.
> Both bugs found today are fixed and verified on hardware; `getModel()`
> resolves, our WGSL compiles, and all four `classify()` answers match the CPU
> oracle.

Companion documents:
- [DigitClassifier-Handoff.md](DigitClassifier-Handoff.md) — design decisions,
  file inventory, why each backend was ruled out. Still the primary reference,
  but see [Corrections to the handoff doc](#corrections-to-the-handoff-doc).
- [WGSL-Compilation-Path.md](WGSL-Compilation-Path.md) — the WGSL→SPIR-V path.
- [DigitClassifier-File-Inventory.md](DigitClassifier-File-Inventory.md) — every
  file this branch adds or modifies.

---

## Contents

- [What changed today](#what-changed-today)
- [Bug 1: renderer crash (fixed, verified)](#bug-1-renderer-crash-fixed-verified)
- [Bug 2: getModel() hangs (fixed, verified)](#bug-2-getmodel-hangs-fixed-verified)
- [Verification status](#verification-status)
- [Result: the feature works](#result-the-feature-works)
- [The device and how to reach it](#the-device-and-how-to-reach-it)
- [How to run the smoke test](#how-to-run-the-smoke-test)
- [How to resume](#how-to-resume)
- [Corrections to the handoff doc](#corrections-to-the-handoff-doc)
- [Traps learned today](#traps-learned-today)

---

## What changed today

The work was uncommitted at the start of the session; it is now on branch
`digitclassifier`, off detached HEAD `f6fd8f0cdc96a`.

| Commit | Contents |
|---|---|
| `3c414d12a8a4b` | Add navigator.digitclassifier — the whole pre-existing feature, 23 files. Includes both docs and `tools/`. |
| `a29ea95bd7e6c` | Bind the WebGPU context provider before creating the Dawn client ([bug 1](#bug-1-renderer-crash-fixed-verified)). |
| *(this commit)* | Send the WebGPU execution context token ([bug 2](#bug-2-getmodel-hangs-fixed-verified)), plus this document and the smoke-test harness. |

Deliberately **not** committed: `digit_classifier.tflite` (source model, stays
local) and `CLAUDE.md` (local agent config, not Chromium's).

---

## Bug 1: renderer crash (fixed, verified)

**Symptom.** The renderer died before any page script ran:

```
F/chromium: [FATAL:services/viz/public/cpp/gpu/context_provider_command_buffer.cc:565]
            DCHECK failed: bind_tried_.
```

**Cause.** Line 565 is the first line of
`ContextProviderCommandBuffer::WebGPUInterface()`. `DawnControlClientHolder`'s
constructor calls that from its initializer list:

```cpp
api_channel_(context_provider_->ContextProvider().WebGPUInterface()->GetAPIChannel())
```

which requires a provider already bound to the sequence.
`DigitClassifier::EnsureDawnControlClient()` passed the freshly created provider
straight to `DawnControlClientHolder::Create()`.

**Fix.** Call `BindToCurrentSequence()` first and treat failure as no context,
matching [`modules/webgpu/gpu.cc:246`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/modules/webgpu/gpu.cc#L246).

**Verified** on device: no FATAL, and `navigator.digitclassifier` reports
`object` in a page for the first time.

---

## Bug 2: getModel() hangs (fixed, verified)

**Symptom.** `getModel()` never settles — no resolve, no reject. The page's
20-second race timer was the only thing that ended the wait.

**Cause.** [`gpu/command_buffer/service/webgpu_decoder_impl.cc:1360`](https://github.com/obeletski/chromium/blob/digitclassifier/gpu/command_buffer/service/webgpu_decoder_impl.cc#L1360):

```cpp
if (isolation_key_) {
  callback_info.callback(WGPURequestAdapterStatus_Success, ...);
} else {
  // We can't RequestDevice until we have an isolation key. Defer the
  // RequestAdapter callback until we do.
  deferred_request_adapter_callbacks_.emplace_back(...);
}
```

`isolation_key_` is populated only by `OnGetIsolationKey()`, which runs only
from `HandleSetWebGPUExecutionContextToken()` (same file, line 2509), which
fires only when the client sends `SetWebGPUExecutionContextToken`.
[`modules/webgpu/gpu.cc:325`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/modules/webgpu/gpu.cc#L325) sends it; our code did not. So the service parked
the `RequestAdapter` reply on `deferred_request_adapter_callbacks_` forever.

A hang rather than a rejection is the diagnostic signature of this bug: every
error path in [`digit_classifier.cc`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/modules/digitclassifier/digit_classifier.cc) rejects, so a pending-forever promise means
a callback that never arrives, not a failure that was swallowed.

**Fix.** Before constructing the Dawn client, send the document token:

```cpp
context_provider->WebGPUInterface()->SetWebGPUExecutionContextToken(
    To<LocalDOMWindow>(context)->document()->Token());
```

`DigitClassifier` is `Exposed=Window`, so the document token is the only case;
`GetExecutionContextToken()` in [`modules/webgpu/gpu.cc`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/modules/webgpu/gpu.cc) has the worker variants
if that ever changes.

This required a new dep in the module's `BUILD.gn` —
`//gpu/command_buffer/client:webgpu_interface`. Without it the code compiles but
`gn check` fails, because the header is only reachable through a `--[private]-->`
edge from `//third_party/blink/renderer/platform:platform`.

**Verified on hardware.** With this build installed (19:32), `getModel()`
resolves in 40 ms and the four `classify()` calls that follow all match the
oracle. Before the fix the same page reported `getModel TIMEOUT` after 20 s.

---

## Verification status

Supersedes the table in the handoff doc.

| Step | State | How it was checked |
|---|---|---|
| Model architecture, weight extraction | ✅ | [`tools/reference_infer.py`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/modules/digitclassifier/tools/reference_infer.py), unchanged from before |
| Blink module compiles, IDL valid, bindings in sync | ✅ | unchanged from before |
| `chrome_public_apk` builds | ✅ | 715,197,274 bytes |
| Weights present in shipped APK | ✅ | unchanged from before |
| **APK installs and runs on device** | ✅ | installed 15:57:49, 16:48:39, 19:32:04 |
| **Runtime flag takes effect** | ✅ | `cr_CommandLine: COMMAND-LINE FLAGS: [...] (from /data/local/tmp/chrome-command-line)` |
| **`navigator.digitclassifier` exists at runtime** | ✅ | page reports `object` |
| **Renderer survives `getModel()`** | ✅ | no FATAL after the bug-1 fix |
| **WebGPU works on this device** | ✅ | baseline page: adapter in 45 ms (`vendor=qualcomm arch=adreno-8xx`), device in 9 ms |
| **Dawn compiles WGSL on this device** | ✅ | a trivial compute shader compiled, `messages=0` |
| **`getModel()` resolves** | ✅ | 40 ms, on the build carrying the bug-2 fix |
| **Our WGSL compiles** | ✅ | `classify()` dispatches; no Dawn/Tint error in logcat |
| **Inference matches the CPU oracle** | ✅ | **4/4**, see [Result](#result-the-feature-works) |

The oracle values to check against, from
`tools/reference_infer.py resources/digit_classifier_weights.bin`:

| input | expected | confidence |
|---|---|---|
| synthetic 1 | `"1"` | 1.0000 |
| synthetic 7 | `"7"` | 0.9593 |
| synthetic 0 | `"0"` | 0.9984 |
| blank | `"5"` | 0.1428 (bias-only path; near-uniform is correct) |

---

## Result: the feature works

Run at 19:34 against the build installed at 19:32, page served from the
device's own `127.0.0.1:8111`:

```
navigator.gpu: present
requestAdapter -> adapter 79ms
  vendor=qualcomm arch=adreno-8xx desc=?
requestDevice -> device 9ms
trivial WGSL compiled, msgs=0
navigator.digitclassifier: object
getModel OK 40ms
classify 1     -> "1" want "1" PASS 116ms
classify 7     -> "7" want "7" PASS 11ms
classify 0     -> "0" want "0" PASS 8ms
classify blank -> "5" want "5" PASS 6ms
=== 4/4 match the CPU oracle ===
```

This is a real GPU result, not a fallback: the module has **no** CPU path.
[`digit_classifier_model.cc`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/modules/digitclassifier/digit_classifier_model.cc) builds its pipeline with `CreateShaderModule()`
(line 161) and `CreateComputePipeline()` (line 169) and runs it with
`DispatchWorkgroups()` (line 242), so a passing `classify()` means our WGSL was
compiled by Tint, lowered to SPIR-V, and executed on the Adreno.

The first `classify()` costs 116 ms and the rest 6–11 ms — pipeline creation and
shader compilation happen once, on the first dispatch.

Logcat is clean of Dawn and Tint diagnostics. The only WebGPU-adjacent warnings
are `maxDynamicUniformBuffersPerPipelineLayout artificially reduced from 32 to
16` (a Dawn/Adreno limit note) and `No Dawn device lost callback was set` —
neither is ours to fix, and neither affected the result.

---

## The device and how to reach it

**It is not the device the handoff doc names.** Attached today:

| | |
|---|---|
| serial | `e39ae043` |
| model | OnePlus 13, `CPH2653` |
| SoC | `SM8750` (Snapdragon 8 Elite) |
| GPU | **Adreno 830** (handoff doc targets Adreno 750) |
| OS | Android 16, API 36, arm64-v8a, user build |

**Topology.** `adb` here talks to the **laptop's** adb server through an SSH
reverse tunnel. `adb devices -l` shows `usb:3-6`, which is the *laptop's* USB
bus — there is no adb server on this machine (`pgrep -x adb` finds none; the
listener is sshd).

**The tunnel's port is not stable.** It was `5037` for most of the session and
moved to `5038` after the SSH sessions turned over. Nothing tells you this has
happened: the stale listener keeps *accepting* connections while nothing
answers, so every `adb` command hangs forever instead of failing. Diagnose it
with a local server on a free port —

```sh
$ADB -P 5039 devices   # binary healthy? sees local USB only (normally empty)
```

— and drive the real device by exporting the port, which every adb invocation
and every script honours:

```sh
export ANDROID_ADB_SERVER_PORT=5038
```

That is how [`serve_smoke_test.sh`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/modules/digitclassifier/tools/serve_smoke_test.sh) is run without editing it; the `$ADB` in it
has no `-P` flag.

Consequences, each of which cost time today:

- **`adb forward` / `adb reverse` bind on the laptop**, not here. A local HTTP
  server is unreachable from the device, and the DevTools port is unreachable
  from here — so CDP-driven testing does not work in this setup.
  `adb reverse` *appears* to succeed while doing nothing useful.
- **Throughput is the tunnel's, and it varies enormously.** Over the 5037
  tunnel it measured 480 KB/s, then 290 KB/s, and installing the 715 MB APK
  took **21 min**, then **44 min**. Over the 5038 tunnel the same install took
  **34 seconds**. Measure before assuming; do not plan around the slow figure.
- **`out/Default/bin/chrome_public_apk install` cannot work**: devil caps adb at
  300 s and raises `CommandTimeoutError`. Use plain adb:

  ```sh
  ADB=third_party/android_sdk/public/platform-tools/adb   # not on PATH
  $ADB -s e39ae043 install -r -d out/Default/apks/ChromePublic.apk
  ```

  Watch progress with `awk '/^rchar/{print $2}' /proc/$(pgrep -f 'adb.*install')/io`
  against 715,197,274. On the slow tunnel adb tried an incremental install,
  failed with `INSTALL_PARSE_FAILED_UNEXPECTED_EXCEPTION`, and fell back to a
  streamed install; on the fast one *Incremental Install* succeeded outright.

  After an incremental install, **check `loadingProgress`** — it streams pages
  from the host lazily, so until it reaches 100% the app depends on the adb
  connection staying up:

  ```sh
  $ADB -s e39ae043 shell 'dumpsys package org.chromium.chrome | grep -E "loadingProgress|lastUpdateTime"'
  ```

  Verify the right APK actually landed by comparing size and path hash against
  the local file, rather than trusting the `Success` line.

`chrome_public_apk launch` **does** work (its device operations are short).

---

## How to run the smoke test

The page cannot be loaded from the host, and both `file://` routes are dead
ends: Chrome returns `ERR_ACCESS_DENIED` for its own data directory, and
`/sdcard` is blocked by scoped storage at targetSdk 36 (`pm grant
READ_EXTERNAL_STORAGE` silently no-ops there). The page is therefore served
**by the device to itself** over `127.0.0.1`, which is a secure context — which
the API requires, since all three interfaces are `[SecureContext]`.

```sh
export ANDROID_ADB_SERVER_PORT=5038          # whatever the tunnel is on today
ADB="third_party/android_sdk/public/platform-tools/adb -s e39ae043"

# 1. serve the page from the device (holds the terminal open — background it)
third_party/blink/renderer/modules/digitclassifier/tools/serve_smoke_test.sh e39ae043 8111 &

# 2. set the flags and launch at the URL
$ADB shell 'echo "chrome --enable-blink-features=DigitClassifier --disable-fre --no-first-run" \
    > /data/local/tmp/chrome-command-line; chmod 644 /data/local/tmp/chrome-command-line'
$ADB logcat -c
$ADB shell 'am force-stop org.chromium.chrome'
$ADB shell 'am start -a android.intent.action.VIEW -d "http://127.0.0.1:8111/" \
    -n org.chromium.chrome/com.google.android.apps.chrome.Main'

# 3. read the result — the page prints into the DOM
$ADB shell screencap -p /data/local/tmp/s.png && $ADB pull /data/local/tmp/s.png /tmp/s.png
```

Writing the flags file and starting the intent directly is more reliable than
`chrome_public_apk launch`, and it does not need the wrapper's `-d` serial
handling to agree with `ANDROID_ADB_SERVER_PORT`. `am force-stop` first, or the
flags file is not re-read.

**Read results from the screen, not logcat.** `console.log()` from a page does
not reach logcat in this build, so [`tools/smoke_test.html`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/modules/digitclassifier/tools/smoke_test.html) renders every step
into the document instead. It also wraps each async call in a
`Promise.race` timeout, which is what turned "the page just sits there" into
"`getModel TIMEOUT`" — keep that when editing it.

`--enable-unsafe-webgpu` is available if adapter gating is ever suspected; it
made no difference today.

Ignore the launch warning `Your device is a user build; Chrome may or may not
pick up your commandline flags` — the flags did apply, because the APK is
debuggable. Confirm in logcat with `grep cr_CommandLine`.

---

## How to resume

The runtime work is done — the device runs a build in which the feature works
end to end. What remains is landing it, none of which needs hardware.

1. **Squash the fixup.** Branch `digitclassifier` carries a `fixup!` commit that
   was left for a human to apply: `git rebase --autosquash`. Per `CLAUDE.md`
   this repo never uses `git commit --amend`.
2. **Repo hygiene before review**: an `OWNERS` file for the module, real bug
   numbers for the `TODO(crbug.com/None)` markers, and web tests under
   `third_party/blink/web_tests/`. The on-device smoke test is a development
   tool, not a substitute for those.
3. **The launch-process question is still open** — this is a non-standard
   web-exposed API with no spec and no chromestatus entry, so it cannot land as
   an enabled feature. It is `status: "test"` in
   [`runtime_enabled_features.json5`](https://github.com/obeletski/chromium/blob/digitclassifier/third_party/blink/renderer/platform/runtime_enabled_features.json5), which is the honest setting.
4. **Nothing has been uploaded to Gerrit.** `git cl format` and
   `git cl presubmit -u --force` have not been run against these commits.
5. **If a rebuild is needed**, the fast inner loop is
   `autoninja --quiet -C out/Default third_party/blink/renderer/modules/digitclassifier:digitclassifier`;
   only a change that must reach the device needs `chrome_public_apk` plus an
   install.

---

## Corrections to the handoff doc

[`DigitClassifier-Handoff.md`](DigitClassifier-Handoff.md) predates any hardware run and is stale in three
places:

1. Its headline claim that **nothing has ever executed on a GPU** is no longer
   true. The WGSL runs on an Adreno 830 and its output matches the CPU oracle
   on all four inputs — see [Result](#result-the-feature-works).
2. It names a **OnePlus 13R / Snapdragon 8 Gen 3 / Adreno 750** as the target.
   The attached device is an Adreno 830, so any GPU-specific reasoning there
   names the wrong part. In practice this changed nothing: Dawn's Qualcomm
   workarounds are keyed on **vendor**, not model — `IsAndroidQualcomm()` and
   `MayBeQualcommProprietary()` (`vulkan/PhysicalDeviceVk.cpp:1442`, `:1507`)
   both test `gpu_info::IsQualcommPCI(GetVendorId())` — so the same set applies
   to both parts, and the shader needed no change to run correctly here.
3. Its resume section says to install with
   `out/Default/bin/chrome_public_apk install`, which cannot succeed over this
   tunnel — devil's 300 s cap kills it. Use plain `adb install`.

---

## Traps learned today

**Background exit codes really do lie.** The first install reported "completed
(exit code 0)" while the wrapper had actually returned 1 with
`CommandTimeoutError`. The handoff doc warned about this for builds; it applies
to every backgrounded pipeline. Check the command's own recorded exit status.

**A dead SSH tunnel hangs adb instead of failing it.** The forwarded
`127.0.0.1:5037` socket kept accepting connections after the far end died, so
`adb devices` blocked forever and `adb` never fell back to starting its own
server — it saw the port occupied and assumed a live server. Always run adb
under `timeout` here, and confirm the binary separately with `adb -P <free
port> devices`.

**`pkill -f <pattern>` matches your own shell.** `pkill -f "adb ... logcat"`
killed the shell running it, since the pattern appears in that shell's own
command line. Filter out `$$`.

**`adb push` reports fantasy throughput for compressible files.** Pushing 64 MB
of zeros measured "3195 MB/s" because adb compresses; it says nothing about the
link. Measure with real data, or watch `/proc/<pid>/io` during a real transfer.

**`adb reverse` opens the listening socket on the *device*.** It collided with
the device-local `nc` server on port 8111 and produced
`nc: bind: Address already in use`. Clear with `adb reverse --remove-all`.

**A GPU-process error can be a red herring.** `Validate adapter failed` /
`Failed to create Dawn context provider for Graphite` appears on every startup
here and concerns Skia Graphite, not WebGPU. WebGPU works fine on this device;
proving that with a baseline page cost one page swap and no rebuild, and it is
what localised the bug to our code.

**Test the platform before blaming your own code — and vice versa.** Running
`navigator.gpu.requestAdapter()` on the same page as the API under test turned
an ambiguous hang into a definite "our bug" in one 45-second cycle, with no
44-minute install.
