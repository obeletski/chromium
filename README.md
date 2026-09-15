# This branch: `floating-window`

> A personal fork of Chromium. **The features on this branch exist to learn how
> Chromium works — there is no intention to merge them upstream.** No CL, no
> Gerrit, no review. Upstream Chromium's own README follows [below](#chromium).

Branched from upstream `f6fd8f0cdc96a` (`chrome/VERSION` 153.0.8005.0), and not
rebased since; the `#L` anchors in the documents below are pinned to this
branch's line numbers.

## What is on it

One feature, grown in four steps, plus the write-ups that explain it.

**A floating window, on desktop.** A toolbar button between the extensions area
and the profile indicator toggles a floating `chrome://floating-window` bubble;
Esc or a second press closes it. The page is served from a C++ string rather
than a `build_webui()` target, and the bubble is a `views::BubbleDialogDelegate`
rather than a subclass of `BubbleDialogDelegateView`, which new code is not
allowed to subclass.

**The page lists the profile's open tabs, with each page's outline.** The `h1`
and `h2` headings of every tab, pulled out of an accessibility-tree snapshot in
the browser process and rendered as a table.

**Those headings are then summarised by a model.** A browser-process
`SimpleURLLoader` request, delivered into the page through a same-origin
`<iframe>` so the tab list does not wait for it. Off-the-record profiles are
refused with a `CHECK`.

**And the same idea on Android**, where none of the desktop answer transfers.
There is no floating window and no room for the table, but the Hub — the tab
switcher — is already the "what do I have open" surface. So the summary is a
`MessageService` card pinned above the tab grid, and a JNI bridge
(`chrome/browser/android/tab_outline/`) asks each tab's renderer for the same
headings. The model call on this side is not written yet.

## The documents

These are the point of the branch as much as the code is. They are written for
a reader who does not already know the part of the tree being touched, so they
explain the surrounding machinery — ownership, async boundaries, the defaults
that had to be overridden — and not only the diff.

| Document | What it covers |
|---|---|
| [`floating-window-implementation.md`](docs/floating_window/floating-window-implementation.md) | The toolbar button, the bubble and the WebUI host, file by file. |
| [`floating-window-alternatives.md`](docs/floating_window/floating-window-alternatives.md) | The designs that were rejected, and why. |
| [`floating-window-page-outlines.md`](docs/floating_window/floating-window-page-outlines.md) | Gathering headings from every tab: the snapshot API, the collector, the ordering hazards. |
| [`floating-window-tab-summary.md`](docs/floating_window/floating-window-tab-summary.md) | The model call: where it may happen, how the key is obtained, what leaves the machine, and what is measured rather than assumed. |
| [`tab-summary-android-ui-alternatives.md`](docs/floating_window/tab-summary-android-ui-alternatives.md) | Five Android surfaces weighed, then the implementation of the chosen one — a snippet from each of its ten files. |

Six orientation notes sit beside them in [`docs/notes/`](docs/notes/) — this
tree's [C++ idioms](docs/notes/chromium-cpp-idioms.md), its [design
patterns](docs/notes/chromium-design-patterns.md), its [process model and
IPC](docs/notes/chromium-linux-processes-and-ipc.md), its
[threading](docs/notes/chromium-threading.md), what is genuinely different on
[Android](docs/notes/chromium-android-architecture.md), and a survey of the
[AI/GenUI machinery](docs/notes/genui-chromium-directions.md) actually in the
tree. They describe *this* checkout rather than upstream Chromium in general,
and the GenUI note is explicit about which of its parts are verified against
source and which are proposals.

Some documents have a `.pdf` rendered next to them by
[`docs/floating_window/tools/render-pdf.sh`](docs/floating_window/tools/render-pdf.sh),
which runs the Markdown through `marked` and `mermaid` and then through this
checkout's own headless `chrome --print-to-pdf`.

## Other branches

`digitclassifier` adds an experimental `navigator.digitclassifier` Web API — a
two-layer MLP running as a WebGPU compute shader inside Blink — with its own
write-ups under `docs/digitclassifier/`. It does not stack on this branch;
both are cut from the same upstream commit. `digitclassifier-details` keeps that
feature's incremental history, including two WebGPU defects found on hardware
that the squashed branch no longer records.

---

# ![Logo](chrome/app/theme/chromium/product_logo_64.png) Chromium

Chromium is an open-source browser project that aims to build a safer, faster,
and more stable way for all users to experience the web.

The project's web site is https://www.chromium.org.

To check out the source code locally, don't use `git clone`! Instead,
follow [the instructions on how to get the code](docs/get_the_code.md).

Documentation in the source is rooted in [docs/README.md](docs/README.md).

Learn how to [Get Around the Chromium Source Code Directory
Structure](https://www.chromium.org/developers/how-tos/getting-around-the-chrome-source-code).

For historical reasons, there are some small top level directories. Now the
guidance is that new top level directories are for product (e.g. Chrome,
Android WebView, Ash). Even if these products have multiple executables, the
code should be in subdirectories of the product.

If you found a bug, please file it at https://crbug.com/new.
