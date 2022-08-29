//   Copyright (c) 2018 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "paddle/fluid/framework/details/sparse_all_reduce_op_handle.h"
#include <algorithm>
#include <utility>
#include "dgc/dgc.h"
#include "paddle/fluid/framework/details/container_cast.h"
#include "paddle/fluid/framework/details/reduce_and_gather.h"
#include "paddle/fluid/framework/details/variable_visitor.h"
#include "paddle/fluid/framework/operator.h"
#include "paddle/fluid/memory/malloc.h"
#include "paddle/fluid/platform/cuda_device_guard.h"
#include "paddle/fluid/platform/gpu_info.h"
#include "paddle/fluid/platform/profiler.h"

DECLARE_bool(sync_nccl_allreduce);

namespace paddle {
namespace framework {
namespace details {

SparseAllReduceOpHandle::SparseAllReduceOpHandle(
    ir::Node *node, const std::vector<Scope *> &local_scopes,
    const std::vector<platform::Place> &places,
    const platform::NCCLCommunicator *ctxs, bool is_encoded, int nranks)
    : AllReduceOpHandle(node, local_scopes, places, ctxs),
      is_encoded_(is_encoded),
      nranks_(nranks) {
  // TODO(gongwb) :polish them!
  if (is_encoded) {
    VLOG(1) << "Use dgc allreduce mode";
  }
}

void SparseAllReduceOpHandle::WaitInputVarGenerated() {
#ifdef PADDLE_WITH_CUDA
  for (auto &p : dev_ctxes_) {
    if (platform::is_gpu_place(p.first)) {
      int dev_id = boost::get<platform::CUDAPlace>(p.first).device;
      auto *compute_dev_ctx =
          platform::DeviceContextPool::Instance().GetByPlace(
              platform::CUDAPlace(dev_id));
      auto *dev_ctx = static_cast<platform::CUDADeviceContext *>(p.second);
      if (compute_dev_ctx->stream() != dev_ctx->stream()) {
        auto &event = events_.at(dev_id);
        PADDLE_ENFORCE_CUDA_SUCCESS(
            cudaEventRecord(event, compute_dev_ctx->stream()));
        PADDLE_ENFORCE_CUDA_SUCCESS(
            cudaStreamWaitEvent(dev_ctx->stream(), event, 0));
      }
    }
  }
#endif
}

void SparseAllReduceOpHandle::RunImplEncoded() {
  platform::RecordEvent record_event(Name());

  auto in_var_handles = DynamicCast<VarHandle>(this->Inputs());
  auto out_var_handles = DynamicCast<VarHandle>(this->Outputs());
  PADDLE_ENFORCE_EQ(
      in_var_handles.size(), places_.size(),
      "The NoDummyInputSize should be equal to the number of places.");
  PADDLE_ENFORCE_EQ(
      in_var_handles.size(), out_var_handles.size(),
      "The NoDummyInputSize and NoDummyOutputSize should be equal.");

  std::vector<const LoDTensor *> ins;
  std::vector<LoDTensor *> outs;
  int k = -1;
  for (size_t i = 0; i < local_scopes_.size(); ++i) {
    auto *local_scope = local_exec_scopes_[i];
    auto original_name =
        paddle::framework::GradOriginalVarName(in_var_handles[i]->name());
    auto encode_var_name = original_name + g_dgc_encoded;
    auto *in_var = local_scope->FindVar(encode_var_name);
    PADDLE_ENFORCE_NOT_NULL(in_var, "%s should not be null", encode_var_name);
    auto &in = in_var->Get<LoDTensor>();
    ins.emplace_back(&in);

    auto *out = local_scope->FindVar(out_var_handles[i]->name())
                    ->GetMutable<LoDTensor>();
    outs.emplace_back(out);

    if (k < 0) {
      k = GetKValue(in_var_handles[i]->name());
    }
  }

  PADDLE_ENFORCE(platform::is_gpu_place(ins[0]->place()));
  PADDLE_ENFORCE(platform::is_gpu_place(outs[0]->place()));
  PADDLE_ENFORCE(nccl_ctxs_, "nccl_ctxs should not be nullptr.");

  int dtype = -1;
  size_t in_numel = 0;
  size_t out_numel = 0;
  PADDLE_ENFORCE(nranks_ > 1);
  std::vector<std::function<void()>> all_gather_calls;
  std::vector<std::function<void()>> sparse_reduce_calls;

  std::vector<memory::AllocationPtr> allocations;

  for (size_t i = 0; i < local_scopes_.size(); ++i) {
    auto &place = places_[i];
    auto &in = *ins[i];
    void *in_tensor_buf = const_cast<void *>(in.data<void>());

    auto &out = *outs[i];
    float *out_tensor_buf = out.data<float>();

    dtype = (dtype == -1) ? platform::ToNCCLDataType(in.type()) : dtype;
    in_numel = (in_numel == 0) ? static_cast<size_t>(in.numel()) : in_numel;
    PADDLE_ENFORCE(in_numel % 2 == 0);
    PADDLE_ENFORCE(in_numel / 2 == static_cast<size_t>(k));
    out_numel = (out_numel == 0) ? static_cast<size_t>(out.numel()) : out_numel;

    int dev_id = boost::get<platform::CUDAPlace>(place).device;
    auto *nccl_ctxs = nccl_ctxs_->GetRunEnvNCCLCtx(run_order_, false);
    auto &nccl_ctx = nccl_ctxs->at(dev_id);
    auto stream = nccl_ctx.stream();
    auto comm = nccl_ctx.comm_;

    int encode_size = 2 * k * sizeof(int);
    // dgc use ncclAllGather to get all the encoded data
    // so the buffer need nranks.
    int buf_size = nranks_ * encode_size;
    auto tmp_ious_data = memory::Alloc(place, buf_size);
    void *gather_buff = reinterpret_cast<void *>(tmp_ious_data->ptr());
    allocations.emplace_back(std::move(tmp_ious_data));

    VLOG(10) << "in_numel:" << in_numel << ", out_numel:" << out_numel
             << ", nranks:" << nranks_ << ", gather_buf size:" << buf_size
             << ", k:" << k << ", place:" << place << ", dtype:" << dtype;

    all_gather_calls.emplace_back([=] {
      PADDLE_ENFORCE_CUDA_SUCCESS(platform::dynload::ncclAllGather(
          in_tensor_buf, gather_buff, 2 * k, static_cast<ncclDataType_t>(dtype),
          comm, stream));
    });

    sparse_reduce_calls.emplace_back([=] {
      platform::CUDADeviceGuard guard(dev_id);
      PADDLE_ENFORCE_EQ(paddle::communication::dgc::sparseReduce(
                            gather_buff, k, out_tensor_buf,
                            static_cast<int>(out_numel), nranks_, stream),
                        true);
    });
  }

  WaitInputVarGenerated();
  SparseAllReduceFunc(all_gather_calls, sparse_reduce_calls);
}

void SparseAllReduceOpHandle::SparseAllReduceFunc(
    const std::vector<std::function<void()>> &all_gather_calls,
    const std::vector<std::function<void()>> &sparse_reduce_calls) {
  this->RunAndRecordEvent([&] {
    if (all_gather_calls.size() == 1UL) {
      // Do not use NCCLGroup when manage NCCL by per thread per device
      all_gather_calls[0]();
    } else {
      platform::NCCLGroupGuard guard;
      for (auto &call : all_gather_calls) {
        call();
      }
    }

    for (auto &call : sparse_reduce_calls) {
      call();
    }
  });

  SyncNCCLAllReduce();
}

int SparseAllReduceOpHandle::GetKValue(const std::string &grad_name) {
  auto original_name = paddle::framework::GradOriginalVarName(grad_name);
  auto var_name = original_name + g_dgc_k;
  PADDLE_ENFORCE(local_scopes_.size() > 0);

  auto *scope = local_exec_scopes_[0];
  auto var = scope->FindVar(var_name);
  PADDLE_ENFORCE_NOT_NULL(var);
  auto tensor = var->Get<LoDTensor>().data<float>();
  return *tensor;
}

bool SparseAllReduceOpHandle::IsEncoded() {
  if (!is_encoded_) {
    return false;
  }
  auto counter_name = g_dgc_counter_name;
  auto step_name = g_dgc_rampup_begin_step;
  PADDLE_ENFORCE(local_scopes_.size() > 0);

  auto *local_scope = local_exec_scopes_[0];
  auto count_var = local_scope->FindVar(counter_name);
  auto step_var = local_scope->FindVar(step_name);
  if (count_var == nullptr || step_var == nullptr) {
    PADDLE_THROW("not find count_var:%s or step_var:%s", counter_name,
                 step_var);
  }

  float count = *count_var->Get<LoDTensor>().data<float>();
  float step = *step_var->Get<LoDTensor>().data<float>();
  if (static_cast<int>(count) < static_cast<int>(step)) {
    VLOG(10) << "in all_reduce currentstep:" << count
             << " < rampup_begin_step:" << step
             << " so not use sparse all reduce";
    return false;
  }

  return true;
}

void SparseAllReduceOpHandle::RunImpl() {
  if (!IsEncoded()) {
    AllReduceOpHandle::RunImpl();
    return;
  }

  RunImplEncoded();
}

std::string SparseAllReduceOpHandle::Name() const {
  return "sparse_all_reduce";
}
}  // namespace details
}  // namespace framework
}  // namespace paddle