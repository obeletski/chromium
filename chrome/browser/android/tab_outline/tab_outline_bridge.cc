// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string>
#include <vector>

#include "base/android/callback_android.h"
#include "base/android/jni_android.h"
#include "base/android/jni_array.h"
#include "base/android/scoped_java_ref.h"
#include "base/functional/bind.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "content/public/browser/web_contents.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/accessibility/ax_mode.h"
#include "ui/accessibility/ax_node_data.h"
#include "ui/accessibility/ax_role_properties.h"
#include "ui/accessibility/ax_tree_update.h"

// Must come after the other includes: the generated header depends on the JNI
// types they declare.
#include "chrome/android/chrome_jni_headers/TabOutlineBridge_jni.h"

using base::android::AttachCurrentThread;
using base::android::JavaRef;
using base::android::ScopedJavaGlobalRef;

namespace {

// The desktop side of this feature uses the same three numbers; see
// chrome/browser/ui/webui/floating_window/floating_window_ui.cc, which is where
// they were tuned. Kept in sync by hand -- the two implementations do not share
// code yet, and that is the main argument for eventually lifting the extraction
// into //components.
constexpr size_t kMaxHeadingBytes = 300;
constexpr size_t kMaxAxNodesPerTab = 20000;
constexpr base::TimeDelta kSnapshotTimeout = base::Milliseconds(1200);

// kWebContents gives the roles and names; kExtendedProperties carries
// kHierarchicalLevel, which is the only way to tell an h1 from an h2 -- the
// role is the same `kHeading` for both. Deliberately *not* ui::kAXModeComplete,
// which adds kInlineTextBoxes: that makes the renderer lay out and serialize
// per-word text boxes for the whole document, which is real work per tab and
// nothing here reads them.
constexpr ui::AXMode kOutlineAXMode(ui::AXMode::kWebContents |
                                    ui::AXMode::kExtendedProperties);

// Flattens one page's h1/h2 headings, in document order, into display lines.
//
// This mirrors ExtractOutline() in floating_window_ui.cc. The difference is the
// return type: the desktop version keeps the level as a separate field because
// it renders the two levels with different indentation, whereas here the level
// is baked into the string as a leading indent, because the card is a single
// TextView and has no structure to hang an indent on.
std::vector<std::string> ExtractOutline(const ui::AXTreeUpdate& update) {
  std::vector<std::string> lines;
  for (const ui::AXNodeData& node : update.nodes) {
    if (!ui::IsHeading(node.role)) {
      continue;
    }
    const int level =
        node.GetIntAttribute(ax::mojom::IntAttribute::kHierarchicalLevel);
    if (level != 1 && level != 2) {
      continue;
    }

    // A heading's accessible name is its computed text content, which can carry
    // the source's line breaks and indentation. Collapse it, or a heading
    // wrapped across several lines in the markup arrives as several lines here
    // and breaks the one-heading-per-line layout.
    std::string text = base::CollapseWhitespaceASCII(
        node.GetStringAttribute(ax::mojom::StringAttribute::kName),
        /*trim_sequences_with_line_breaks=*/true);
    if (text.empty()) {
      continue;
    }

    // A page is free to have a pathologically long heading, and all of it would
    // otherwise cross the JNI boundary. TruncateUTF8ToByteSize() cuts on a
    // character boundary rather than mid-sequence, which substr() would not.
    if (text.size() > kMaxHeadingBytes) {
      text = std::string(base::TruncateUTF8ToByteSize(text, kMaxHeadingBytes));
    }

    lines.push_back(level == 2 ? "    " + text : text);
  }
  return lines;
}

// Runs on the UI thread when the renderer answers, or when the snapshot times
// out -- in which case `update` is simply empty and the tab contributes no
// lines. There is no separate failure signal, and the Java side cannot tell
// "page with no headings" from "renderer never answered". That is the same
// ambiguity the desktop implementation has, recorded in its limitations list.
void OnSnapshot(ScopedJavaGlobalRef<jobject> callback,
                ui::AXTreeUpdate& update) {
  JNIEnv* env = AttachCurrentThread();
  base::android::RunObjectCallbackAndroid(
      callback,
      base::android::ToJavaArrayOfStrings(env, ExtractOutline(update)));
}

}  // namespace

// Asks one tab's renderer for its h1/h2 headings.
//
// `jweb_contents` must be live: Java is responsible for the null check, because
// on Android a backgrounded tab frequently has no WebContents at all and that
// is an ordinary state rather than an error worth crossing JNI to discover.
static void JNI_TabOutlineBridge_RequestOutline(
    JNIEnv* env,
    const JavaRef<jobject>& jweb_contents,
    const JavaRef<jobject>& jcallback) {
  content::WebContents* web_contents =
      content::WebContents::FromJavaWebContents(jweb_contents);
  ScopedJavaGlobalRef<jobject> callback(jcallback);
  if (!web_contents) {
    // Lost between the Java null check and here. Answer with nothing rather
    // than dropping the callback, or the Java side waits forever for a reply
    // that is never coming.
    base::android::RunObjectCallbackAndroid(
        callback, base::android::ToJavaArrayOfStrings(
                      env, std::vector<std::string>()));
    return;
  }

  web_contents->RequestAXTreeSnapshot(
      base::BindOnce(&OnSnapshot, std::move(callback)), kOutlineAXMode,
      kMaxAxNodesPerTab, kSnapshotTimeout,
      // Same-origin pruning: a cross-origin iframe's headings are not part of
      // this page's outline, and reaching into them would widen what is read
      // out of arbitrary sites for no benefit here.
      content::WebContents::AXTreeSnapshotPolicy::kSameOriginDirectDescendants);
}

// Registers the entry point above with jni_zero. Omitting this compiles cleanly
// and then fails at run time with an UnsatisfiedLinkError, which is why the
// generated header plants a -Wunused-function tripwire for it.
DEFINE_JNI(TabOutlineBridge)
