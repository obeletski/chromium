// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stddef.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/android/callback_android.h"
#include "base/android/jni_android.h"
#include "base/android/jni_string.h"
#include "base/android/scoped_java_ref.h"
#include "base/check.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner_helpers.h"
#include "base/task/sequenced_task_runner.h"
#include "base/timer/timer.h"
#include "chrome/browser/android/tab_outline/tab_outline.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/webui/floating_window/floating_window_summarizer.h"
#include "content/public/browser/web_contents.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "ui/accessibility/ax_tree_update.h"

// Must come after the other includes: the generated header depends on the JNI
// types they declare.
#include "chrome/android/chrome_jni_headers/TabSummaryBridge_jni.h"

using base::android::JavaRef;
using base::android::ScopedJavaGlobalRef;

namespace {

// The prompt is capped again inside the summarizer (kMaxTabsInPrompt there).
// This cap is about the *snapshots*: each one wakes a renderer, and a profile
// with two hundred tabs would spend the whole deadline on tabs whose headings
// the prompt then discards. Kept at the summarizer's number so the two cannot
// disagree about which tabs were considered.
constexpr size_t kMaxTabsToSnapshot = 40;

// One in-flight summary request, from "Java handed us a tab list" to "the model
// answered".
//
// This is the Android counterpart of OutlineCollector in floating_window_ui.cc,
// and it is one class rather than that file's collector-plus-registry because
// there is no page to serve and no correlation token to keep: the reply goes to
// a single Java callback that was handed in at the start.
//
// Lifetime is the interesting part, and it is unusual enough to state plainly.
// The object is **self-owned**: JNI_TabSummaryBridge_CreateRequest() news one
// and returns the pointer to Java as an opaque handle, and the object destroys
// itself once its callback has run. Java never frees it, and holds the handle
// only across the three synchronous calls that build the request. There is no
// path on which the handle outlives Start().
//
// Threading: browser process, UI thread, start to finish -- every snapshot
// reply, the deadline timer and the summarizer's callback all land there.
class TabSummaryRequest {
 public:
  TabSummaryRequest(Profile* profile, ScopedJavaGlobalRef<jobject> callback)
      : profile_(profile), callback_(std::move(callback)) {}

  TabSummaryRequest(const TabSummaryRequest&) = delete;
  TabSummaryRequest& operator=(const TabSummaryRequest&) = delete;

  // Records one tab and, if it has a live renderer, asks for its headings.
  //
  // The snapshot is issued here rather than in Start() so that `web_contents`
  // does not have to be stored: Java calls AddTab() and Start() in one
  // uninterrupted loop, but a WebContents held across a suspension point would
  // be a dangling pointer waiting to happen, and there is no reason to hold it.
  void AddTab(std::string title, content::WebContents* web_contents) {
    if (tabs_.size() >= kMaxTabsToSnapshot) {
      return;
    }
    const size_t index = tabs_.size();
    floating_window::SummaryInput input;
    input.title = std::move(title);
    tabs_.push_back(std::move(input));

    if (!web_contents) {
      // Ordinary on Android: a backgrounded tab is frequently discarded and a
      // restored tab has never had a renderer this session. The tab still goes
      // into the prompt, with its title and no headings -- which is honest, and
      // is most of what a title-only tab would have contributed anyway.
      return;
    }

    ++pending_;

    // WrapCallbackWithDefaultInvokeIfNotRun covers the case where the reply
    // callback is *destroyed* without running -- a renderer going away while
    // the request is in flight. Without it that tab never resolves and the
    // request waits out kOverallDeadline for no reason. It does not cover a
    // renderer that stays alive and never answers; the deadline does.
    ui::AXTreeUpdate on_failure;
    web_contents->RequestAXTreeSnapshot(
        mojo::WrapCallbackWithDefaultInvokeIfNotRun(
            base::BindOnce(&TabSummaryRequest::OnSnapshot,
                           weak_factory_.GetWeakPtr(), index),
            base::OwnedRef(std::move(on_failure))),
        tab_outline::kOutlineAXMode, tab_outline::kMaxAxNodesPerTab,
        tab_outline::kSnapshotTimeout,
        // Same-origin pruning: a cross-origin iframe's headings are not part of
        // this page's outline, and reaching into them would widen what is read
        // out of arbitrary sites for no benefit here.
        content::WebContents::AXTreeSnapshotPolicy::
            kSameOriginDirectDescendants);
  }

  // Closes the request to further tabs and lets it complete.
  void Start() {
    deadline_.Start(FROM_HERE, tab_outline::kOverallDeadline,
                    base::BindOnce(&TabSummaryRequest::Summarize,
                                   weak_factory_.GetWeakPtr()));
    // Drops the count AddTab() could not drop. pending_ starts at one so that a
    // snapshot completing inline -- or a tab list in which no tab has a
    // renderer at all -- cannot summarize a half-built list before Java has
    // finished handing the tabs over.
    ResolveOne();
  }

 private:
  // DeleteSoon() destroys through this helper, so it needs access to the
  // destructor; keeping the destructor private is what stops anything else from
  // deleting a self-owned object out from under its own callbacks.
  friend class base::DeleteHelper<TabSummaryRequest>;

  ~TabSummaryRequest() = default;

  void OnSnapshot(size_t index, ui::AXTreeUpdate& update) {
    // An empty update is what both failure paths produce -- the renderer-side
    // timeout and the wrapper above -- so it needs no special case: the tab
    // simply contributes its title and no headings.
    tabs_[index].headings =
        tab_outline::ExtractOutline(update, /*indent_level_2=*/false);
    ResolveOne();
  }

  void ResolveOne() {
    CHECK_GT(pending_, 0u);
    if (--pending_ == 0) {
      Summarize();
    }
  }

  void Summarize() {
    // Both the deadline and the last reply lead here, and on a slow run they
    // can both fire. Whichever arrives first wins; the other must do nothing.
    if (summarizing_) {
      return;
    }
    summarizing_ = true;
    deadline_.Stop();

    if (!floating_window::FloatingWindowSummarizer::IsAvailable()) {
      // Java asked IsAvailable() before building the request, so reaching this
      // means the flag or the key changed underneath it. Answer rather than
      // dropping the callback.
      Finish(std::nullopt);
      return;
    }

    summarizer_.Summarize(profile_, tabs_,
                          base::BindOnce(&TabSummaryRequest::OnSummary,
                                         weak_factory_.GetWeakPtr()));
  }

  void OnSummary(std::optional<std::string> summary) { Finish(summary); }

  void Finish(std::optional<std::string> summary) {
    base::android::RunOptionalStringCallbackAndroid(
        callback_, base::optional_ref<const std::string>(summary));

    // Not `delete this`. Finish() is reached from inside the summarizer's own
    // callback, and the summarizer is a member of this object -- deleting here
    // would free it while one of its methods is still on the stack. Deferring
    // by one task lets that frame unwind first. The weak factory means the
    // deadline timer and any straggling snapshot reply are already
    // disarmed by the time this runs.
    base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(FROM_HERE, this);
  }

  // Raw, and held across the async gap. The regular profile outlives the tab
  // switcher on Android, and the desktop side holds the same pointer across the
  // same window for the same reason (see the base::Unretained(profile) comment
  // in floating_window_ui.cc). It is the weakest assumption in this file.
  raw_ptr<Profile> profile_;
  ScopedJavaGlobalRef<jobject> callback_;

  std::vector<floating_window::SummaryInput> tabs_;

  // Outstanding snapshot replies, plus the one held by Java's still-running
  // AddTab() loop. See Start().
  size_t pending_ = 1;
  bool summarizing_ = false;

  base::OneShotTimer deadline_;
  floating_window::FloatingWindowSummarizer summarizer_;

  base::WeakPtrFactory<TabSummaryRequest> weak_factory_{this};
};

}  // namespace

// True when the feature flag is on and an API key is configured. Java checks
// this before building a request, so that a build with no key never renders a
// "Summarising..." state it cannot finish.
static jboolean JNI_TabSummaryBridge_IsAvailable(JNIEnv* env) {
  return floating_window::FloatingWindowSummarizer::IsAvailable();
}

// Opens a request and returns it to Java as an opaque handle.
//
// `jprofile` must be the regular profile: Summarize() CHECKs that it is not
// off-the-record, because a summary of Incognito tabs must never reach a remote
// endpoint. The card that calls this is scoped to the regular tab switcher pane
// (MessageCardScope.REGULAR), so the CHECK documents an invariant rather than
// guarding a reachable path.
static jlong JNI_TabSummaryBridge_CreateRequest(
    JNIEnv* env,
    const JavaRef<jobject>& jprofile,
    const JavaRef<jobject>& jcallback) {
  Profile* profile = Profile::FromJavaObject(jprofile);
  CHECK(profile);
  return reinterpret_cast<jlong>(
      new TabSummaryRequest(profile, ScopedJavaGlobalRef<jobject>(jcallback)));
}

// Adds one tab. `jweb_contents` may be null -- see AddTab().
//
// The handle parameter is `requestHandle`, not `nativeRequest`. A jlong whose
// Java name begins with `native` is jni_zero's marker for "this is a pointer to
// a C++ object": it strips the prefix, treats the remainder as a class name and
// generates `Request* _ptr = reinterpret_cast<Request*>(...)` plus a call to a
// *member* function. That is the right convention for a long-lived native
// counterpart of a Java object, and the wrong one here -- these three entry
// points are shims around a self-owned request, not methods on a peer. Naming
// it `nativeRequest` fails to compile with "unknown type name 'Request'", which
// is a confusing way to be told about a naming convention.
static void JNI_TabSummaryBridge_AddTab(
    JNIEnv* env,
    jlong request_handle,
    const std::string& title,
    const JavaRef<jobject>& jweb_contents) {
  reinterpret_cast<TabSummaryRequest*>(request_handle)
      ->AddTab(title,
               content::WebContents::FromJavaWebContents(jweb_contents));
}

// Closes the request. The handle is invalid from here on: the object completes
// on its own and then deletes itself.
static void JNI_TabSummaryBridge_Start(JNIEnv* env, jlong request_handle) {
  reinterpret_cast<TabSummaryRequest*>(request_handle)->Start();
}

DEFINE_JNI(TabSummaryBridge)
