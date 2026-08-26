// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_MODULES_DIGITCLASSIFIER_NAVIGATOR_DIGIT_CLASSIFIER_H_
#define THIRD_PARTY_BLINK_RENDERER_MODULES_DIGITCLASSIFIER_NAVIGATOR_DIGIT_CLASSIFIER_H_

#include "third_party/blink/renderer/core/execution_context/navigator_base.h"
#include "third_party/blink/renderer/modules/digitclassifier/digit_classifier.h"
#include "third_party/blink/renderer/platform/heap/member.h"
#include "third_party/blink/renderer/platform/supplementable.h"

namespace blink {

class NavigatorDigitClassifier final
    : public GarbageCollected<NavigatorDigitClassifier>,
      public Supplement<NavigatorBase> {
 public:
  static const char kSupplementName[];
  static DigitClassifier* digitclassifier(NavigatorBase& navigator);
  explicit NavigatorDigitClassifier(NavigatorBase& navigator);

  NavigatorDigitClassifier(const NavigatorDigitClassifier&) = delete;
  NavigatorDigitClassifier& operator=(const NavigatorDigitClassifier&) = delete;

  void Trace(blink::Visitor*) const override;

 private:
  Member<DigitClassifier> digit_classifier_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_MODULES_DIGITCLASSIFIER_NAVIGATOR_DIGIT_CLASSIFIER_H_
