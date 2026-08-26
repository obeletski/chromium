// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_MODULES_DIGITCLASSIFIER_DIGIT_CLASSIFIER_WEIGHTS_H_
#define THIRD_PARTY_BLINK_RENDERER_MODULES_DIGITCLASSIFIER_DIGIT_CLASSIFIER_WEIGHTS_H_

#include <cstdint>
#include <optional>

#include "base/containers/span.h"
#include "third_party/blink/renderer/modules/modules_export.h"
#include "third_party/blink/renderer/platform/wtf/vector.h"

namespace blink {

// Parsed view over the MNIST weight blob bundled as
// IDR_DIGIT_CLASSIFIER_WEIGHTS. The blob is produced offline from
// digit_classifier.tflite; the runtime never parses flatbuffers.
//
// The network is a two-layer MLP:
//   hidden[j] = relu(dot(W1[j], input) + b1[j])   j < kHiddenSize
//   logits[i] = dot(W2[i], hidden) + b2[i]        i < kOutputSize
//
// Softmax is deliberately not applied: it is monotonic, so it cannot change
// which digit wins.
class MODULES_EXPORT DigitClassifierWeights {
 public:
  // 28 * 28 * 3 -- the model's input tensor is [1,28,28,3] NHWC.
  static constexpr uint32_t kInputSize = 2352;
  static constexpr uint32_t kHiddenSize = 128;
  static constexpr uint32_t kOutputSize = 10;

  // Reads and validates the bundled resource. Returns nullopt if the resource
  // is missing, truncated, or declares shapes this build does not expect.
  static std::optional<DigitClassifierWeights> Load();

  DigitClassifierWeights(DigitClassifierWeights&&);
  DigitClassifierWeights& operator=(DigitClassifierWeights&&);
  ~DigitClassifierWeights();

  // Row-major [kHiddenSize][kInputSize].
  base::span<const float> layer1_weights() const;
  base::span<const float> layer1_bias() const;
  // Row-major [kOutputSize][kHiddenSize].
  base::span<const float> layer2_weights() const;
  base::span<const float> layer2_bias() const;

 private:
  explicit DigitClassifierWeights(Vector<float> storage);

  Vector<float> storage_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_MODULES_DIGITCLASSIFIER_DIGIT_CLASSIFIER_WEIGHTS_H_
