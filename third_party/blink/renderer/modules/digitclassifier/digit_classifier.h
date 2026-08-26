// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_MODULES_DIGITCLASSIFIER_DIGIT_CLASSIFIER_H_
#define THIRD_PARTY_BLINK_RENDERER_MODULES_DIGITCLASSIFIER_DIGIT_CLASSIFIER_H_

#include "base/functional/callback_forward.h"
#include "third_party/blink/renderer/bindings/core/v8/script_promise.h"
#include "third_party/blink/renderer/bindings/core/v8/script_promise_resolver.h"
#include "third_party/blink/renderer/core/execution_context/execution_context_lifecycle_observer.h"
#include "third_party/blink/renderer/modules/modules_export.h"
#include "third_party/blink/renderer/platform/bindings/script_wrappable.h"
#include "third_party/blink/renderer/platform/graphics/gpu/dawn_control_client_holder.h"
#include "third_party/blink/renderer/platform/graphics/gpu/webgpu_cpp.h"
#include "third_party/blink/renderer/platform/heap/visitor.h"
#include "third_party/blink/renderer/platform/wtf/vector.h"

namespace blink {

class DigitClassifierModel;
class ExceptionState;
class ExecutionContext;
class ScriptState;

// Implements `navigator.digitclassifier`. Hands out the MNIST classifier that
// is bundled as IDR_DIGIT_CLASSIFIER_WEIGHTS and executed with compute shaders
// on the GPU.
//
// The GPU is reached the same way WebGPU reaches it: a DawnControlClientHolder
// wraps a WebGPU context provider, and Dawn commands are serialized over the
// wire to the GPU process. No separate service is involved.
class MODULES_EXPORT DigitClassifier final : public ScriptWrappable,
                                             public ExecutionContextClient {
  DEFINE_WRAPPERTYPEINFO();

 public:
  explicit DigitClassifier(ExecutionContext* execution_context);

  DigitClassifier(const DigitClassifier&) = delete;
  DigitClassifier& operator=(const DigitClassifier&) = delete;

  void Trace(blink::Visitor*) const override;

  // IDL interface:
  ScriptPromise<DigitClassifierModel> getModel(ScriptState* script_state,
                                               ExceptionState& exception_state);

 private:
  // Creates the WebGPU context provider if needed, then runs `callback`. The
  // provider is created asynchronously and shared by every getModel() call.
  void EnsureDawnControlClient(ScriptState* script_state,
                               base::OnceClosure callback);
  void OnDawnControlClientReady();

  void RequestDevice(ScriptState* script_state,
                     ScriptPromiseResolver<DigitClassifierModel>* resolver);
  void OnRequestAdapter(ScriptState* script_state,
                        ScriptPromiseResolver<DigitClassifierModel>* resolver,
                        wgpu::RequestAdapterStatus status,
                        wgpu::Adapter adapter,
                        wgpu::StringView error_message);
  void OnRequestDevice(ScriptPromiseResolver<DigitClassifierModel>* resolver,
                       wgpu::RequestDeviceStatus status,
                       wgpu::Device device,
                       wgpu::StringView error_message);

  scoped_refptr<DawnControlClientHolder> dawn_control_client_;
  Vector<base::OnceClosure> dawn_control_client_ready_callbacks_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_MODULES_DIGITCLASSIFIER_DIGIT_CLASSIFIER_H_
