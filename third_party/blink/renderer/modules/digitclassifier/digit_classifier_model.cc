// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/digitclassifier/digit_classifier_model.h"

#include <utility>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/numerics/byte_conversions.h"
#include "third_party/blink/renderer/core/execution_context/agent.h"
#include "third_party/blink/renderer/core/execution_context/execution_context.h"
#include "third_party/blink/renderer/platform/bindings/exception_code.h"
#include "third_party/blink/renderer/platform/bindings/exception_state.h"
#include "third_party/blink/renderer/platform/graphics/gpu/webgpu_callback.h"
#include "third_party/blink/renderer/platform/scheduler/public/event_loop.h"
#include "third_party/blink/renderer/platform/wtf/functional.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

namespace blink {

namespace {

constexpr size_t kResultSize = sizeof(uint32_t);

// One workgroup, one invocation per hidden unit. Both barriers sit in uniform
// control flow, and workgroup storage is 552 bytes against a 16KiB floor.
//
// Softmax is intentionally absent: it is monotonic, so it cannot change which
// logit is largest, and `classify()` only reports the winner.
constexpr char kShaderSource[] = R"(
const kInput  : u32 = 2352u;
const kHidden : u32 = 128u;
const kOutput : u32 = 10u;

@group(0) @binding(0) var<storage, read>       w1       : array<f32>;
@group(0) @binding(1) var<storage, read>       b1       : array<f32>;
@group(0) @binding(2) var<storage, read>       w2       : array<f32>;
@group(0) @binding(3) var<storage, read>       b2       : array<f32>;
@group(0) @binding(4) var<storage, read>       in_data  : array<f32>;
@group(0) @binding(5) var<storage, read_write> out_data : array<u32>;

var<workgroup> hidden : array<f32, 128>;
var<workgroup> logits : array<f32, 10>;

@compute @workgroup_size(128)
fn main(@builtin(local_invocation_id) lid : vec3<u32>) {
  let j = lid.x;

  // Layer 1: one invocation per hidden unit, scalar ReLU.
  var acc = b1[j];
  let row = j * kInput;
  for (var k = 0u; k < kInput; k = k + 1u) {
    acc = acc + w1[row + k] * in_data[k];
  }
  hidden[j] = max(acc, 0.0);

  workgroupBarrier();

  // Layer 2: the first kOutput invocations.
  if (j < kOutput) {
    var o = b2[j];
    let row2 = j * kHidden;
    for (var h = 0u; h < kHidden; h = h + 1u) {
      o = o + w2[row2 + h] * hidden[h];
    }
    logits[j] = o;
  }

  workgroupBarrier();

  if (j == 0u) {
    var best = 0u;
    var best_value = logits[0];
    for (var i = 1u; i < kOutput; i = i + 1u) {
      if (logits[i] > best_value) {
        best_value = logits[i];
        best = i;
      }
    }
    out_data[0] = best;
  }
}
)";

wgpu::Buffer CreateStorageBuffer(const wgpu::Device& device,
                                 const wgpu::Queue& queue,
                                 base::span<const float> data) {
  wgpu::BufferDescriptor descriptor = {};
  descriptor.size = data.size_bytes();
  descriptor.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer buffer = device.CreateBuffer(&descriptor);
  if (buffer) {
    queue.WriteBuffer(buffer, 0, data.data(), data.size_bytes());
  }
  return buffer;
}

}  // namespace

// static
DigitClassifierModel* DigitClassifierModel::Create(
    ExecutionContext* execution_context,
    scoped_refptr<DawnControlClientHolder> dawn_control_client,
    wgpu::Device device,
    DigitClassifierWeights weights) {
  auto* model = MakeGarbageCollected<DigitClassifierModel>(
      execution_context, std::move(dawn_control_client), std::move(device));
  if (!model->Initialize(weights)) {
    return nullptr;
  }
  return model;
}

DigitClassifierModel::DigitClassifierModel(
    ExecutionContext* execution_context,
    scoped_refptr<DawnControlClientHolder> dawn_control_client,
    wgpu::Device device)
    : ExecutionContextClient(execution_context),
      dawn_control_client_(std::move(dawn_control_client)),
      device_(std::move(device)) {}

DigitClassifierModel::~DigitClassifierModel() = default;

bool DigitClassifierModel::Initialize(const DigitClassifierWeights& weights) {
  wgpu::Queue queue = device_.GetQueue();

  layer1_weights_buffer_ =
      CreateStorageBuffer(device_, queue, weights.layer1_weights());
  layer1_bias_buffer_ =
      CreateStorageBuffer(device_, queue, weights.layer1_bias());
  layer2_weights_buffer_ =
      CreateStorageBuffer(device_, queue, weights.layer2_weights());
  layer2_bias_buffer_ =
      CreateStorageBuffer(device_, queue, weights.layer2_bias());

  wgpu::BufferDescriptor input_descriptor = {};
  input_descriptor.size = kExpectedInputLength * sizeof(float);
  input_descriptor.usage =
      wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
  input_buffer_ = device_.CreateBuffer(&input_descriptor);

  wgpu::BufferDescriptor result_descriptor = {};
  result_descriptor.size = kResultSize;
  result_descriptor.usage =
      wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc;
  result_buffer_ = device_.CreateBuffer(&result_descriptor);

  if (!layer1_weights_buffer_ || !layer1_bias_buffer_ ||
      !layer2_weights_buffer_ || !layer2_bias_buffer_ || !input_buffer_ ||
      !result_buffer_) {
    return false;
  }

  wgpu::ShaderSourceWGSL wgsl_source = {};
  wgsl_source.code = kShaderSource;
  wgpu::ShaderModuleDescriptor shader_descriptor = {};
  shader_descriptor.nextInChain = &wgsl_source;
  wgpu::ShaderModule shader_module =
      device_.CreateShaderModule(&shader_descriptor);
  if (!shader_module) {
    return false;
  }

  wgpu::ComputePipelineDescriptor pipeline_descriptor = {};
  pipeline_descriptor.compute.module = shader_module;
  pipeline_descriptor.compute.entryPoint = "main";
  pipeline_ = device_.CreateComputePipeline(&pipeline_descriptor);
  if (!pipeline_) {
    return false;
  }

  const std::array<wgpu::BindGroupEntry, 6> entries = {{
      {.binding = 0, .buffer = layer1_weights_buffer_},
      {.binding = 1, .buffer = layer1_bias_buffer_},
      {.binding = 2, .buffer = layer2_weights_buffer_},
      {.binding = 3, .buffer = layer2_bias_buffer_},
      {.binding = 4, .buffer = input_buffer_},
      {.binding = 5, .buffer = result_buffer_},
  }};
  wgpu::BindGroupDescriptor bind_group_descriptor = {};
  bind_group_descriptor.layout = pipeline_.GetBindGroupLayout(0);
  bind_group_descriptor.entryCount = entries.size();
  bind_group_descriptor.entries = entries.data();
  bind_group_ = device_.CreateBindGroup(&bind_group_descriptor);

  return bind_group_ != nullptr;
}

ScriptPromise<IDLString> DigitClassifierModel::classify(
    ScriptState* script_state,
    NotShared<DOMFloat32Array> input,
    ExceptionState& exception_state) {
  if (!script_state->ContextIsValid()) {
    exception_state.ThrowDOMException(DOMExceptionCode::kInvalidStateError,
                                      "Invalid script state");
    return EmptyPromise();
  }

  if (input->IsDetached()) {
    exception_state.ThrowTypeError("The input buffer is detached.");
    return EmptyPromise();
  }

  if (input->length() != kExpectedInputLength) {
    exception_state.ThrowTypeError(
        String::Format("The input buffer must contain exactly %zu elements "
                       "(28 * 28 * 3), but it contains %zu.",
                       kExpectedInputLength, input->length()));
    return EmptyPromise();
  }

  if (dawn_control_client_->IsContextLost()) {
    exception_state.ThrowDOMException(DOMExceptionCode::kOperationError,
                                      "The GPU context was lost.");
    return EmptyPromise();
  }

  // A fresh readback buffer per call keeps overlapping classify() calls from
  // racing on a mapped range. Everything else is ordered by the queue.
  wgpu::BufferDescriptor readback_descriptor = {};
  readback_descriptor.size = kResultSize;
  readback_descriptor.usage =
      wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer readback = device_.CreateBuffer(&readback_descriptor);
  if (!readback) {
    exception_state.ThrowDOMException(DOMExceptionCode::kOperationError,
                                      "Failed to allocate a readback buffer.");
    return EmptyPromise();
  }

  wgpu::Queue queue = device_.GetQueue();
  const base::span<const float> values = input->AsSpan();
  queue.WriteBuffer(input_buffer_, 0, values.data(), values.size_bytes());

  wgpu::CommandEncoder encoder = device_.CreateCommandEncoder();
  {
    wgpu::ComputePassEncoder pass = encoder.BeginComputePass();
    pass.SetPipeline(pipeline_);
    pass.SetBindGroup(0, bind_group_);
    pass.DispatchWorkgroups(1);
    pass.End();
  }
  encoder.CopyBufferToBuffer(result_buffer_, 0, readback, 0, kResultSize);
  wgpu::CommandBuffer commands = encoder.Finish();
  queue.Submit(1, &commands);

  auto* resolver = MakeGarbageCollected<ScriptPromiseResolver<IDLString>>(
      script_state, exception_state.GetContext());
  auto promise = resolver->Promise();

  auto* callback = MakeWGPUOnceCallback(resolver->WrapCallbackInScriptScope(
      BindOnce(&DigitClassifierModel::OnResultMapped,
                    WrapPersistent(this), readback)));
  readback.MapAsync(wgpu::MapMode::Read, 0, kResultSize,
                    wgpu::CallbackMode::AllowProcessEvents,
                    callback->UnboundCallback(), callback->AsUserdata());
  dawn_control_client_->EnsureFlush(
      *ExecutionContext::From(script_state)->GetAgent()->event_loop());

  return promise;
}

void DigitClassifierModel::OnResultMapped(
    wgpu::Buffer readback,
    ScriptPromiseResolver<IDLString>* resolver,
    wgpu::MapAsyncStatus status,
    wgpu::StringView error_message) {
  if (status != wgpu::MapAsyncStatus::Success) {
    resolver->RejectWithDOMException(
        DOMExceptionCode::kOperationError,
        "Failed to read the classification result back from the GPU.");
    return;
  }

  const void* mapped = readback.GetConstMappedRange(0, kResultSize);
  if (!mapped) {
    readback.Unmap();
    resolver->RejectWithDOMException(
        DOMExceptionCode::kOperationError,
        "The classification result could not be mapped.");
    return;
  }

  // SAFETY: `kResultSize` bytes were requested from GetConstMappedRange() and
  // it returned non-null, so the range is valid for exactly that many bytes.
  const auto bytes = UNSAFE_BUFFERS(
      base::span(static_cast<const uint8_t*>(mapped), kResultSize));
  const uint32_t digit = base::U32FromLittleEndian(bytes.first<4u>());
  readback.Unmap();

  if (digit >= DigitClassifierWeights::kOutputSize) {
    resolver->RejectWithDOMException(
        DOMExceptionCode::kOperationError,
        "The classifier produced an out-of-range result.");
    return;
  }

  resolver->Resolve(String::Number(digit));
}

void DigitClassifierModel::Trace(Visitor* visitor) const {
  ScriptWrappable::Trace(visitor);
  ExecutionContextClient::Trace(visitor);
}

}  // namespace blink
