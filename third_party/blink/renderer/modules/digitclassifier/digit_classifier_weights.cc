// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/digitclassifier/digit_classifier_weights.h"

#include <algorithm>

#include "base/containers/span.h"
#include "base/logging.h"
#include "base/numerics/byte_conversions.h"
#include "third_party/blink/public/resources/grit/blink_resources.h"
#include "third_party/blink/renderer/platform/data_resource_helper.h"

namespace blink {

namespace {

// Blob header, little-endian. Mirrors extract_weights.py.
//   0   char[4]  magic "DCLS"
//   4   u32      version
//   8   u32      input_size
//   12  u32      hidden_size
//   16  u32      output_size
//   20  u32[3]   reserved
constexpr size_t kHeaderSize = 32;
constexpr uint32_t kExpectedVersion = 1;
constexpr char kMagic[] = {'D', 'C', 'L', 'S'};

constexpr size_t kLayer1WeightCount =
    static_cast<size_t>(DigitClassifierWeights::kHiddenSize) *
    DigitClassifierWeights::kInputSize;
constexpr size_t kLayer2WeightCount =
    static_cast<size_t>(DigitClassifierWeights::kOutputSize) *
    DigitClassifierWeights::kHiddenSize;
constexpr size_t kLayer1BiasOffset = kLayer1WeightCount;
constexpr size_t kLayer2WeightOffset =
    kLayer1BiasOffset + DigitClassifierWeights::kHiddenSize;
constexpr size_t kLayer2BiasOffset = kLayer2WeightOffset + kLayer2WeightCount;
constexpr size_t kTotalFloatCount =
    kLayer2BiasOffset + DigitClassifierWeights::kOutputSize;

}  // namespace

// static
std::optional<DigitClassifierWeights> DigitClassifierWeights::Load() {
  const Vector<char> blob =
      UncompressResourceAsBinary(IDR_DIGIT_CLASSIFIER_WEIGHTS);

  const size_t expected_size = kHeaderSize + kTotalFloatCount * sizeof(float);
  if (blob.size() != expected_size) {
    DLOG(ERROR) << "digit classifier weights: got " << blob.size()
                << " bytes, expected " << expected_size;
    return std::nullopt;
  }

  const auto chars = base::span(blob);
  if (!std::ranges::equal(chars.first<4u>(), base::span(kMagic))) {
    DLOG(ERROR) << "digit classifier weights: bad magic";
    return std::nullopt;
  }

  const auto bytes = base::as_byte_span(blob);
  const uint32_t version = base::U32FromLittleEndian(bytes.subspan<4u, 4u>());
  if (version != kExpectedVersion) {
    DLOG(ERROR) << "digit classifier weights: version " << version
                << ", expected " << kExpectedVersion;
    return std::nullopt;
  }

  // The shader is compiled against these sizes, so a blob describing a
  // different network must be rejected rather than reinterpreted.
  if (base::U32FromLittleEndian(bytes.subspan<8u, 4u>()) != kInputSize ||
      base::U32FromLittleEndian(bytes.subspan<12u, 4u>()) != kHiddenSize ||
      base::U32FromLittleEndian(bytes.subspan<16u, 4u>()) != kOutputSize) {
    DLOG(ERROR) << "digit classifier weights: unexpected layer shapes";
    return std::nullopt;
  }

  // Copy the payload out of the resource into float storage. The resource is
  // little-endian f32, matching every platform Chromium targets.
  // `allow_nonunique_obj` is required because float has padding-free but
  // non-unique object representations (NaN payloads, signed zero); the bytes
  // are only ever copied, never compared or hashed.
  Vector<float> storage(static_cast<wtf_size_t>(kTotalFloatCount));
  base::as_writable_bytes(base::allow_nonunique_obj, base::span(storage))
      .copy_from(bytes.subspan(kHeaderSize));

  return DigitClassifierWeights(std::move(storage));
}

DigitClassifierWeights::DigitClassifierWeights(Vector<float> storage)
    : storage_(std::move(storage)) {}

DigitClassifierWeights::DigitClassifierWeights(DigitClassifierWeights&&) =
    default;
DigitClassifierWeights& DigitClassifierWeights::operator=(
    DigitClassifierWeights&&) = default;
DigitClassifierWeights::~DigitClassifierWeights() = default;

base::span<const float> DigitClassifierWeights::layer1_weights() const {
  return base::span(storage_).subspan(0u, kLayer1WeightCount);
}

base::span<const float> DigitClassifierWeights::layer1_bias() const {
  return base::span(storage_).subspan(kLayer1BiasOffset,
                                      size_t{kHiddenSize});
}

base::span<const float> DigitClassifierWeights::layer2_weights() const {
  return base::span(storage_).subspan(kLayer2WeightOffset, kLayer2WeightCount);
}

base::span<const float> DigitClassifierWeights::layer2_bias() const {
  return base::span(storage_).subspan(kLayer2BiasOffset, size_t{kOutputSize});
}

}  // namespace blink
