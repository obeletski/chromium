# ![Logo](chrome/app/theme/chromium/product_logo_64.png) Chromium

> **This branch (`ai-side-panel-desktop`) is a fork of Chromium carrying one
> experimental change: a tab-scoped "AI side panel" for desktop Chrome.**

## AI side panel — what this branch adds

A new side panel that opens beside the web contents, is **scoped to a single
tab**, and is launched from **⋮ → More tools → AI side panel**. Each tab owns
its own panel instance, so switching tabs swaps the contents and switching back
restores them.

| | |
|---|---|
| Feature flag | `features::kAiSidePanel`, disabled by default — run `chrome --enable-features=AiSidePanel` |
| Platforms | Desktop only: Linux, macOS, Windows, ChromeOS. Not Android, not iOS |
| Profiles | Regular profiles only — not Incognito, not Guest |
| Status | Phase 1: the plumbing is complete and tested; the panel's content is a placeholder showing the active tab's title and URL. No model is wired up yet |
| Size | 26 files, ~1100 lines, one commit on top of upstream `main` |

New code lives in
[`chrome/browser/ui/views/side_panel/ai/`](chrome/browser/ui/views/side_panel/ai):
`AiSidePanelCoordinator` (one per tab, owned by `TabFeatures`, registers the
panel's entry into the tab's `SidePanelRegistry` and builds its content lazily)
and `AiSidePanelView` (the Views-native content). The rest of the change is
wiring: a feature flag, a command id, an action id, a side panel entry id, an
app-menu item, a pinnable toolbar action, metrics metadata, and six browser
tests.

### Documentation

| Document | What it covers |
|---|---|
| [`docs-ob/ai_side_panel_design.md`](docs-ob/ai_side_panel_design.md) | Design: an inventory of every side panel in Chromium today, a reference for each participant in the side panel system, the creation flow, the decisions taken, and screenshots |
| [`docs-ob/ai_side_panel_implementation.md`](docs-ob/ai_side_panel_implementation.md) | Architecture and implementation: every touched file with what it contains and what changed, ownership and interaction diagrams, annotated code excerpts, the wiring checklist for adding a side panel, build/test instructions, and a glossary |

### Building and running

```sh
autoninja --quiet -C out/Desktop chrome
out/Desktop/chrome --enable-features=AiSidePanel

tools/autotest.py --quiet --run-all -C out/Desktop \
    chrome/browser/ui/views/side_panel/ai/ai_side_panel_coordinator_browsertest.cc
```

---

## Upstream Chromium

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
