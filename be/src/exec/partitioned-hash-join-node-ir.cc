// Copyright 2012 Cloudera Inc.
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

#include "exec/partitioned-hash-join-node.inline.h"

#include "codegen/impala-ir.h"
#include "exec/hash-table.inline.h"
#include "runtime/row-batch.h"

using namespace impala;
using namespace std;

// Wrapper around ExecNode's eval conjuncts with a different function name.
// This lets us distinguish between the join conjuncts vs. non-join conjuncts
// for codegen.
// Note: don't declare this static.  LLVM will pick the fastcc calling convention and
// we will not be able to replace the functions with codegen'd versions.
// TODO: explicitly set the calling convention?
// TODO: investigate using fastcc for all codegen internal functions?
bool IR_NO_INLINE EvalOtherJoinConjuncts(
    ExprContext* const* ctxs, int num_ctxs, TupleRow* row) {
  return ExecNode::EvalConjuncts(ctxs, num_ctxs, row);
}

// CreateOutputRow, EvalOtherJoinConjuncts, and EvalConjuncts are replaced by
// codegen.
template<int const JoinOp>
int PartitionedHashJoinNode::ProcessProbeBatch(
    RowBatch* out_batch, HashTableCtx* ht_ctx) {
  ExprContext* const* join_conjunct_ctxs = &other_join_conjunct_ctxs_[0];
  int num_join_conjuncts = other_join_conjunct_ctxs_.size();
  ExprContext* const* conjunct_ctxs = &conjunct_ctxs_[0];
  int num_conjuncts = conjunct_ctxs_.size();

  DCHECK(!out_batch->AtCapacity());
  TupleRow* out_row = out_batch->GetRow(out_batch->AddRow());
  int max_rows = out_batch->capacity() - out_batch->num_rows();
  int num_rows_added = 0;

  while (probe_batch_pos_ >= 0) {
    if (current_probe_row_ != NULL) {
      while (!hash_tbl_iterator_.AtEnd()) {
        TupleRow* matched_build_row = hash_tbl_iterator_.GetRow();
        DCHECK(matched_build_row != NULL);

        if ((JoinOp == TJoinOp::RIGHT_SEMI_JOIN || JoinOp == TJoinOp::RIGHT_ANTI_JOIN) &&
            hash_tbl_iterator_.matched()) {
          // We have already matched this build row, continue to next match.
          hash_tbl_iterator_.Next<true>(ht_ctx);
          continue;
        }

        CreateOutputRow(out_row, current_probe_row_, matched_build_row);
        if (!EvalOtherJoinConjuncts(join_conjunct_ctxs, num_join_conjuncts, out_row)) {
          hash_tbl_iterator_.Next<true>(ht_ctx);
          continue;
        }

        // At this point the probe is considered matched.
        matched_probe_ = true;
        if (JoinOp == TJoinOp::LEFT_ANTI_JOIN ||
            JoinOp == TJoinOp::NULL_AWARE_LEFT_ANTI_JOIN) {
          // We can safely ignore this probe row for left anti joins.
          hash_tbl_iterator_.reset();
          goto next_row;
        }
        if (JoinOp == TJoinOp::RIGHT_OUTER_JOIN || JoinOp == TJoinOp::RIGHT_SEMI_JOIN ||
            JoinOp == TJoinOp::RIGHT_ANTI_JOIN || JoinOp == TJoinOp::FULL_OUTER_JOIN) {
          // There is a match for this build row, mark it as matched for right/full joins.
          hash_tbl_iterator_.set_matched(true);
        }

        // Update hash_tbl_iterator.
        if (JoinOp == TJoinOp::LEFT_SEMI_JOIN) {
          hash_tbl_iterator_.reset();
        } else {
          hash_tbl_iterator_.Next<true>(ht_ctx);
        }

        if ((JoinOp != TJoinOp::RIGHT_ANTI_JOIN) &&
            ExecNode::EvalConjuncts(conjunct_ctxs, num_conjuncts, out_row)) {
          ++num_rows_added;
          out_row = out_row->next_row(out_batch);
          if (num_rows_added == max_rows) goto end;
        }
      }

      if (join_op_ == TJoinOp::NULL_AWARE_LEFT_ANTI_JOIN && !matched_probe_) {
        // Null aware behavior. The probe row did not match in the hash table so we
        // should interpret the hash table probe as "unknown" if there are nulls on the
        // build size. For those rows, we need to process the remaining join
        // predicates later.
        if (null_aware_partition_->build_rows()->num_rows() != 0) {
          if (num_join_conjuncts == 0) goto next_row;
          if (!null_aware_partition_->probe_rows()->AddRow(current_probe_row_)) return -1;
          goto next_row;
        }
      }

      if ((JoinOp == TJoinOp::LEFT_ANTI_JOIN || JoinOp == TJoinOp::LEFT_OUTER_JOIN ||
           JoinOp == TJoinOp::FULL_OUTER_JOIN ||
           JoinOp == TJoinOp::NULL_AWARE_LEFT_ANTI_JOIN) &&
          !matched_probe_) {
        // No match for this row, we need to output it.
        CreateOutputRow(out_row, current_probe_row_, NULL);
        if (ExecNode::EvalConjuncts(conjunct_ctxs, num_conjuncts, out_row)) {
          ++num_rows_added;
          matched_probe_ = true;
          out_row = out_row->next_row(out_batch);
          if (num_rows_added == max_rows) goto end;
        }
      }
    }

next_row:
    // Must have reached the end of the hash table iterator for the current row before
    // moving to the row.
    DCHECK(hash_tbl_iterator_.AtEnd());

    if (UNLIKELY(probe_batch_pos_ == probe_batch_->num_rows())) {
      // Finished this batch.
      current_probe_row_ = NULL;
      goto end;
    }

    // Establish current_probe_row_ and find its corresponding partition.
    current_probe_row_ = probe_batch_->GetRow(probe_batch_pos_++);
    matched_probe_ = false;
    uint32_t hash;
    if (!ht_ctx->EvalAndHashProbe(current_probe_row_, &hash)) {
      if (join_op_ == TJoinOp::NULL_AWARE_LEFT_ANTI_JOIN) {
        // For NAAJ, we need to treat NULLs on the probe carefully. The logic is:
        // 1. No build rows -> Return this row.
        // 2. Has build rows & no other join predicates, skip row.
        // 3. Has build rows & other join predicates, we need to evaluate against all
        // build rows. First evaluate it against this partition, and if there is not
        // a match, save it to evaluate against other partitions later. If there
        // is a match, the row is skipped.
        if (!non_empty_build_) continue;
        if (num_join_conjuncts == 0) goto next_row;
        if (UNLIKELY(!null_probe_rows_->AddRow(current_probe_row_))) return -1;
        matched_null_probe_.push_back(false);
        goto next_row;
      }
      continue;
    }
    const uint32_t partition_idx = hash >> (32 - NUM_PARTITIONING_BITS);
    if (LIKELY(hash_tbls_[partition_idx] != NULL)) {
      hash_tbl_iterator_= hash_tbls_[partition_idx]->Find(ht_ctx, hash);
    } else {
      Partition* partition = hash_partitions_[partition_idx];
      if (UNLIKELY(partition->is_closed())) {
        // This partition is closed, meaning the build side for this partition was empty.
        DCHECK_EQ(state_, PROCESSING_PROBE);
      } else {
        DCHECK(partition->is_spilled());
        DCHECK(partition->probe_rows() != NULL);
        // This partition is not in memory, spill the probe row.
        if (UNLIKELY(!AppendRow(partition->probe_rows(), current_probe_row_))) return -1;
        continue;
      }
    }
  }

end:
  DCHECK_LE(num_rows_added, max_rows);
  return num_rows_added;
}

int PartitionedHashJoinNode::ProcessProbeBatch(
    const TJoinOp::type join_op, RowBatch* out_batch, HashTableCtx* ht_ctx) {
 switch (join_op) {
    case TJoinOp::INNER_JOIN:
      return ProcessProbeBatch<TJoinOp::INNER_JOIN>(out_batch, ht_ctx);
    case TJoinOp::LEFT_OUTER_JOIN:
      return ProcessProbeBatch<TJoinOp::LEFT_OUTER_JOIN>(out_batch, ht_ctx);
    case TJoinOp::LEFT_SEMI_JOIN:
      return ProcessProbeBatch<TJoinOp::LEFT_SEMI_JOIN>(out_batch, ht_ctx);
    case TJoinOp::LEFT_ANTI_JOIN:
      return ProcessProbeBatch<TJoinOp::LEFT_ANTI_JOIN>(out_batch, ht_ctx);
    case TJoinOp::NULL_AWARE_LEFT_ANTI_JOIN:
      return ProcessProbeBatch<TJoinOp::NULL_AWARE_LEFT_ANTI_JOIN>(out_batch, ht_ctx);
    case TJoinOp::RIGHT_OUTER_JOIN:
      return ProcessProbeBatch<TJoinOp::RIGHT_OUTER_JOIN>(out_batch, ht_ctx);
    case TJoinOp::RIGHT_SEMI_JOIN:
      return ProcessProbeBatch<TJoinOp::RIGHT_SEMI_JOIN>(out_batch, ht_ctx);
    case TJoinOp::RIGHT_ANTI_JOIN:
      return ProcessProbeBatch<TJoinOp::RIGHT_ANTI_JOIN>(out_batch, ht_ctx);
    case TJoinOp::FULL_OUTER_JOIN:
      return ProcessProbeBatch<TJoinOp::FULL_OUTER_JOIN>(out_batch, ht_ctx);
    default:
      DCHECK(false) << "Unknown join type";
      return -1;
  }
}

Status PartitionedHashJoinNode::ProcessBuildBatch(RowBatch* build_batch) {
  for (int i = 0; i < build_batch->num_rows(); ++i) {
    TupleRow* build_row = build_batch->GetRow(i);
    uint32_t hash;
    if (!ht_ctx_->EvalAndHashBuild(build_row, &hash)) {
      if (null_aware_partition_ != NULL) {
        // TODO: remove with codegen/template
        // If we are NULL aware and this build row has NULL in the eq join slot,
        // append it to the null_aware partition. We will need it later.
        if (!null_aware_partition_->build_rows()->AddRow(build_row)) {
          return null_aware_partition_->build_rows()->status();
        }
      }
      continue;
    }
    Partition* partition = hash_partitions_[hash >> (32 - NUM_PARTITIONING_BITS)];
    // TODO: Should we maintain a histogram with the size of each partition?
    bool result = AppendRow(partition->build_rows(), build_row);
    if (UNLIKELY(!result)) return status_;
  }
  return Status::OK;
}
