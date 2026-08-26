// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/digitclassifier/digit_classifier.h"

#include <string_view>
#include <utility>

#include "gpu/command_buffer/client/webgpu_interface.h"
#include "third_party/blink/public/platform/platform.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/execution_context/agent.h"
#include "third_party/blink/renderer/core/execution_context/execution_context.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/modules/digitclassifier/digit_classifier_model.h"
#include "third_party/blink/renderer/modules/digitclassifier/digit_classifier_weights.h"
#include "third_party/blink/renderer/platform/bindings/exception_code.h"
#include "third_party/blink/renderer/platform/bindings/exception_state.h"
#include "third_party/blink/renderer/platform/graphics/gpu/webgpu_callback.h"
#include "third_party/blink/renderer/platform/graphics/web_graphics_context_3d_provider_util.h"
#include "third_party/blink/renderer/platform/heap/cross_thread_handle.h"
#include "third_party/blink/renderer/platform/scheduler/public/event_loop.h"
#include "third_party/blink/renderer/platform/wtf/cross_thread_functional.h"
#include "third_party/blink/renderer/platform/wtf/functional.h"

namespace blink {

namespace {

// wgpu::StringView's conversion operator resolves the WGPU_STRLEN sentinel,
// so an undefined message arrives here as an empty view.
String StringViewToString(wgpu::StringView view) {
  const std::string_view message = view;
  if (message.empty()) {
    return String();
  }
  return String::FromUtf8(message);
}

}  // namespace

DigitClassifier::DigitClassifier(ExecutionContext* execution_context)
    : ExecutionContextClient(execution_context) {}

ScriptPromise<DigitClassifierModel> DigitClassifier::getModel(
    ScriptState* script_state,
    ExceptionState& exception_state) {
  if (!script_state->ContextIsValid()) {
    exception_state.ThrowDOMException(DOMExceptionCode::kInvalidStateError,
                                      "Invalid script state");
    return EmptyPromise();
  }

  auto* resolver =
      MakeGarbageCollected<ScriptPromiseResolver<DigitClassifierModel>>(
          script_state, exception_state.GetContext());
  auto promise = resolver->Promise();

  EnsureDawnControlClient(
      script_state,
      BindOnce(&DigitClassifier::RequestDevice, WrapPersistent(this),
                    WrapPersistent(script_state), WrapPersistent(resolver)));

  return promise;
}

void DigitClassifier::EnsureDawnControlClient(ScriptState* script_state,
                                              base::OnceClosure callback) {
  if (dawn_control_client_ && !dawn_control_client_->IsContextLost()) {
    std::move(callback).Run();
    return;
  }

  dawn_control_client_ready_callbacks_.push_back(std::move(callback));

  // A creation request is already in flight; its completion will drain the
  // queue we just appended to.
  if (dawn_control_client_ready_callbacks_.size() > 1) {
    return;
  }

  ExecutionContext* execution_context = ExecutionContext::From(script_state);
  CreateWebGPUGraphicsContext3DProviderAsync(
      execution_context->Url(), Platform::WebGPUReplyThread::kMainThread,
      execution_context->GetTaskRunner(TaskType::kWebGPU),
      CrossThreadBindOnce(
          [](CrossThreadHandle<DigitClassifier> classifier_handle,
             CrossThreadHandle<ExecutionContext> execution_context_handle,
             std::unique_ptr<WebGraphicsContext3DProvider> context_provider) {
            auto unwrap_classifier =
                MakeUnwrappingCrossThreadHandle(classifier_handle);
            auto unwrap_execution_context =
                MakeUnwrappingCrossThreadHandle(execution_context_handle);
            if (!unwrap_classifier || !unwrap_execution_context) {
              return;
            }
            auto* classifier = unwrap_classifier.GetOnCreationThread();
            auto* context = unwrap_execution_context.GetOnCreationThread();

            // DawnControlClientHolder reaches for WebGPUInterface() while
            // constructing, which requires a provider that has been bound to
            // this sequence. See modules/webgpu/gpu.cc, which binds the same
            // way before handing the provider on.
            if (context_provider && context_provider->BindToCurrentSequence()) {
              // The GPU service defers every RequestAdapter reply until it has
              // an isolation key, and it only asks for one when this token
              // arrives. Skipping it leaves getModel() pending forever rather
              // than rejecting. DigitClassifier is Exposed=Window, so the
              // document token is the only case to handle; see
              // GetExecutionContextToken() in modules/webgpu/gpu.cc for the
              // worker variants.
              context_provider->WebGPUInterface()
                  ->SetWebGPUExecutionContextToken(
                      To<LocalDOMWindow>(context)->document()->Token());

              classifier->dawn_control_client_ =
                  DawnControlClientHolder::Create(
                      std::move(context_provider),
                      context->GetTaskRunner(TaskType::kWebGPU));
            }
            classifier->OnDawnControlClientReady();
          },
          MakeCrossThreadHandle(this),
          MakeCrossThreadHandle(execution_context)));
}

void DigitClassifier::OnDawnControlClientReady() {
  Vector<base::OnceClosure> callbacks =
      std::move(dawn_control_client_ready_callbacks_);
  dawn_control_client_ready_callbacks_.clear();
  for (auto& callback : callbacks) {
    std::move(callback).Run();
  }
}

void DigitClassifier::RequestDevice(
    ScriptState* script_state,
    ScriptPromiseResolver<DigitClassifierModel>* resolver) {
  if (!dawn_control_client_ || dawn_control_client_->IsContextLost()) {
    resolver->RejectWithDOMException(
        DOMExceptionCode::kOperationError,
        "Failed to create a WebGPU context for the digit classifier.");
    return;
  }

  ExecutionContext* execution_context = ExecutionContext::From(script_state);

  // The classifier is a plain compute workload; any adapter will do.
  wgpu::RequestAdapterOptions options = {};

  auto* callback = MakeWGPUOnceCallback(resolver->WrapCallbackInScriptScope(
      BindOnce(&DigitClassifier::OnRequestAdapter, WrapPersistent(this),
                    WrapPersistent(script_state))));
  dawn_control_client_->GetWGPUInstance().RequestAdapter(
      &options, wgpu::CallbackMode::AllowProcessEvents,
      callback->UnboundCallback(), callback->AsUserdata());
  dawn_control_client_->EnsureFlush(
      *execution_context->GetAgent()->event_loop());
}

void DigitClassifier::OnRequestAdapter(
    ScriptState* script_state,
    ScriptPromiseResolver<DigitClassifierModel>* resolver,
    wgpu::RequestAdapterStatus status,
    wgpu::Adapter adapter,
    wgpu::StringView error_message) {
  if (status != wgpu::RequestAdapterStatus::Success || adapter == nullptr) {
    String message = StringViewToString(error_message);
    resolver->RejectWithDOMException(
        DOMExceptionCode::kOperationError,
        message.empty() ? String("No GPU adapter is available.") : message);
    return;
  }

  ExecutionContext* execution_context = ExecutionContext::From(script_state);
  wgpu::DeviceDescriptor descriptor = {};

  auto* callback = MakeWGPUOnceCallback(resolver->WrapCallbackInScriptScope(
      BindOnce(&DigitClassifier::OnRequestDevice, WrapPersistent(this))));
  adapter.RequestDevice(&descriptor, wgpu::CallbackMode::AllowProcessEvents,
                        callback->UnboundCallback(), callback->AsUserdata());
  dawn_control_client_->EnsureFlush(
      *execution_context->GetAgent()->event_loop());
}

void DigitClassifier::OnRequestDevice(
    ScriptPromiseResolver<DigitClassifierModel>* resolver,
    wgpu::RequestDeviceStatus status,
    wgpu::Device device,
    wgpu::StringView error_message) {
  if (status != wgpu::RequestDeviceStatus::Success || device == nullptr) {
    String message = StringViewToString(error_message);
    resolver->RejectWithDOMException(
        DOMExceptionCode::kOperationError,
        message.empty() ? String("Failed to create a GPU device.") : message);
    return;
  }

  std::optional<DigitClassifierWeights> weights =
      DigitClassifierWeights::Load();
  if (!weights) {
    resolver->RejectWithDOMException(
        DOMExceptionCode::kOperationError,
        "The bundled digit classifier weights are missing or malformed.");
    return;
  }

  DigitClassifierModel* model = DigitClassifierModel::Create(
      GetExecutionContext(), dawn_control_client_, std::move(device),
      *std::move(weights));
  if (!model) {
    resolver->RejectWithDOMException(
        DOMExceptionCode::kOperationError,
        "Failed to build the digit classifier compute pipeline.");
    return;
  }

  resolver->Resolve(model);
}

void DigitClassifier::Trace(Visitor* visitor) const {
  ScriptWrappable::Trace(visitor);
  ExecutionContextClient::Trace(visitor);
}

}  // namespace blink
