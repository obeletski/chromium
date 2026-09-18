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
#include "chrome/browser/android/tab_outline/tab_outline.h"
#include "content/public/browser/web_contents.h"
#include "ui/accessibility/ax_tree_update.h"

// Must come after the other includes: the generated header depends on the JNI
// types they declare.
#include "chrome/android/chrome_jni_headers/TabOutlineBridge_jni.h"

using base::android::AttachCurrentThread;
using base::android::JavaRef;
using base::android::ScopedJavaGlobalRef;

namespace {

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
      base::android::ToJavaArrayOfStrings(
          env, tab_outline::ExtractOutline(update, /*indent_level_2=*/true)));
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
      base::BindOnce(&OnSnapshot, std::move(callback)),
      tab_outline::kOutlineAXMode, tab_outline::kMaxAxNodesPerTab,
      tab_outline::kSnapshotTimeout,
      // Same-origin pruning: a cross-origin iframe's headings are not part of
      // this page's outline, and reaching into them would widen what is read
      // out of arbitrary sites for no benefit here.
      content::WebContents::AXTreeSnapshotPolicy::kSameOriginDirectDescendants);
}

// Registers the entry point above with jni_zero. Omitting this compiles cleanly
// and then fails at run time with an UnsatisfiedLinkError, which is why the
// generated header plants a -Wunused-function tripwire for it.
DEFINE_JNI(TabOutlineBridge)
