/* Copyright (c) 2019 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#include "paddle/fluid/operators/distributed/communicator.h"
#include <gflags/gflags.h>
#include <paddle/fluid/framework/program_desc.h>
#include <chrono>  // NOLINT
#include <map>
#include <thread>  // NOLINT
#include <unordered_set>
#include "paddle/fluid/framework/eigen.h"
#include "paddle/fluid/framework/selected_rows.h"
#include "paddle/fluid/framework/tensor_util.h"
#include "paddle/fluid/framework/threadpool.h"
#include "paddle/fluid/framework/variable_helper.h"
#include "paddle/fluid/operators/distributed/distributed.h"
#include "paddle/fluid/operators/distributed/parameter_recv.h"
#include "paddle/fluid/operators/distributed/parameter_send.h"

DECLARE_int32(communicator_max_merge_var_num);
DECLARE_int32(communicator_send_queue_size);

DEFINE_bool(communicator_independent_recv_thread, true,
            "use an independent to recv vars from parameter server");
DEFINE_int32(communicator_min_send_grad_num_before_recv, 20,
             "max grad num to send before recv parameters");
DEFINE_int32(communicator_thread_pool_size, 5, "thread num to do send or recv");
DEFINE_int32(communicator_send_wait_times, 5,
             "times that send thread will wait if merge num does not reach "
             "max_merge_var_num");
DEFINE_bool(communicator_fake_rpc, false,
            "fake mode does not really send any thing");
DEFINE_bool(communicator_merge_sparse_grad, true,
            "merge sparse gradient before sending");
DEFINE_int32(communicator_merge_sparse_bucket, 2000,
             "number of threads for sparse var");

namespace paddle {
namespace operators {
namespace distributed {

inline double GetCurrentUS() {
  struct timeval time;
  gettimeofday(&time, NULL);
  return 1e+6 * time.tv_sec + time.tv_usec;
}

template <typename T>
inline void VSUB(int n, const T *x, const T *y, T *z) {
  for (int i = 0; i < n; ++i) {
    z[i] = x[i] - y[i];
  }
}

std::once_flag Communicator::init_flag_;
std::shared_ptr<Communicator> Communicator::communicator_(nullptr);

void AsyncCommunicator::InitImpl(const RpcCtxMap &send_varname_to_ctx,
                                 const RpcCtxMap &recv_varname_to_ctx,
                                 Scope *recv_scope) {
  send_varname_to_ctx_ = std::move(send_varname_to_ctx);
  recv_varname_to_ctx_ = std::move(recv_varname_to_ctx);
  recv_scope_ = std::move(recv_scope);

  // get all send information from graph, build vars_to_send
  VLOG(0) << "communicator_independent_recv_thread: "
          << FLAGS_communicator_independent_recv_thread;
  VLOG(0) << "communicator_send_queue_size: "
          << FLAGS_communicator_send_queue_size;
  VLOG(0) << "communicator_min_send_grad_num_before_recv: "
          << FLAGS_communicator_min_send_grad_num_before_recv;
  VLOG(0) << "communicator_thread_pool_size: "
          << FLAGS_communicator_thread_pool_size;
  VLOG(0) << "communicator_send_wait_times: "
          << FLAGS_communicator_send_wait_times;
  VLOG(0) << "communicator_max_merge_var_num: "
          << FLAGS_communicator_max_merge_var_num;
  VLOG(0) << "communicator_fake_rpc: " << FLAGS_communicator_fake_rpc;
  VLOG(0) << "communicator_merge_sparse_grad: "
          << FLAGS_communicator_merge_sparse_grad;
  VLOG(0) << "communicator_is_sgd_optimizer: "
          << FLAGS_communicator_is_sgd_optimizer;

  if (send_varname_to_ctx.size() == 0) {
    VLOG(0) << "nothing need to be send, will not start send_thread";
  } else {
    send_scope_.reset(new Scope());
    for (auto &iter : send_varname_to_ctx_) {
      send_varname_to_queue_[iter.first] =
          std::make_shared<BlockingQueue<std::shared_ptr<Variable>>>(
              FLAGS_communicator_send_queue_size);
    }
    send_threadpool_.reset(
        new ::ThreadPool(FLAGS_communicator_thread_pool_size));
  }

  if (recv_varname_to_ctx.size() == 0) {
    VLOG(0) << "nothing need to be received, will not start recv_thread";
  } else {
    recv_threadpool_.reset(
        new ::ThreadPool(FLAGS_communicator_thread_pool_size));
  }
}

void AsyncCommunicator::InitImpl(const paddle::framework::ProgramDesc &program,
                                 Scope *param_scope) {
  using RpcCtxMap = operators::distributed::RpcCtxMap;
  VLOG(3) << "ProcessGraph";
  RpcCtxMap send_varname_to_ctx;
  RpcCtxMap recv_varname_to_ctx;
  for (auto *op : program.Block(0).AllOps()) {
    VLOG(3) << "node name " << op->Type();
    if (op->Type() == "send") {
      auto send_var_name = op->Input("X")[0];
      auto send_varnames = boost::get<std::vector<std::string>>(
          op->GetNullableAttr("send_varnames"));
      auto epmap =
          boost::get<std::vector<std::string>>(op->GetNullableAttr("epmap"));
      auto height_section =
          boost::get<std::vector<int64_t>>(op->GetNullableAttr("sections"));
      auto trainer_id = boost::get<int>(op->GetNullableAttr("trainer_id"));
      auto merge_add = boost::get<bool>(op->GetNullableAttr("merge_add"));
      if (!merge_add) {
        merge_add = FLAGS_communicator_is_sgd_optimizer;
      }
      auto use_send_handler =
          boost::get<bool>(op->GetNullableAttr("use_send_handler"));
      send_varname_to_ctx[send_var_name] = operators::distributed::RpcContext(
          send_var_name, send_varnames, epmap, height_section, trainer_id,
          merge_add, use_send_handler);
      VLOG(3) << "find and init an send op: "
              << send_varname_to_ctx[send_var_name];
    } else if (op->Type() == "recv") {
      auto do_not_run = boost::get<int>(op->GetNullableAttr("do_not_run"));
      PADDLE_ENFORCE_GT(do_not_run, 0, "recv should not run!");
      auto recv_var_name = op->Output("Out")[0];
      auto recv_varnames = boost::get<std::vector<std::string>>(
          op->GetNullableAttr("recv_varnames"));
      auto epmap =
          boost::get<std::vector<std::string>>(op->GetNullableAttr("epmap"));
      auto trainer_id = boost::get<int>(op->GetNullableAttr("trainer_id"));
      recv_varname_to_ctx[recv_var_name] = operators::distributed::RpcContext(
          recv_var_name, recv_varnames, epmap, {}, trainer_id);
    }
  }

  // init communicator here
  if (send_varname_to_ctx.size() == 0 && recv_varname_to_ctx.size() == 0) {
    LOG(WARNING) << "no var need to send and recv!!";
  }

  operators::distributed::AsyncCommunicator::InitImpl(
      send_varname_to_ctx, recv_varname_to_ctx, param_scope);
}

AsyncCommunicator::~AsyncCommunicator() {
  if (FLAGS_v >= 3) {
    std::string msg("~Communicator");
    fwrite(msg.c_str(), msg.length(), 1, stdout);
  }
  running_ = false;
  if (send_thread_) send_thread_->join();
  if (recv_thread_) recv_thread_->join();
  if (FLAGS_v >= 3) {
    std::string msg("~Communicator done");
    fwrite(msg.c_str(), msg.length(), 1, stdout);
  }
}

void AsyncCommunicator::SendThread() {
  VLOG(3) << "SendThread start!";
  while (running_) {
    std::vector<std::future<void>> task_futures;
    task_futures.reserve(send_varname_to_ctx_.size());
    VLOG(3) << "run send graph";
    auto before_run_send_graph = GetCurrentUS();
    for (auto &iter : send_varname_to_queue_) {
      auto &var_name = iter.first;
      auto &var_queue = iter.second;
      if (var_queue->Size() > 0) {
        auto send_task = [this, &var_name, &var_queue] {
          VLOG(3) << var_name << " merge and send";
          std::vector<std::shared_ptr<Variable>> vars;
          size_t merged_var_num = 0;
          size_t wait_times = 0;
          while (merged_var_num < FLAGS_communicator_max_merge_var_num) {
            if (var_queue->Size() == 0) {
              VLOG(3) << "wait_times -> " << wait_times;
              if (wait_times >= FLAGS_communicator_send_wait_times) {
                break;
              }
              std::this_thread::sleep_for(std::chrono::milliseconds(10));
              wait_times++;
              continue;
            } else {
              wait_times = 0;

              vars.push_back(var_queue->Pop());
              // only count the send number of the first var
              if (var_name == send_varname_to_queue_.begin()->first) {
                grad_num_.fetch_add(1, std::memory_order_relaxed);
              }
              merged_var_num++;
            }
          }
          auto before_merge = GetCurrentUS();
          auto &ctx = send_varname_to_ctx_.at(var_name);
          if (ctx.use_send_handler) {
            MergeVars<float>(var_name, vars, send_scope_.get(), ctx.merge_add);
          } else {
            MergeVars<int64_t>(var_name, vars, send_scope_.get(),
                               ctx.merge_add);
          }
          auto after_merge = GetCurrentUS();
          VLOG(3) << "merge " << merged_var_num << " " << var_name
                  << " use time " << after_merge - before_merge;
          auto send_functor = distributed::ParameterSend<float>();
          if (!FLAGS_communicator_fake_rpc) {
            send_functor(ctx, *send_scope_, true, 1);
          }
          auto after_send = GetCurrentUS();
          VLOG(3) << "send " << var_name << " use time "
                  << after_send - after_merge;
        };
        task_futures.emplace_back(
            send_threadpool_->enqueue(std::move(send_task)));
      } else {
        VLOG(4) << var_name << " queue empty";
      }
    }
    for (auto &task_f : task_futures) {
      task_f.wait();
    }
    auto after_run_send_graph = GetCurrentUS();

    VLOG(3) << "run send graph use time "
            << after_run_send_graph - before_run_send_graph;
    Recv();
  }
  VLOG(0) << "communicator stopped, send thread exit";
}

void AsyncCommunicator::RecvThread() {
  VLOG(3) << "RecvThread start!";
  while (running_) {
    auto grad_num = grad_num_.load();
    if (grad_num > FLAGS_communicator_min_send_grad_num_before_recv) {
      VLOG(1) << "current grad num " << grad_num;
      RecvAll();
      grad_num_.store(0);
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  VLOG(0) << "communicator stopped, recv thread exit";
}

void AsyncCommunicator::Send(const std::string &var_name,
                             const framework::Scope &scope) {
  VLOG(3) << "communicator send " << var_name;
  // push var into send queue by var_name
  auto *grad_var = scope.FindVar(var_name);
  PADDLE_ENFORCE(grad_var->IsInitialized(), "grad var should be inited");
  if (grad_var->IsType<framework::SelectedRows>() &&
      !FLAGS_communicator_merge_sparse_grad) {
    auto send_functor = distributed::ParameterSend<float>();
    auto &ctx = send_varname_to_ctx_.at(var_name);
    if (!FLAGS_communicator_fake_rpc) {
      send_functor(ctx, scope, true, 1);
    }
  } else {
    auto tmp_grad_var = std::make_shared<Variable>();
    framework::CopyVariable(*grad_var, tmp_grad_var.get());
    auto &queue = send_varname_to_queue_.at(var_name);
    VLOG(3) << "send " << var_name << " queue size " << queue->Size();
    queue->Push(tmp_grad_var);
  }
}

void AsyncCommunicator::Recv() {
  if (FLAGS_communicator_independent_recv_thread) {
    return;
  }

  auto grad_num = grad_num_.load();
  if (grad_num > 0) {
    RecvAll();
    grad_num_.store(0);
  } else {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

void AsyncCommunicator::RecvAll() {
  VLOG(3) << "parallel run recv graph";
  if (!running_) return;
  auto before_send = GetCurrentUS();
  std::vector<std::future<void>> task_futures;
  task_futures.reserve(recv_varname_to_ctx_.size());
  for (auto &iter : recv_varname_to_ctx_) {
    auto recv_task = [this, &iter] {
      auto &var_name = iter.first;
      VLOG(4) << "recv var " << var_name;
      auto recv_functor = distributed::ParameterRecv<float>();
      if (!FLAGS_communicator_fake_rpc) {
        recv_functor(iter.second, *recv_scope_);
      }
    };
    task_futures.emplace_back(recv_threadpool_->enqueue(std::move(recv_task)));
  }
  for (auto &task : task_futures) {
    task.wait();
  }
  auto after_recv = GetCurrentUS();
  VLOG(1) << "run recv graph use time " << after_recv - before_send;
}

void AsyncCommunicator::Start() {
  VLOG(0) << "Communicator start";
  if (!communicator_) {
    VLOG(0) << "Communicator is not inited, do nothing";
  } else {
    VLOG(1) << "start send thread and recv thread";
    running_ = true;
    // start send and recv thread
    send_thread_.reset(
        new std::thread(std::bind(&AsyncCommunicator::SendThread, this)));
    if (FLAGS_communicator_independent_recv_thread) {
      recv_thread_.reset(
          new std::thread(std::bind(&AsyncCommunicator::RecvThread, this)));
    }
  }
}

void AsyncCommunicator::Stop() {
  VLOG(0) << "Communicator stop";
  running_ = false;
  if (!communicator_) {
    VLOG(0) << "Communicator is not inited, do nothing";
  } else {
    if (send_thread_) {
      VLOG(1) << "stop send thread";
      send_thread_->join();
      send_thread_.reset(nullptr);
    }
    if (recv_thread_) {
      VLOG(1) << "stop recv thread";
      recv_thread_->join();
      recv_thread_.reset(nullptr);
    }
  }
  VLOG(0) << "Communicator stop done";
}

void AsyncCommunicator::Send(const std::vector<std::string> &sparse_var_names,
                             const std::vector<std::string> &sparse_var_tables,
                             const framework::Scope &scope) {}

void AsyncCommunicator::InitImpl(
    const paddle::framework::ProgramDesc &program, Scope *param_scope,
    std::map<std::string, std::map<std::string, std::vector<std::string>>>
        &vars_info,
    const int &trainers, const int &geo_need_push_nums) {}

GeoSgdCommunicator::~GeoSgdCommunicator() {
  if (FLAGS_v >= 3) {
    std::string msg("~Geo Sgd Communicator");
    fwrite(msg.c_str(), msg.length(), 1, stdout);
  }
  running_ = false;
  if (send_thread_) send_thread_->join();
  if (FLAGS_v >= 3) {
    std::string msg("~Geo Sgd Communicator done");
    fwrite(msg.c_str(), msg.length(), 1, stdout);
  }
}

void GeoSgdCommunicator::InitImpl(
    const paddle::framework::ProgramDesc &program, Scope *training_scope,
    std::map<std::string, std::map<std::string, std::vector<std::string>>>
        &vars_info,
    const int &trainers, const int &geo_need_push_nums) {
  training_scope_ = std::move(training_scope);
  trainer_nums_ = std::move(trainers);
  geo_need_push_nums_ = std::move(geo_need_push_nums);

  // get all send information from graph, build vars_to_send
  VLOG(0) << "communicator_independent_recv_thread: "
          << FLAGS_communicator_independent_recv_thread;
  VLOG(0) << "communicator_send_queue_size: "
          << FLAGS_communicator_send_queue_size;
  VLOG(0) << "communicator_min_send_grad_num_before_recv: "
          << FLAGS_communicator_min_send_grad_num_before_recv;
  VLOG(0) << "communicator_thread_pool_size: "
          << FLAGS_communicator_thread_pool_size;
  VLOG(0) << "communicator_send_wait_times: "
          << FLAGS_communicator_send_wait_times;
  VLOG(0) << "communicator_max_merge_var_num: "
          << FLAGS_communicator_max_merge_var_num;
  VLOG(0) << "communicator_fake_rpc: " << FLAGS_communicator_fake_rpc;
  VLOG(0) << "communicator_merge_sparse_grad: "
          << FLAGS_communicator_merge_sparse_grad;
  VLOG(0) << "Trainer nums: " << trainer_nums_;
  VLOG(0) << "geo_sgd_push_before_local_train_nums: " << geo_need_push_nums_;
  VLOG(0) << "communicator_merge_sparse_bucket "
          << FLAGS_communicator_merge_sparse_bucket;

  // process var info from transpiler
  for (auto &iter : vars_info) {
    // change var name in delta scope: "var" -> "var.delta"
    std::string var_name = iter.first;
    std::string send_var_name = VarToDeltaVar(var_name);
    std::vector<std::string> vars_names = iter.second["var_names"];
    std::vector<std::string> send_var_names;
    for (auto origin_var_name : vars_names) {
      send_var_names.push_back(VarToDeltaVar(origin_var_name));
    }

    // get vars section for split
    std::vector<std::string> vars_sections_str = iter.second["sections"];
    std::vector<int64_t> vars_sections_int = {};
    for (std::string str : vars_sections_str) {
      int64_t str2i = std::stol(str.c_str());
      vars_sections_int.push_back(str2i);
    }

    std::vector<std::string> vars_epmap = iter.second["epmap"];

    // record var is sparse or not
    bool is_sparse = iter.second["is_sparse"].front() == std::string("True");
    var_list_[var_name] = is_sparse;

    send_varname_to_ctx_[send_var_name] = operators::distributed::RpcContext(
        send_var_name, send_var_names, vars_epmap, vars_sections_int, 0);
    recv_varname_to_ctx_[var_name] = operators::distributed::RpcContext(
        var_name, vars_names, vars_epmap, vars_sections_int, 0);

    // record sparse section
    if (is_sparse) {
      need_thread_nums_ +=
          send_varname_to_ctx_[send_var_name].height_sections.size();
      absolute_section_[var_name] = operators::ToAbsoluteSection(
          send_varname_to_ctx_[send_var_name].height_sections);
    }
  }

  if (send_varname_to_ctx_.size() == 0 && recv_varname_to_ctx_.size() == 0) {
    LOG(WARNING) << "no var need to send and recv!!";
  }

  send_threadpool_.reset(new ::ThreadPool(FLAGS_communicator_thread_pool_size));
  need_push_queue_ =
      std::make_shared<BlockingQueue<std::shared_ptr<SparseIdsMap>>>(
          geo_need_push_nums);
  delta_scope_.reset(new Scope());
  old_scope_.reset(new Scope());
  pserver_scope_.reset(new Scope());
}

void GeoSgdCommunicator::Start() {
  VLOG(0) << "Geo Sgd Communicator start";
  if (!communicator_) {
    VLOG(0) << "Geo Sgd Communicator is not inited, do nothing";
  } else {
    VLOG(0) << "start send thread ";
    running_ = true;
    // start send and recv thread
    send_thread_.reset(
        new std::thread(std::bind(&GeoSgdCommunicator::SendThread, this)));
  }
}

void GeoSgdCommunicator::Stop() {
  VLOG(0) << "Geo Sgd Communicator stop";
  running_ = false;
  if (!communicator_) {
    VLOG(0) << "Geo Sgd Communicator is not inited, do nothing";
  } else {
    if (send_thread_) {
      VLOG(1) << "stop send thread";
      send_thread_->join();
      send_thread_.reset(nullptr);
    }
  }
  VLOG(0) << "Geo Sgd Communicator stop done";
}

void GeoSgdCommunicator::Send(const std::string &var_name,
                              const framework::Scope &scope) {
  // when execute trainer startup program, recv parameter from pserver
  // training_scope & pserver_scope param will copy it
  if (var_name == "param_init") {
    for (auto &iter : var_list_) {
      // For sparse param, old_scope store LoDTensor,
      // pserver_scope store SelectedRows.
      auto local_var_name = iter.first;
      if (var_list_[local_var_name] == true) {
        GeoSgdSparseParamInit(training_scope_, pserver_scope_.get(),
                              local_var_name);
      } else {
        GeoSgdDenseParamInit(training_scope_, pserver_scope_.get(),
                             local_var_name);
      }
      GeoSgdDenseParamInit(training_scope_, old_scope_.get(), local_var_name);
    }
  }
}

void GeoSgdCommunicator::Send(const std::vector<std::string> &sparse_var_names,
                              const std::vector<std::string> &sparse_var_tables,
                              const framework::Scope &scope) {
  // SparseIdsMap = std::unordered_map<std::string,std::unordered_set<int64_t>>
  std::shared_ptr<SparseIdsMap> ids_table = std::make_shared<SparseIdsMap>();
  auto before_run_send = GetCurrentUS();
  for (size_t i = 0; i < sparse_var_tables.size(); i++) {
    if (ids_table->find(sparse_var_tables[i]) == ids_table->end()) {
      // create empty set for new sparse var
      auto splited_var_nums =
          recv_varname_to_ctx_[sparse_var_tables[i]].splited_var_names.size();
      ids_table->insert(
          std::pair<std::string, std::vector<std::unordered_set<int64_t>>>(
              sparse_var_tables[i],
              std::vector<std::unordered_set<int64_t>>{splited_var_nums}));
    }
    auto *var = scope.FindVar(sparse_var_names[i]);
    auto var_tensor = var->Get<framework::LoDTensor>();
    int element_number = var_tensor.numel();
    int *var_mutable_data = var_tensor.mutable_data<int>(var_tensor.place());
    // insert ids which has not been record
    for (size_t j = 0; j < element_number; j++) {
      auto ep_idx = GetSectionIndex(var_mutable_data[j],
                                    absolute_section_[sparse_var_tables[i]]);
      ids_table->at(sparse_var_tables[i])[ep_idx].insert(var_mutable_data[j]);
      VLOG(4) << "Sparse var " << sparse_var_tables[i] << " insert "
              << var_mutable_data[j];
    }
  }
  need_push_queue_->Push(ids_table);
  auto after_run_send = GetCurrentUS();
  VLOG(3) << "run send_op use time " << after_run_send - before_run_send;
}

void GeoSgdCommunicator::SendThread() {
  VLOG(0) << "SendThread start!";
  auto before_run_training = GetCurrentUS();

  while (running_) {
    std::vector<std::future<void>> task_futures;
    task_futures.reserve(send_varname_to_ctx_.size());

    size_t wait_times = 0;
    while (ids_send_vec_.size() < geo_need_push_nums_) {
      VLOG(4) << "ids_send_vec_ Size: " << ids_send_vec_.size();
      if (need_push_queue_->Size() > 0) {
        wait_times = 0;
        ids_send_vec_.push_back(*(need_push_queue_->Pop()));
        VLOG(4) << "ids_send_vec_ pushed";
      } else if (need_push_queue_->Size() == 0) {
        VLOG(3) << "wait_times -> " << wait_times;
        if (wait_times >= FLAGS_communicator_send_wait_times) {
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        wait_times++;
        continue;
      }
    }

    if (ids_send_vec_.size() >= geo_need_push_nums_) {
      auto after_run_training = GetCurrentUS();
      VLOG(3) << "run Training use time "
              << after_run_training - before_run_training;
      before_run_training = GetCurrentUS();
      VLOG(3) << "Start send after get need_push_num";

      for (auto &iter : send_varname_to_ctx_) {
        auto &var_name = iter.first;
        if (var_list_[DeltaVarToVar(var_name)] == true) {
          // sparse var: merge->send->recv
          for (auto &splited_var_name : iter.second.splited_var_names) {
            auto send_task = [this, &var_name, &splited_var_name] {
              auto before_run_geo = GetCurrentUS();
              auto ids_set =
                  SparseIdsMerge(ids_send_vec_, var_name, splited_var_name);
              SendUpdateSparseVars(var_name, splited_var_name, ids_set);
              RecvUpdateSparseVars(var_name, splited_var_name);
              auto after_run_geo = GetCurrentUS();
              VLOG(1) << "run GEO-SGD var " << splited_var_name << " use time "
                      << after_run_geo - before_run_geo;
            };
            task_futures.emplace_back(
                send_threadpool_->enqueue(std::move(send_task)));
          }
        } else {
          auto send_task = [this, &var_name] {
            auto before_run_geo = GetCurrentUS();
            SendUpdateDenseVars(var_name);
            RecvUpdateDenseVars(var_name);
            auto after_run_geo = GetCurrentUS();
            VLOG(3) << "run GEO-SGD var " << var_name << " use time "
                    << after_run_geo - before_run_geo;
          };
          task_futures.emplace_back(
              send_threadpool_->enqueue(std::move(send_task)));
        }
      }
      for (auto &task_f : task_futures) {
        task_f.wait();
      }
      ids_send_vec_.clear();
    }
  }
}

std::unordered_set<int64_t> GeoSgdCommunicator::SparseIdsMerge(
    const std::vector<SparseIdsMap> &ids_send_vec, const std::string &var_name,
    const std::string &splited_var_name) {
  // every batch has some sparse id, merge them into one unoredered_set
  VLOG(3) << "Sparse Ids merge var: " << var_name
          << " splited var: " << splited_var_name;
  auto before_run_ids_merge_ = GetCurrentUS();
  auto origin_var_name = DeltaVarToVar(var_name);
  auto splited_var_index = GetSplitedVarIndex(var_name, splited_var_name);
  std::unordered_set<int64_t> ids_set;

  for (auto ids_map : ids_send_vec) {
    for (auto id : ids_map[origin_var_name][splited_var_index]) {
      ids_set.insert(id);
    }
  }
  auto after_run_ids_merge_ = GetCurrentUS();
  VLOG(3) << "run SparseIdsMerge " << splited_var_name << " has nums "
          << ids_set.size() << " use time "
          << after_run_ids_merge_ - before_run_ids_merge_;
  return ids_set;
}

void GeoSgdCommunicator::SendUpdateDenseVars(const std::string &var_name) {
  // calc var_delata = (var_training - var_old)/trainer_nums
  // calc var_old += var_delta
  // var_name: param.delta
  auto origin_var_name = DeltaVarToVar(var_name);
  auto before_run_send_dense = GetCurrentUS();

  auto *var_x = training_scope_->FindVar(origin_var_name);
  auto var_x_tensor = var_x->Get<framework::LoDTensor>();

  auto *var_y = old_scope_->FindVar(origin_var_name);
  auto var_y_tensor = var_y->Get<framework::LoDTensor>();

  auto cpu_ctx = paddle::platform::CPUDeviceContext();
  auto dims = var_x_tensor.dims();

  // create temp var for sub
  auto *var_y_sub = old_scope_->Var(var_name);
  framework::CopyVariable(*var_y, var_y_sub);
  auto var_y_sub_tensor = var_y_sub->Get<framework::LoDTensor>();

  // create delta var in delta scope
  auto *var_z = delta_scope_->Var(var_name);
  auto *var_z_tensor = var_z->GetMutable<framework::LoDTensor>();
  var_z_tensor->mutable_data<float>(dims, var_x_tensor.place());
  var_z_tensor->set_lod(var_x_tensor.lod());

  math::SetConstant<paddle::platform::CPUDeviceContext, float> constant_functor;
  constant_functor(cpu_ctx, var_z_tensor, static_cast<float>(0));

  // calc sub = var_training - var_old
  auto blas = math::GetBlas<paddle::platform::CPUDeviceContext, float>(cpu_ctx);
  blas.SCAL(var_y_sub_tensor.numel(), -1,
            var_y_sub_tensor.mutable_data<float>(var_y_sub_tensor.place()));
  blas.VADD(var_x_tensor.numel(),
            var_x_tensor.mutable_data<float>(var_x_tensor.place()),
            var_y_sub_tensor.mutable_data<float>(var_y_sub_tensor.place()),
            var_z_tensor->mutable_data<float>(var_z_tensor->place()));

  // calc var_delta = sub / trainer_nums
  float trainer_param = 1.0 / static_cast<float>(trainer_nums_);
  blas.SCAL(var_z_tensor->numel(), trainer_param,
            var_z_tensor->mutable_data<float>(var_z_tensor->place()));

  // calc var_old += var_delta
  blas.VADD(var_y_tensor.numel(),
            var_y_tensor.mutable_data<float>(var_y_tensor.place()),
            var_z_tensor->mutable_data<float>(var_z_tensor->place()),
            var_y_tensor.mutable_data<float>(var_y_tensor.place()));

  auto after_run_send_dense = GetCurrentUS();
  VLOG(3) << "run send update dense var " << var_name << " use time "
          << after_run_send_dense - before_run_send_dense;

  auto send_functor = distributed::ParameterSend<float>();
  auto &ctx = send_varname_to_ctx_.at(var_name);

  auto before_send_dense = GetCurrentUS();
  send_functor(ctx, *delta_scope_.get(), true, 1);
  auto after_send_denxe = GetCurrentUS();
  VLOG(3) << "send " << var_name << " use time "
          << after_send_denxe - before_send_dense;
}

void GeoSgdCommunicator::SendUpdateSparseVars(
    const std::string &var_name, const std::string &splited_var_name,
    const std::unordered_set<int64_t> &ids_table) {
  // calc var_delata = (var_training - var_old)/trainer_nums
  // calc var_old += var_delta
  // var_name: param.delta, splited_var_name: param.block0.delta
  // origin_var_name: param
  auto before_run_send_sparse = GetCurrentUS();

  auto ids_num = ids_table.size();
  VLOG(4) << "Sparse Ids nums is : " << ids_num;
  auto origin_var_name = DeltaVarToVar(var_name);

  auto *var_x = training_scope_->FindVar(origin_var_name);
  auto var_x_tensor = var_x->Get<framework::LoDTensor>();

  auto *var_y = old_scope_.get()->FindVar(origin_var_name);
  auto var_y_tensor = var_y->Get<framework::LoDTensor>();

  auto dims = var_x_tensor.dims();
  auto row_numel = dims[1];

  float *x_value = var_x_tensor.mutable_data<float>(var_x_tensor.place());
  float *y_value = var_y_tensor.mutable_data<float>(var_y_tensor.place());

  auto *var_z = delta_scope_->Var(splited_var_name);
  auto *var_z_select_rows = var_z->GetMutable<framework::SelectedRows>();
  auto *var_z_value = var_z_select_rows->mutable_value();
  var_z_value->Resize({static_cast<int64_t>(ids_num), row_numel});
  auto *z_value = var_z_value->mutable_data<float>(var_x_tensor.place());

  std::vector<int64_t> new_rows;
  new_rows.insert(new_rows.begin(), ids_table.begin(), ids_table.end());

  auto cpu_ctx = paddle::platform::CPUDeviceContext();
  auto blas = math::GetBlas<paddle::platform::CPUDeviceContext, float>(cpu_ctx);
  float avg = 1 / static_cast<float>(trainer_nums_);
  for (int y = 0; y < new_rows.size(); y++) {
    auto ids = new_rows[y];

    float *x_val = x_value + ids * row_numel;
    float *y_val = y_value + ids * row_numel;
    float *z_val = z_value + y * row_numel;

    std::vector<float> row_delta(row_numel, 0);
    VSUB<float>(row_numel, x_val, y_val, row_delta.data());
    blas.SCAL(row_numel, avg, row_delta.data());
    blas.VADD(row_numel, row_delta.data(), y_val, y_val);
    blas.VCOPY(row_numel, row_delta.data(), z_val);
  }

  auto after_run_send_sparse = GetCurrentUS();
  VLOG(3) << "run send update sparse var " << splited_var_name << " use time "
          << after_run_send_sparse - before_run_send_sparse;

  auto splited_var_index = GetSplitedVarIndex(var_name, splited_var_name);
  std::vector<int64_t> send_rows;
  send_rows.reserve(new_rows.size());
  for (auto idx : new_rows) {
    send_rows.push_back(idx -
                        absolute_section_[origin_var_name][splited_var_index]);
  }
  var_z_select_rows->set_rows(send_rows);
  var_z_select_rows->set_height(
      send_varname_to_ctx_[var_name].height_sections[splited_var_index]);

  auto before_send_sparse = GetCurrentUS();
  RpcSend(var_name, splited_var_name, splited_var_index);
  auto after_send_sparse = GetCurrentUS();
  VLOG(3) << "send " << splited_var_name << " has nums " << new_rows.size()
          << " use time " << after_send_sparse - before_send_sparse;
}

void GeoSgdCommunicator::RecvUpdateDenseVars(const std::string &var_name) {
  // calc var_training += var_pserver - var_old
  // calc var_old = var_pserver
  // var_name: param.delta

  // step1: recv dense var from pserver
  auto origin_var_name = DeltaVarToVar(var_name);

  auto before_run_recv = GetCurrentUS();
  auto recv_functor = distributed::ParameterRecv<float>();
  recv_functor(recv_varname_to_ctx_[origin_var_name], *pserver_scope_.get());
  auto after_run_recv = GetCurrentUS();
  VLOG(3) << "recv var " << origin_var_name << " use time "
          << after_run_recv - before_run_recv;

  // step2: update dense var
  auto before_run_update = GetCurrentUS();
  auto *var_x = training_scope_->FindVar(origin_var_name);
  auto var_x_tensor = var_x->Get<framework::LoDTensor>();

  auto *var_y = old_scope_->FindVar(origin_var_name);
  auto var_y_tensor = var_y->Get<framework::LoDTensor>();

  auto *var_y_sub = old_scope_->Var(origin_var_name);
  framework::CopyVariable(*var_y, var_y_sub);
  auto var_y_sub_tensor = var_y_sub->Get<framework::LoDTensor>();

  auto *var_z = pserver_scope_.get()->FindVar(origin_var_name);
  auto var_z_tensor = var_z->Get<framework::LoDTensor>();

  auto cpu_ctx = paddle::platform::CPUDeviceContext();
  auto blas = math::GetBlas<paddle::platform::CPUDeviceContext, float>(cpu_ctx);
  // calc sub = pserver - old
  blas.SCAL(var_y_sub_tensor.numel(), -1,
            var_y_sub_tensor.mutable_data<float>(var_y_sub_tensor.place()));
  blas.VADD(var_y_tensor.numel(),
            var_y_sub_tensor.mutable_data<float>(var_y_sub_tensor.place()),
            var_z_tensor.mutable_data<float>(var_z_tensor.place()),
            var_y_sub_tensor.mutable_data<float>(var_y_sub_tensor.place()));
  // calc recv += sub
  blas.VADD(var_x_tensor.numel(),
            var_x_tensor.mutable_data<float>(var_x_tensor.place()),
            var_y_sub_tensor.mutable_data<float>(var_y_sub_tensor.place()),
            var_x_tensor.mutable_data<float>(var_x_tensor.place()));
  // calc old = pserver
  framework::CopyVariable(*var_z, var_y);
  auto after_run_update = GetCurrentUS();
  VLOG(3) << "dese var update " << origin_var_name << " use time "
          << after_run_update - before_run_update;
}

void GeoSgdCommunicator::RecvUpdateSparseVars(
    const std::string &var_name, const std::string &splited_var_name) {
  // step 1: recv splited var from pserver
  auto splited_var_index = GetSplitedVarIndex(var_name, splited_var_name);
  auto origin_var_name = DeltaVarToVar(var_name);
  auto origin_splited_var_name = DeltaVarToVar(splited_var_name);

  auto before_run_recv = GetCurrentUS();
  RpcRecv(origin_var_name, origin_splited_var_name, splited_var_index);
  auto after_run_recv = GetCurrentUS();
  VLOG(3) << "recv var " << origin_splited_var_name << " use time "
          << after_run_recv - before_run_recv;

  // step 2: update sparse var
  auto before_run_update = GetCurrentUS();
  auto *var_x = training_scope_->FindVar(origin_var_name);
  auto var_x_tensor = var_x->Get<framework::LoDTensor>();
  auto dims = var_x_tensor.dims();
  float *x_value = var_x_tensor.mutable_data<float>(var_x_tensor.place());

  auto *var_y = old_scope_->FindVar(origin_var_name);
  auto var_y_tensor = var_y->Get<framework::LoDTensor>();
  float *y_value = var_y_tensor.mutable_data<float>(var_y_tensor.place());

  auto *var_z = pserver_scope_.get()->FindVar(origin_splited_var_name);
  auto var_z_slr = var_z->GetMutable<framework::SelectedRows>();
  auto row_size = var_z_slr->rows().size();

  std::vector<int64_t> new_rows;
  new_rows.reserve(row_size);

  for (auto ids : var_z_slr->rows()) {
    new_rows.push_back(ids +
                       absolute_section_[origin_var_name][splited_var_index]);
  }

  auto *new_value = var_z_slr->mutable_value();
  auto row_numel = dims[1];
  auto *z_value = new_value->mutable_data<float>(var_x_tensor.place());

  auto cpu_ctx = paddle::platform::CPUDeviceContext();
  auto blas = math::GetBlas<paddle::platform::CPUDeviceContext, float>(cpu_ctx);
  for (int y = 0; y < new_rows.size(); y++) {
    std::vector<float> row_delta(row_numel, 0);

    auto ids = new_rows[y];

    float *x_val = x_value + ids * row_numel;
    float *y_val = y_value + ids * row_numel;
    float *z_val = z_value + y * row_numel;

    VSUB(row_numel, z_val, y_val, row_delta.data());
    blas.VADD(row_numel, row_delta.data(), x_val, x_val);
    blas.VCOPY(row_numel, z_val, y_val);
  }

  auto after_run_update = GetCurrentUS();
  VLOG(3) << "sparse var recv update " << origin_splited_var_name << " has num "
          << new_rows.size() << " use time "
          << after_run_update - before_run_update;
}

void GeoSgdCommunicator::GeoSgdSparseParamInit(framework::Scope *scope_x,
                                               framework::Scope *scope_y,
                                               const std::string var_name) {
  // create selectedrows var from lodtensor var info
  auto *var_x = scope_x->Var(var_name);
  auto *var_y = scope_y->Var(var_name);

  auto var_x_tensor = var_x->Get<framework::LoDTensor>();
  auto *var_y_select_rows = var_y->GetMutable<framework::SelectedRows>();

  auto dims = var_x_tensor.dims();
  auto rows = dims[0];
  auto row_numel = dims[1];

  var_y_select_rows->set_height(rows);
  std::vector<int64_t> new_rows{};
  var_y_select_rows->set_rows(new_rows);
  auto *var_y_value = var_y_select_rows->mutable_value();
  var_y_value->Resize({rows, row_numel});
  var_y_value->mutable_data<float>(var_x_tensor.place());
}

void GeoSgdCommunicator::GeoSgdDenseParamInit(framework::Scope *scope_x,
                                              framework::Scope *scope_y,
                                              const std::string var_name) {
  auto *var_x = scope_x->Var(var_name);
  auto *var_y = scope_y->Var(var_name);
  framework::CopyVariable(*var_x, var_y);
}

void GeoSgdCommunicator::RpcSend(const std::string &origin_var_name,
                                 const std::string &splited_var_name,
                                 const size_t &splited_var_index) {
  auto trainer_id = send_varname_to_ctx_[origin_var_name].trainer_id;
  auto endpoint =
      send_varname_to_ctx_[origin_var_name].epmap[splited_var_index];

  platform::DeviceContextPool &pool = platform::DeviceContextPool::Instance();
  auto &cpu_ctx_send = *pool.Get(platform::CPUPlace());
  distributed::RPCClient *rpc_client =
      distributed::RPCClient::GetInstance<RPCCLIENT_T>(trainer_id);

  rpc_client->AsyncSendVar(endpoint, cpu_ctx_send, *delta_scope_.get(),
                           splited_var_name);
}

void GeoSgdCommunicator::RpcRecv(const std::string &var_name,
                                 const std::string &splited_var_name,
                                 const size_t &splited_var_index) {
  auto train_id = recv_varname_to_ctx_[var_name].trainer_id;
  auto endpoint = recv_varname_to_ctx_[var_name].epmap[splited_var_index];
  platform::DeviceContextPool &pool = platform::DeviceContextPool::Instance();
  auto &cpu_ctx_recv = *pool.Get(platform::CPUPlace());
  distributed::RPCClient *rpc_client =
      distributed::RPCClient::GetInstance<RPCCLIENT_T>(train_id);

  pserver_scope_->Var(splited_var_name);
  rpc_client->AsyncGetVar(endpoint, cpu_ctx_recv, *pserver_scope_.get(),
                          splited_var_name, splited_var_name, splited_var_name);
}

void GeoSgdCommunicator::Recv() {}

void GeoSgdCommunicator::InitImpl(const RpcCtxMap &send_varname_to_ctx,
                                  const RpcCtxMap &recv_varname_to_ctx,
                                  Scope *recv_scope) {}

void GeoSgdCommunicator::InitImpl(const paddle::framework::ProgramDesc &program,
                                  Scope *recv_scope) {}

}  // namespace distributed
}  // namespace operators
}  // namespace paddle
