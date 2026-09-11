// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_FLOATING_WINDOW_FLOATING_WINDOW_SUMMARIZER_H_
#define CHROME_BROWSER_UI_WEBUI_FLOATING_WINDOW_FLOATING_WINDOW_SUMMARIZER_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"

class Profile;

namespace network {
class SimpleURLLoader;
}  // namespace network

namespace floating_window {

// One row's worth of what the summarizer is allowed to see. Deliberately not
// `TabEntry` from floating_window_ui.cc: that type carries rendering state
// (`snapshot_returned`, window grouping) the model has no use for, and keeping
// the input narrow is what makes it obvious at a glance which bytes leave the
// machine. This is the "domain lens" idea applied to an egress boundary.
struct SummaryInput {
  SummaryInput();
  SummaryInput(const SummaryInput&);
  SummaryInput(SummaryInput&&);
  SummaryInput& operator=(const SummaryInput&);
  SummaryInput& operator=(SummaryInput&&);
  ~SummaryInput();

  std::string title;
  // Level 1 and 2 headings, in document order, already truncated and collapsed
  // by ExtractOutline().
  std::vector<std::string> headings;
};

// Turns the open tabs' titles and headings into a prose summary by asking a
// hosted Gemini model.
//
// Why this is its own class rather than code inside floating_window_ui.cc: it
// is the seam that keeps the model swappable. Everything upstream of it deals
// only in `SummaryCallback`, so replacing this implementation with the
// on-device path (OptimizationGuideKeyedService::StartSession with
// mojom::OnDeviceFeature::kSummarize) would be a change here and nowhere else.
// It is also the only part of the feature that touches the network, so the
// traffic annotation and the key handling live in exactly one auditable place.
//
// Threading: browser process, UI thread, start to finish. `Summarize()` returns
// immediately; the callback runs later on the same sequence.
//
// Lifetime: the caller must keep the instance alive until the callback runs.
// Destroying it cancels the request -- `SimpleURLLoader`'s destructor tears
// down the load -- and the callback is then simply never invoked. That is the
// intended cancellation mechanism, not a leak.
class FloatingWindowSummarizer {
 public:
  // Runs exactly once: with the summary text on success, or with `std::nullopt`
  // on any failure -- no key, no network, a malformed reply, an API error.
  //
  // The optional is load-bearing rather than decorative. An earlier version
  // signalled failure with an empty string, which collapsed two different
  // outcomes into one value and made a failed request render as though the
  // model had succeeded and found nothing to say. That is the same defect this
  // feature already has on the outline side, where a renderer dying mid-flight
  // is indistinguishable from a page with no headings. Encoding the difference
  // in the type is what stops it recurring.
  using SummaryCallback = base::OnceCallback<void(std::optional<std::string>)>;

  FloatingWindowSummarizer();
  FloatingWindowSummarizer(const FloatingWindowSummarizer&) = delete;
  FloatingWindowSummarizer& operator=(const FloatingWindowSummarizer&) = delete;
  ~FloatingWindowSummarizer();

  // True if the feature is enabled and an API key is configured. Callers should
  // check this before constructing one, and skip the summary entirely when it
  // is false -- an absent key must never become an unauthenticated request.
  static bool IsAvailable();

  // Starts the request. `profile` supplies the URLLoaderFactory and must not be
  // off-the-record: callers are responsible for that check, because a summary
  // of Incognito tabs must not leave the machine and this class cannot make
  // that policy decision on the caller's behalf.
  void Summarize(Profile* profile,
                 const std::vector<SummaryInput>& tabs,
                 SummaryCallback callback);

 private:
  void OnResponse(SummaryCallback callback,
                  std::optional<std::string> response_body);

  // Held as a member because destroying a SimpleURLLoader cancels its request;
  // it must outlive the round trip. See the lifetime note on the class.
  std::unique_ptr<network::SimpleURLLoader> url_loader_;

  base::WeakPtrFactory<FloatingWindowSummarizer> weak_factory_{this};
};

}  // namespace floating_window

#endif  // CHROME_BROWSER_UI_WEBUI_FLOATING_WINDOW_FLOATING_WINDOW_SUMMARIZER_H_
