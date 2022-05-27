// This file is made available under Elastic License 2.0
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/gensrc/thrift/PlanNodes.thrift

// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

namespace cpp starrocks
namespace java com.starrocks.thrift

include "Exprs.thrift"
include "Types.thrift"

enum TRuntimeFilterBuildJoinMode {
  NONE,
  BORADCAST,
  PARTITIONED,
  LOCAL_HASH_BUCKET,
  COLOCATE,
  SHUFFLE_HASH_BUCKET,
  REPLICATED
}

struct TRuntimeFilterDescription {
  // Filter unique id (within a query)
  1: optional i32 filter_id

  // Expr on which the filter is built on a hash join.
  2: optional Exprs.TExpr build_expr

  // The order of Expr in join predicate
  3: optional i32 expr_order

  // Map of target node id to the probe expr of target node.
  4: optional map<Types.TPlanNodeId, Exprs.TExpr> plan_node_id_to_target_expr

  // Indicates if there is at least one target scan node that is not in the same
  // fragment as the join node that produced the runtime filter
  6: optional bool has_remote_targets;

  // The size of the bloom filter. For global rf, it should be specified.
  // And for local rf, build side can choose bloom filter size at his will.
  7: optional i64 bloom_filter_size

  // address of merge nodes.
  // multiple rf merge nodes can address straggler problem.
  8: optional list<Types.TNetworkAddress> runtime_filter_merge_nodes;

  // partitioned and bucket shuffle use different hash algorithm.
  9: optional TRuntimeFilterBuildJoinMode build_join_mode;
  // if this rf is generated by broadcast, and can be used by other process
  // we just need one instance to send one copy of that rf.
  10: optional Types.TUniqueId sender_finst_id;
  // TPlanNodeId of HashJoinNode who build this runtime filter
  11: optional Types.TPlanNodeId build_plan_node_id;
  // support speculative delivery of GRFs generated by broadcast HashJoin   
  12: optional list<Types.TUniqueId> broadcast_grf_senders;
  13: optional list<TRuntimeFilterDestination> broadcast_grf_destinations;
  
  // for COLOCATE and LOCAL_HASH_BUCKET HashJoin, we need a mapping to save
  // bucketSeq to fragment instance ordinal mapping.
  14: optional list<i32> bucketseq_to_instance;
}

struct TRuntimeFilterProberParams {
  1: optional Types.TUniqueId fragment_instance_id
  2: optional Types.TNetworkAddress fragment_instance_address
}

struct TRuntimeFilterDestination {
  1: optional Types.TNetworkAddress address;
  2: optional list<Types.TUniqueId> finstance_ids;
}

struct TRuntimeFilterParams {
  // Runtime filter Id to the fragment instances where that runtime filter will be applied on
  2: optional map<i32, list<TRuntimeFilterProberParams>> id_to_prober_params
  // Runtime filter Id to (number of partitioned runtime filters)
  // To merge a global runtime filter, merge node has to merge
  // all partitioned runtime filters for the sake of correctness.
  3: optional map<i32, i32> runtime_filter_builder_number
  // if aggregated runtime filter size exceeds it, merge node can stop merging.
  4: optional i64 runtime_filter_max_size;
}
