# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Why this checkout exists

**Features here are written to learn how Chromium works. There is no intention
to merge them upstream.** No CL, no review, no Gerrit. That inverts one of the
usual trade-offs:

- **Comment for a reader who does not know this part of the tree yet.** Chromium
  house style keeps comments sparse because reviewers already know the
  surrounding code. That assumption does not hold here, so comment *above* the
  usual density.
- **Explain the neighbouring machinery too, not just the new lines.** When new
  code leans on an existing mechanism — an ownership model, a callback that
  arrives asynchronously, a default that had to be overridden, a base class that
  supplies behaviour for free — say how that mechanism works and where it lives,
  with file and line. The value is in what the diff *doesn't* show.
- Prefer explaining a trap over noting that one exists: which API looked right
  and silently would not have worked, which call ordering matters and why.
- Still not worth writing: comments that restate the line below them.
- Per-feature notes belong in `docs/<feature>/`, and are worth writing at length
  (see `docs/digitclassifier/`, `docs/floating_window/`). Anything that would be
  disproportionate for an upstream CL is probably right here.

Two conventions that upstream would enforce and we deliberately skip, because
both cost more than they are worth for code that will not ship: `IDS_` strings
in `generated_resources.grd` (new messages need translation screenshots that
presubmit blocks on) and full test coverage. Hardcode UI strings as `u"..."`
with a TODO, matching `ai_overlay_toolbar_button.cc`. Everything else — layering,
include hygiene, `git cl format`, presubmit-clean metrics XML — still applies:
the point is to learn how the real thing is built.

## This checkout

`~/chromium-desk` — `src/` plus a sibling `../notes/` directory of orientation
write-ups (see "Local notes" at the bottom). A second, android-only checkout
exists at `~/chromium`; do not confuse the two.

- **Both `android` and `linux` are synced.** `../.gclient` sets
  `target_os = ["android", "linux"]`, so host targets *do* build here.
- `depot_tools` is at `~/depot_tools` and already on `PATH` (`gn`, `autoninja`,
  `siso`, `git cl`, `gclient`).
- `chrome/VERSION` is 153.0.8005.0. Both feature branches sit on the upstream
  base `f6fd8f0cdc96a`.
- **This `CLAUDE.md` is tracked in git** — but see the caveat under **Feature
  branches** about which branch it exists on. `digit_classifier.tflite` and
  `docs/glic/` stay untracked on purpose; do not `git add` them.
- Builds are **local only** (`use_remoteexec=false` everywhere); there is no
  distributed compile, so a clean build of a large target is very slow. Always
  build the narrowest target that covers the change. `siso` is the backend.

### The four `out/` dirs

| Dir | Config | State |
|---|---|---|
| `out/Default` | **linux x64**, `is_debug=true`, `is_component_build=false` | re-gen'd for linux 2026-09-08; no `chrome` binary — effectively unbuilt for its current config |
| `out/Release` | **android arm64**, `is_debug=false`, `symbol_level=1`, `dcheck_always_on=false` | the android dir; `apks/ChromePublic.apk` is built |
| `out/Linux` | linux x64, debug, **component** build | has a built `chrome`; the cheapest dir for incremental host work |
| `out/de` | same defaults as `out/Linux` | gen'd only, nothing built; ignore unless asked |

Two traps here:

- **`out/Default` is no longer an android dir.** Its `args.gn` was switched to
  `target_os="linux"` *after* an android build, so the leftover
  `out/Default/apks/ChromePublic.apk` and
  `out/Default/android_clang_arm64_with_system_allocator/` are **stale**.
  Anything android goes to `out/Release` now.
- Use `out/Release` for anything you intend to **measure**. On a debug build a
  GPU compute path here produced different classification *results*, not merely
  slower ones, so timings and outputs from `out/Default` or `out/Linux` prove
  nothing about shipped behaviour.

## Build

```sh
autoninja --quiet -C out/Linux chrome          # host / linux
autoninja --quiet -C out/Release <target>      # android
```

Always pass `--quiet`.

Android device targets (`//docs/android_build_instructions.md`):
- `chrome_public_apk` — basic functionality, no `//clank` code
- `chrome_apk` — needs the internal `//clank` repo
- `trichrome_chrome_google_bundle` — closest to production; use for perf testing

Install / run via the generated wrapper, not `adb` directly:

```sh
out/Release/bin/chrome_public_apk install
out/Release/bin/chrome_public_apk launch
```

## Feature branches

Each lives on its own branch off upstream; they do not stack.

> **This file is committed per branch.** Because the branches do not stack,
> `CLAUDE.md` exists only on the branch it was committed to — checking out a
> branch that predates it will *delete it from the working tree*. If it goes
> missing, recover it with
> `git checkout <branch-that-has-it> -- CLAUDE.md`, and copy it forward
> (`git add` + commit) whenever a new feature branch is cut.

### `digitclassifier` — `navigator.digitclassifier`

The `digitclassifier` branch adds an experimental, non-standard Web API that
classifies a 28x28x3 NHWC `Float32Array` (2352 floats, `[0,1]`) as `"0"`..`"9"`.
Read `docs/digitclassifier/` **before** touching it — the session log there is
newer than the handoff and contradicts parts of it.

- `docs/digitclassifier/DigitClassifier-Session-2026-08-23.md` — most recent
  state; what is proven on hardware and what is not. Read this first.
- `DigitClassifier-Handoff.md` (design decisions), `-File-Inventory.md` (every
  file the branch touches), `WGSL-Compilation-Path.md` (WGSL to SPIR-V).

Shape of the implementation:

- Code is `third_party/blink/renderer/modules/digitclassifier/`. A two-layer MLP
  (2352 -> 128 ReLU -> 10) runs as a **WebGPU compute shader**; Dawn/WebGPU is
  the only usable GPU ML backend in this checkout (no TFLite GPU delegate, no
  QNN, no `chrome_ml_api`), and it forces both `getModel()` and `classify()` to
  be async.
- Weights ship as `resources/digit_classifier_weights.bin` through
  `blink_resources.grd` (`IDR_DIGIT_CLASSIFIER_WEIGHTS`, uncompressed). A
  renderer **cannot** open APK assets, whatever the comments there claim.
- `tools/` holds offline scripts: `reference_infer.py` is the CPU oracle every
  GPU answer is checked against, `extract_weights.py` regenerates the blob from
  `digit_classifier.tflite`, which is deliberately untracked.
- Gated on the `DigitClassifier` runtime-enabled feature (`status: "test"`), so
  it needs `--enable-blink-features=DigitClassifier`. Every interface is
  `[SecureContext]`.

A new Blink module stays invisible until **four** registration points know about
it: `modules/BUILD.gn` deps, `bindings/idl_in_modules.gni`,
`bindings/generated_in_modules.gni` (not derived from the previous list — edit
both), and `runtime_enabled_features.json5`.

### `floating-window` — a toolbar button opening a floating HTML window

A desktop toolbar button between the extensions area and the profile /
Incognito indicator that toggles a floating `chrome://floating-window` bubble;
Esc or a second press closes it. Read
`docs/floating_window/floating-window-implementation.md` for the walkthrough and
`-alternatives.md` for what was rejected and why.

Three reusable lessons from it:

- A new `chrome://` host needs **two** metrics registrations or
  `WebUIUrlHashesBrowserTest` fails: the `base::Hash` of the URL in
  `metadata/ui/enums.xml` (enum `WebUIUrlHashes`) and a `.host` variant in
  `metadata/page/histograms.xml` (variant `WebUIHost`).
- A static WebUI page does not need a `build_webui()` target: serve it from a
  C++ string via `WebUIDataSource::SetRequestFilter()`.
  `SetResourcePathToResponse()` looks equivalent but is only read on the
  `LocalResourceLoaderConfig` path, never by `StartDataRequest()`.
- `views::BubbleDialogDelegateView` cannot be subclassed by new code — private
  constructors behind a `friend` allowlist. Use `views::BubbleDialogDelegate` +
  `SetContentsView()`.

## Test

`tools/autotest.py` maps a **source filename** to its test target, builds it, and
runs it. Do **not** run `autoninja` first, and do not pass GN labels (anything
with a `:`).

Host tests run natively in `out/Linux` — no device, no emulator:

```sh
tools/autotest.py --quiet --run-all -C out/Linux base/strings/strcat_unittest.cc
tools/autotest.py -C out/Linux bit_cast_unittest.cc --gtest_filter=BitCastTest*  # single test
tools/autotest.py -C out/Linux --line 11 base/strings/strcat_unittest.cc          # test at a line
```

Android tests use `-C out/Release` and need a device or emulator; Java tests are
addressed by class:

```sh
tools/autotest.py -C out/Release UrlUtilitiesUnitTest
```

No physical device attached → use a prebuilt AVD (`//docs/android_emulator.md`):

```sh
tools/android/avd/avd.py list
# bin/run_<target> wrappers appear once the target is built
out/Release/bin/run_base_unittests --avd-config tools/android/avd/proto/android_35_google_apis_x64.textpb
tools/android/avd/avd.py start --avd-config <proto>   # standalone, for install/launch
tools/android/avd/avd.py stop                          # stop all
```

If an emulator is already running, `-d emulator-XXXX` is much faster than
`--avd-config`.

### The physical device (when one is attached)

`adb` here talks to the **laptop's** adb server through an SSH reverse tunnel,
so the usual recipes bend:

- Export the tunnel's port first; it changes when SSH sessions turn over, and a
  dead tunnel makes every `adb` command **hang forever instead of failing**:
  `export ANDROID_ADB_SERVER_PORT=<port>`. `adb` is not on `PATH` — use
  `third_party/android_sdk/public/platform-tools/adb`.
- `adb forward` / `adb reverse` bind on the **laptop**, not here, so a host-side
  HTTP server is unreachable from the device and `adb reverse` appears to
  succeed while doing nothing. For a `[SecureContext]` API, serve the page from
  the device's own `127.0.0.1` — see
  `.../digitclassifier/tools/serve_smoke_test.sh`; `file://` is not secure.
- `out/Release/bin/chrome_public_apk install` **cannot work** over the tunnel:
  devil caps adb at 300 s and the 715 MB APK takes far longer. Use plain
  `adb -s <serial> install -r -d out/Release/apks/ChromePublic.apk`, then check
  `dumpsys package org.chromium.chrome | grep loadingProgress` — an incremental
  install streams pages lazily and the app depends on the tunnel until 100%.
- `launch` works, but `am start` with the flags written to the command-line file
  is the more predictable path. `console.log()` does not reach logcat; render
  results into the page and `screencap` it.

## Git workflow

Nothing here is uploaded (see **Why this checkout exists**), so the local rules
are what matter; the Gerrit ones are recorded for when a change *is* meant to
go out. Chromium uses Gerrit via `git cl`, not GitHub PRs.

- Each feature gets its own branch, cut from the upstream commit below any other
  in-flight work — not stacked on another feature's branch.
- Never commit submodules: `git -c diff.ignoreSubmodules=all commit -a`.
- `git cl format` is still worth running before every commit; so is any
  presubmit script that guards a file you touched (e.g.
  `tools/metrics/histograms/pretty_print.py --presubmit <file>`).
- *If uploading:* `git cl upload --title="<CL title>"` — `--title` is
  **mandatory** — and `git cl presubmit -u --force` first.
- Never `git commit --amend`; use `git commit --fixup=<hash>` and let the user
  run `git rebase --autosquash`.
- After rebasing onto `main`, you **must** run `gclient sync`.
- Fix only presubmit warnings caused by your own lines; leave pre-existing ones
  alone.
- Commit messages: imperative mood, no "This CL", refer to functions as
  `FunctionName()`, wrap at 72 chars.

## Architecture: the layering that matters

Dependencies flow one way. Putting code in the wrong layer is the most common
review-blocking mistake.

- `base/` — platform abstractions, containers, threading, callbacks.
- `content/` — the multi-process sandboxed web platform (HTML5, GPU). Code
  belongs here **only** if it is a spec'd, chromestatus-tracked web platform
  feature. See `content/README.md`.
- `components/` — features shared by more than one embedder (`//chrome`,
  `//ios/chrome`, `//android_webview`) or between Blink and the browser process.
  `//ios` does not depend on `//chrome`, which is why so much lands here.
- `chrome/` — the opinionated product layer (desktop + Android browser UI).
- `third_party/blink/renderer/` — the rendering engine, and a **hard boundary**:
  inside it use WTF types (`blink::Vector`, `blink::String`, from
  `third_party/blink/renderer/platform/wtf/`) and Oilpan GC (`Member<>`,
  `WeakMember<>`, `Persistent<>`). Do **not** use STL containers or most `base/`
  equivalents there. See `styleguide/c++/blink-c++.md`.

Cross-cutting mechanisms:
- **IPC** between processes is Mojo. Start from the `.mojom` interface definition
  to understand a browser↔renderer interaction.
- **Async / threading** goes through `base::TaskRunner` +
  `base::BindOnce`/`BindRepeating`. See `docs/threading_and_tasks.md` and
  `docs/callback.md`.

## JNI (this is an Android build, so most features cross the boundary)

Codegen lives in `//third_party/jni_zero`.

- Java `@CalledByNative` methods appear in C++ with a `Java_` prefix.
- Java `@NativeMethods` interfaces are calls **into** C++, appearing with a
  `JNI_` prefix. A first param `long nativeBarImpl` means the implementation is
  `BarImpl::Foo` in C++.
- To find the Java side: `#include "{JavaClass}_jni.h"` → search for
  `{JavaClass}.java`. To find the C++ side: grep for `{JavaClass}_jni.h`.
- Always change the `.java` and the `.cc`/`.h` together.

## Working conventions in this repo

- **Read before editing.** Never infer a function's behavior from its name;
  read the implementation and at least one call site.
- **Don't create new test files** if a test file already exists for the
  component — extend it.
- **Include hygiene / IWYU:** include the header defining every symbol you use in
  a `.cc` (no transitive includes); remove includes you orphan; keep conditional
  `#if` includes in their own section *below* the unconditional ones.
- **Stay on task:** don't fix unrelated TODOs or code health issues.
- Comments explain *why*, not *what* — but see **Why this checkout exists**
  above for how densely to write them here. Upstream's "sparingly" does not
  apply.
- Editing a prompt file under `agents/prompts/` with a `.tmpl.md` counterpart
  means editing **both** (mirroring `process_prompts.py` output).

## Repo resources for agents

- `agents/ai_policy.md` — **binding project policy.** You must be able to explain
  any code you send for review; flag low-confidence AI-assisted areas in the CL
  description. "A human reply must get a human reply" — the human operator, not
  the agent, answers reviewer feedback.
- `agents/prompts/knowledge_base.md` — routing table from task ("add a UMA
  metric", "header not found", "add a pref") to the canonical doc. Consult it
  before answering from general knowledge; this codebase is too specific for that.
- `agents/prompts/templates/` — per-area notes (`android.md` applies to this
  checkout; also `rust.md`, `webui_lit.md`, `code_coverage.md`, …).
- `agents/skills/<name>/SKILL.md` — 49 Chromium-specific skills (histograms,
  disable-test, fuzzing, nullaway, feature-flag-removal, jni-type-conversion,
  network-traffic-annotations, …). `.claude/skills/` is gitignored; symlink or
  copy a `SKILL.md` there to install one.
- Style guides live in `styleguide/` (`c++/c++.md`, `c++/blink-c++.md`,
  `java/java.md`); GN style is `docs/imported/gn/style_guide.md`.

## Debugging build failures

- *Header not found:* check the target's `BUILD.gn` `deps`, then the include
  path, then `gn gen <out dir>`, then
  `gn desc <out dir> //failing:target deps` and `gn check <out dir> //failing:target`.
- *Undefined symbol:* the providing target is probably missing from `deps`; also
  confirm `is_component_build` in `args.gn` matches expectations.
- *Visibility error:* add the depending target to the dependency's `visibility`.
- Never make speculative fixes for compile errors — read the defining file first,
  and check whether the same error already appeared earlier in the session.

## Local notes (`../notes/`)

Written against this checkout; not upstream Chromium docs. The Markdown is the
source — the two `.pdf`s are renders (`chromium-android-offline.pdf` renders
`chromium-android-architecture.md`, despite the name).

- `chromium-android-architecture.md` — reading path and mental model: the
  layering vs. process axes, where Blink fits, what is genuinely different on
  Android (helper processes are Android Services, not `fork`/`exec`).
- `digitclassifier-walkthrough.md` — file-by-file trace of the
  `digitclassifier` branch, closing with a list of the places where in-tree
  docs and comments are wrong.
- `genui-chromium-directions.md` — what AI/GenUI machinery is actually in the
  tree (Glic, Skills, Actor, on-device model plumbing) and where it could go.
  Part I is verified against source; Parts II–V are explicitly proposals.
