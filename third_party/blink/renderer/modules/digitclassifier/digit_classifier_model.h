// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_MODULES_DIGITCLASSIFIER_DIGIT_CLASSIFIER_MODEL_H_
#define THIRD_PARTY_BLINK_RENDERER_MODULES_DIGITCLASSIFIER_DIGIT_CLASSIFIER_MODEL_H_

#include <cstddef>

#include "third_party/blink/renderer/bindings/core/v8/script_promise.h"
#include "third_party/blink/renderer/bindings/core/v8/script_promise_resolver.h"
#include "third_party/blink/renderer/core/execution_context/execution_context_lifecycle_observer.h"
#include "third_party/blink/renderer/core/typed_arrays/array_buffer_view_helpers.h"
#include "third_party/blink/renderer/core/typed_arrays/dom_typed_array.h"
#include "third_party/blink/renderer/modules/digitclassifier/digit_classifier_weights.h"
#include "third_party/blink/renderer/modules/modules_export.h"
#include "third_party/blink/renderer/platform/bindings/script_wrappable.h"
#include "third_party/blink/renderer/platform/graphics/gpu/dawn_control_client_holder.h"
#include "third_party/blink/renderer/platform/graphics/gpu/webgpu_cpp.h"
#include "third_party/blink/renderer/platform/heap/visitor.h"

namespace blink {

class ExceptionState;
class ExecutionContext;
class ScriptState;

// A loaded MNIST classifier. The model's input is 28x28 RGB in NHWC order, so
// callers pass 28 * 28 * 3 floats normalized to [0.0, 1.0].
//
// The whole network runs in a single compute dispatch of one 128-invocation
// workgroup: at 0.6 MFLOP the arithmetic is trivial next to dispatch and
// readback overhead, so minimizing round trips matters more than parallelism.
class MODULES_EXPORT DigitClassifierModel final : public ScriptWrappable,
                                                  public ExecutionContextClient {
  DEFINE_WRAPPERTYPEINFO();

 public:
  // 28 * 28 * 3, matching the [1,28,28,3] input tensor of the bundled model.
  static constexpr size_t kExpectedInputLength =
      DigitClassifierWeights::kInputSize;

  // Uploads `weights` and builds the compute pipeline. Returns nullptr if any
  // GPU resource could not be created.
  static DigitClassifierModel* Create(
      ExecutionContext* execution_context,
      scoped_refptr<DawnControlClientHolder> dawn_control_client,
      wgpu::Device device,
      DigitClassifierWeights weights);

  DigitClassifierModel(ExecutionContext* execution_context,
                       scoped_refptr<DawnControlClientHolder> dawn_control_client,
                       wgpu::Device device);

  DigitClassifierModel(const DigitClassifierModel&) = delete;
  DigitClassifierModel& operator=(const DigitClassifierModel&) = delete;
  ~DigitClassifierModel() override;

  void Trace(blink::Visitor*) const override;

  // IDL interface:
  ScriptPromise<IDLString> classify(ScriptState* script_state,
                                    NotShared<DOMFloat32Array> input,
                                    ExceptionState& exception_state);

 private:
  bool Initialize(const DigitClassifierWeights& weights);

  // `readback` is bound at call time; `resolver` is injected by
  // WrapCallbackInScriptScope() as the first unbound parameter, so it must
  // follow every bound argument.
  void OnResultMapped(wgpu::Buffer readback,
                      ScriptPromiseResolver<IDLString>* resolver,
                      wgpu::MapAsyncStatus status,
                      wgpu::StringView error_message);

  scoped_refptr<DawnControlClientHolder> dawn_control_client_;
  wgpu::Device device_;
  wgpu::ComputePipeline pipeline_;
  wgpu::BindGroup bind_group_;
  wgpu::Buffer layer1_weights_buffer_;
  wgpu::Buffer layer1_bias_buffer_;
  wgpu::Buffer layer2_weights_buffer_;
  wgpu::Buffer layer2_bias_buffer_;
  wgpu::Buffer input_buffer_;
  wgpu::Buffer result_buffer_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_MODULES_DIGITCLASSIFIER_DIGIT_CLASSIFIER_MODEL_H_
