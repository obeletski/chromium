// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ANDROID_TAB_OUTLINE_TAB_OUTLINE_H_
#define CHROME_BROWSER_ANDROID_TAB_OUTLINE_TAB_OUTLINE_H_

#include <stddef.h>

#include <string>
#include <vector>

#include "base/time/time.h"
#include "ui/accessibility/ax_mode.h"

namespace ui {
struct AXTreeUpdate;
}  // namespace ui

namespace tab_outline {

// Asking a renderer for a page's h1/h2 headings, shared by the two Android
// callers: TabOutlineBridge, which shows the headings in the tab summary card,
// and TabSummaryBridge, which feeds them to a model.
//
// This exists as its own header because the second caller arrived. Everything
// here was file-local in tab_outline_bridge.cc until then, which is the right
// default -- a shared header for one caller is a wider surface for nothing.
//
// Threading: nothing here touches threads. ExtractOutline() is a pure function
// over a snapshot the caller already has.

// The desktop side of this feature uses the same three numbers; see
// chrome/browser/ui/webui/floating_window/floating_window_ui.cc, which is where
// they were tuned. Kept in sync by hand -- the two implementations do not share
// code yet, and that is the main argument for eventually lifting the extraction
// into //components.
inline constexpr size_t kMaxHeadingBytes = 300;
inline constexpr size_t kMaxAxNodesPerTab = 20000;
inline constexpr base::TimeDelta kSnapshotTimeout = base::Milliseconds(1200);

// kSnapshotTimeout is handed to the renderer and truncates the tree it
// serializes. kOverallDeadline is enforced on this side instead, and covers
// what the renderer-side timeout cannot: a renderer that never answers at all,
// so that neither the reply nor the callback's destruction ever arrives. Only
// the multi-tab caller needs it -- a single outline request has nothing to wait
// on but itself.
inline constexpr base::TimeDelta kOverallDeadline = base::Milliseconds(2000);

// kWebContents gives the roles and names; kExtendedProperties carries
// kHierarchicalLevel, which is the only way to tell an h1 from an h2 -- the
// role is the same `kHeading` for both. Deliberately *not* ui::kAXModeComplete,
// which adds kInlineTextBoxes: that makes the renderer lay out and serialize
// per-word text boxes for the whole document, which is real work per tab and
// nothing here reads them.
inline constexpr ui::AXMode kOutlineAXMode(ui::AXMode::kWebContents |
                                           ui::AXMode::kExtendedProperties);

// Flattens one page's h1/h2 headings, in document order.
//
// `indent_level_2` is what separates the two callers. The card renders into a
// single TextView and has nowhere to hang structure, so it wants the level
// baked into the string as a leading indent; the prompt builder indents every
// heading itself and wants them plain. Mirrors ExtractOutline() in
// floating_window_ui.cc, which keeps the level as a struct field instead
// because the WebUI page renders the two levels with different indentation.
std::vector<std::string> ExtractOutline(const ui::AXTreeUpdate& update,
                                        bool indent_level_2);

}  // namespace tab_outline

#endif  // CHROME_BROWSER_ANDROID_TAB_OUTLINE_TAB_OUTLINE_H_
