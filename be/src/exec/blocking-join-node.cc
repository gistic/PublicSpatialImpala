// Copyright 2013 Cloudera Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "exec/blocking-join-node.h"

#include <sstream>

#include "exprs/expr.h"
#include "runtime/row-batch.h"
#include "runtime/runtime-state.h"
#include "util/debug-util.h"
#include "util/runtime-profile.h"

#include "gen-cpp/PlanNodes_types.h"

using namespace boost;
using namespace impala;
using namespace llvm;
using namespace std;

const char* BlockingJoinNode::LLVM_CLASS_NAME = "class.impala::BlockingJoinNode";

BlockingJoinNode::BlockingJoinNode(const string& node_name, const TJoinOp::type join_op,
    ObjectPool* pool, const TPlanNode& tnode, const DescriptorTbl& descs)
  : ExecNode(pool, tnode, descs),
    node_name_(node_name),
    join_op_(join_op),
    eos_(false),
    probe_side_eos_(false),
    can_add_probe_filters_(false) {
}

Status BlockingJoinNode::Init(const TPlanNode& tnode) {
  RETURN_IF_ERROR(ExecNode::Init(tnode));
  return Status::OK;
}

BlockingJoinNode::~BlockingJoinNode() {
  // probe_batch_ must be cleaned up in Close() to ensure proper resource freeing.
  DCHECK(probe_batch_ == NULL);
}

Status BlockingJoinNode::Prepare(RuntimeState* state) {
  SCOPED_TIMER(runtime_profile_->total_time_counter());
  RETURN_IF_ERROR(ExecNode::Prepare(state));

  build_pool_.reset(new MemPool(mem_tracker()));
  build_timer_ = ADD_TIMER(runtime_profile(), "BuildTime");
  probe_timer_ = ADD_TIMER(runtime_profile(), "ProbeTime");
  build_row_counter_ = ADD_COUNTER(runtime_profile(), "BuildRows", TCounterType::UNIT);
  probe_row_counter_ = ADD_COUNTER(runtime_profile(), "ProbeRows", TCounterType::UNIT);

  // Validate the row desc layout is what we expect. The join node returns a row
  // that is a concatenation of the left side and build side row desc's. For example if
  // the probe row had 1 tuple and the build row had 2, the resulting row desc
  // of the join node would have 3 tuples with:
  //   result[0] = left[0]
  //   result[1] = build[0]
  //   result[2] = build[1]
  // The current join node implementation relies on this property to enable some
  // optimizations.
  int num_left_tuples = child(0)->row_desc().tuple_descriptors().size();
  int num_build_tuples = child(1)->row_desc().tuple_descriptors().size();
  for (int i = 0; i < num_left_tuples; ++i) {
    TupleDescriptor* desc = child(0)->row_desc().tuple_descriptors()[i];
    DCHECK_EQ(i, row_desc().GetTupleIdx(desc->id()));
  }
  for (int i = 0; i < num_build_tuples; ++i) {
    TupleDescriptor* desc = child(1)->row_desc().tuple_descriptors()[i];
    DCHECK_EQ(num_left_tuples + i, row_desc().GetTupleIdx(desc->id()));
  }

  probe_tuple_row_size_ = num_left_tuples * sizeof(Tuple*);
  build_tuple_row_size_ = num_build_tuples * sizeof(Tuple*);

  probe_batch_.reset(
      new RowBatch(child(0)->row_desc(), state->batch_size(), mem_tracker()));
  return Status::OK;
}

void BlockingJoinNode::Close(RuntimeState* state) {
  if (is_closed()) return;
  if (build_pool_.get() != NULL) build_pool_->FreeAll();
  probe_batch_.reset();
  ExecNode::Close(state);
}

void BlockingJoinNode::BuildSideThread(RuntimeState* state, Promise<Status>* status) {
  Status s;
  {
    SCOPED_TIMER(state->total_cpu_timer());
    SCOPED_TIMER(runtime_profile()->total_async_timer());
    s = ConstructBuildSide(state);
  }
  // Release the thread token as soon as possible (before the main thread joins
  // on it).  This way, if we had a chain of 10 joins using 1 additional thread,
  // we'd keep the additional thread busy the whole time.
  state->resource_pool()->ReleaseThreadToken(false);
  status->Set(s);
}

Status BlockingJoinNode::Open(RuntimeState* state) {
  SCOPED_TIMER(runtime_profile_->total_time_counter());
  RETURN_IF_ERROR(ExecNode::Open(state));
  RETURN_IF_CANCELLED(state);
  RETURN_IF_ERROR(state->QueryMaintenance());

  // Kick-off the construction of the build-side table in a separate
  // thread, so that the left child can do any initialisation in parallel.
  // Only do this if we can get a thread token.  Otherwise, do this in the
  // main thread
  Promise<Status> build_side_status;
  if (state->resource_pool()->TryAcquireThreadToken()) {
    AddRuntimeExecOption("Join Build-Side Prepared Asynchronously");
    Thread build_thread(node_name_, "build thread",
        bind(&BlockingJoinNode::BuildSideThread, this, state, &build_side_status));
    if (!state->cgroup().empty()) {
      RETURN_IF_ERROR(state->exec_env()->cgroups_mgr()->AssignThreadToCgroup(
          build_thread, state->cgroup()));
    }
  } else {
    build_side_status.Set(ConstructBuildSide(state));
  }

  // Open the left child so that it may perform any initialisation in parallel.
  // Don't exit even if we see an error, we still need to wait for the build thread
  // to finish.
  Status open_status = child(0)->Open(state);

  // Blocks until ConstructBuildSide has returned, after which the build side structures
  // are fully constructed.
  RETURN_IF_ERROR(build_side_status.Get());
  RETURN_IF_ERROR(open_status);

  // Seed left child in preparation for GetNext().
  while (true) {
    RETURN_IF_ERROR(child(0)->GetNext(state, probe_batch_.get(), &probe_side_eos_));
    COUNTER_ADD(probe_row_counter_, probe_batch_->num_rows());
    probe_batch_pos_ = 0;
    if (probe_batch_->num_rows() == 0) {
      if (probe_side_eos_) {
        RETURN_IF_ERROR(InitGetNext(NULL /* eos */));
        eos_ = true;
        break;
      }
      probe_batch_->Reset();
      continue;
    } else {
      current_probe_row_ = probe_batch_->GetRow(probe_batch_pos_++);
      RETURN_IF_ERROR(InitGetNext(current_probe_row_));
      break;
    }
  }
  return Status::OK;
}

void BlockingJoinNode::DebugString(int indentation_level, stringstream* out) const {
  *out << string(indentation_level * 2, ' ');
  *out << node_name_;
  *out << "(eos=" << (eos_ ? "true" : "false")
       << " probe_batch_pos=" << probe_batch_pos_;
  AddToDebugString(indentation_level, out);
  ExecNode::DebugString(indentation_level, out);
  *out << ")";
}

string BlockingJoinNode::GetLeftChildRowString(TupleRow* row) {
  stringstream out;
  out << "[";
  int num_probe_tuple_rows = child(0)->row_desc().tuple_descriptors().size();
  for (int i = 0; i < row_desc().tuple_descriptors().size(); ++i) {
    if (i != 0) out << " ";
    if (i >= num_probe_tuple_rows) {
      // Build row is not yet populated, print NULL
      out << PrintTuple(NULL, *row_desc().tuple_descriptors()[i]);
    } else {
      out << PrintTuple(row->GetTuple(i), *row_desc().tuple_descriptors()[i]);
    }
  }
  out << "]";
  return out.str();
}

// This function is replaced by codegen
void BlockingJoinNode::CreateOutputRow(TupleRow* out, TupleRow* probe, TupleRow* build) {
  uint8_t* out_ptr = reinterpret_cast<uint8_t*>(out);
  if (probe == NULL) {
    memset(out_ptr, 0, probe_tuple_row_size_);
  } else {
    memcpy(out_ptr, probe, probe_tuple_row_size_);
  }
  if (build == NULL) {
    memset(out_ptr + probe_tuple_row_size_, 0, build_tuple_row_size_);
  } else {
    memcpy(out_ptr + probe_tuple_row_size_, build, build_tuple_row_size_);
  }
}
