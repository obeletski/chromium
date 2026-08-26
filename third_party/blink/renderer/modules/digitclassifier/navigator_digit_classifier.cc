// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/digitclassifier/navigator_digit_classifier.h"

namespace blink {

const char NavigatorDigitClassifier::kSupplementName[] =
    "NavigatorDigitClassifier";

NavigatorDigitClassifier::NavigatorDigitClassifier(NavigatorBase& navigator)
    : Supplement<NavigatorBase>(navigator),
      digit_classifier_(MakeGarbageCollected<DigitClassifier>(
          navigator.GetExecutionContext())) {}

DigitClassifier* NavigatorDigitClassifier::digitclassifier(
    NavigatorBase& navigator) {
  NavigatorDigitClassifier* supplement =
      Supplement<NavigatorBase>::From<NavigatorDigitClassifier>(navigator);
  if (!supplement) {
    supplement = MakeGarbageCollected<NavigatorDigitClassifier>(navigator);
    ProvideTo(navigator, supplement);
  }
  return supplement->digit_classifier_.Get();
}

void NavigatorDigitClassifier::Trace(Visitor* visitor) const {
  visitor->Trace(digit_classifier_);
  Supplement<NavigatorBase>::Trace(visitor);
}

}  // namespace blink
