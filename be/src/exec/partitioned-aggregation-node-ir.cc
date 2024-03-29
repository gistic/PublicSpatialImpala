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

#include "exec/partitioned-aggregation-node.h"

#include "exec/hash-table.inline.h"
#include "runtime/buffered-tuple-stream.inline.h"
#include "runtime/row-batch.h"
#include "runtime/tuple-row.h"

using namespace impala;

Status PartitionedAggregationNode::ProcessBatchNoGrouping(
    RowBatch* batch, HashTableCtx* ht_ctx) {
  for (int i = 0; i < batch->num_rows(); ++i) {
    UpdateTuple(&agg_fn_ctxs_[0], singleton_output_tuple_, batch->GetRow(i));
  }
  return Status::OK;
}

template<bool AGGREGATED_ROWS>
Status PartitionedAggregationNode::ProcessBatch(RowBatch* batch, HashTableCtx* ht_ctx) {
  DCHECK(!hash_partitions_.empty());

  for (int i = 0; i < batch->num_rows(); ++i) {
    TupleRow* row = batch->GetRow(i);
    uint32_t hash = 0;
    if (AGGREGATED_ROWS) {
      if (!ht_ctx->EvalAndHashBuild(row, &hash)) continue;
    } else {
      if (!ht_ctx->EvalAndHashProbe(row, &hash)) continue;
    }

    // To process this row, we first see if it can be aggregated or inserted into this
    // partition's hash table. If we need to insert it and that fails, due to OOM, we
    // spill the partition. The partition to spill is not necessarily dst_partition,
    // so we can try again to insert the row.
    Partition* dst_partition = hash_partitions_[hash >> (32 - NUM_PARTITIONING_BITS)];
    if (!dst_partition->is_spilled()) {
      DCHECK_NOTNULL(dst_partition->hash_tbl.get());
      DCHECK(dst_partition->aggregated_row_stream->is_pinned());

      HashTable* ht = dst_partition->hash_tbl.get();
      if (!AGGREGATED_ROWS) {
        // If the row is already an aggregate row, it cannot match anything in the
        // hash table since we process the aggregate rows first. These rows should
        // have been aggregated in the initial pass.
        // TODO: change HT interface to have FindOrInsert()
        HashTable::Iterator it = ht->Find(ht_ctx, hash);
        if (!it.AtEnd()) {
          // Row is already in hash table. Do the aggregation and we're done.
          UpdateTuple(&dst_partition->agg_fn_ctxs[0], it.GetTuple(), row);
          continue;
        }
      } else {
        DCHECK(ht->Find(ht_ctx, hash).AtEnd()) << ht->size();
      }

      Tuple* intermediate_tuple = NULL;
allocate_tuple:
#if 0
      // TODO: this optimization doesn't work. Why?
      // First construct the intermediate tuple in the dst partition's stream.
      // TODO: needs_serialize can be removed with codegen.
      if (AGGREGATED_ROWS && !needs_serialize_) {
        // We can just copy the row into the stream.
        if (!dst_partition->aggregated_row_stream->AddRow(
            row, reinterpret_cast<uint8_t**>(&intermediate_tuple))) {
          intermediate_tuple = NULL;
        }
      }
#endif
      // If this aggregate function requires serialize, or we are seeing this
      // result row the first time, we need to construct the result row and
      // initialize it.
      intermediate_tuple = ConstructIntermediateTuple(dst_partition->agg_fn_ctxs,
          NULL, dst_partition->aggregated_row_stream.get());
      if (intermediate_tuple != NULL) {
        UpdateTuple(&dst_partition->agg_fn_ctxs[0],
            intermediate_tuple, row, AGGREGATED_ROWS);
      }

      // After copying and initialize it, try to insert the tuple into the hash table.
      // If it inserts, we are done.
      if (intermediate_tuple != NULL && ht->Insert(ht_ctx, intermediate_tuple, hash)) {
        continue;
      }

      // In this case, we either didn't have enough memory to add the intermediate_tuple
      // to the stream or we didn't have enough memory to insert it into the hash table.
      // We need to spill.
      RETURN_IF_ERROR(SpillPartition(dst_partition, intermediate_tuple));
      if (!dst_partition->is_spilled()) {
        DCHECK(dst_partition->aggregated_row_stream->is_pinned());
        // We spilled a different partition, try to insert this tuple.
        if (intermediate_tuple == NULL) goto allocate_tuple;
        if (ht->Insert(ht_ctx, intermediate_tuple, hash)) continue;
        DCHECK(false) << "How can we get here. We spilled a different partition but "
            "still did not have enough memory.";
        return Status::MEM_LIMIT_EXCEEDED;
      }

      // In this case, we were able to add the tuple to the stream but not enough
      // to put it in the hash table. Nothing left to do, the tuple is spilled.
      if (intermediate_tuple != NULL) continue;
    }

    // This partition is already spilled, just append the row.
    BufferedTupleStream* dst_stream = AGGREGATED_ROWS ?
        dst_partition->aggregated_row_stream.get() :
        dst_partition->unaggregated_row_stream.get();
    DCHECK(dst_stream != NULL);
    DCHECK(!dst_stream->is_pinned()) << AGGREGATED_ROWS;
    DCHECK(dst_stream->has_write_block()) << AGGREGATED_ROWS;
    if (dst_stream->AddRow(row)) continue;
    Status status = dst_stream->status();
    DCHECK(!status.ok()) << AGGREGATED_ROWS;
    return status;
  }

  return Status::OK;
}

Status PartitionedAggregationNode::ProcessBatch_false(
    RowBatch* batch, HashTableCtx* ht_ctx) {
  return ProcessBatch<false>(batch, ht_ctx);
}

Status PartitionedAggregationNode::ProcessBatch_true(
    RowBatch* batch, HashTableCtx* ht_ctx) {
  return ProcessBatch<true>(batch, ht_ctx);
}
