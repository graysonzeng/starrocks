// This file is licensed under the Elastic License 2.0. Copyright 2021 StarRocks Limited.

#include "exec/pipeline/scan/olap_scan_context.h"

#include "exec/vectorized/olap_scan_node.h"
#include "exprs/vectorized/runtime_filter_bank.h"

namespace starrocks::pipeline {

using namespace vectorized;

/// OlapScanContext.
void OlapScanContext::attach_shared_input(int32_t operator_seq, int32_t source_index) {
    auto key = std::make_pair(operator_seq, source_index);
    DCHECK(_active_inputs.count(key) == 0);
    VLOG_ROW << fmt::format("attach_shared_input ({}, {}), active {}", operator_seq, source_index,
                            _active_inputs.size());
    _active_inputs.emplace(key);
}

void OlapScanContext::detach_shared_input(int32_t operator_seq, int32_t source_index) {
    auto key = std::make_pair(operator_seq, source_index);
    DCHECK(_active_inputs.count(key) == 1);
    VLOG_ROW << fmt::format("detach_shared_input ({}, {}), remain {}", operator_seq, source_index,
                            _active_inputs.size());
    _active_inputs.erase(key);
}

bool OlapScanContext::has_active_input() const {
    return !_active_inputs.empty();
}

BalancedChunkBuffer& OlapScanContext::get_shared_buffer() {
    return _chunk_buffer;
}

Status OlapScanContext::prepare(RuntimeState* state) {
    return Status::OK();
}

void OlapScanContext::close(RuntimeState* state) {
    _chunk_buffer.close();
}

Status OlapScanContext::parse_conjuncts(RuntimeState* state, const std::vector<ExprContext*>& runtime_in_filters,
                                        RuntimeFilterProbeCollector* runtime_bloom_filters) {
    const TOlapScanNode& thrift_olap_scan_node = _scan_node->thrift_olap_scan_node();
    const TupleDescriptor* tuple_desc = state->desc_tbl().get_tuple_descriptor(thrift_olap_scan_node.tuple_id);

    // Get _conjunct_ctxs.
    _conjunct_ctxs = _scan_node->conjunct_ctxs();
    _conjunct_ctxs.insert(_conjunct_ctxs.end(), runtime_in_filters.begin(), runtime_in_filters.end());

    // eval_const_conjuncts.
    Status status;
    RETURN_IF_ERROR(vectorized::OlapScanConjunctsManager::eval_const_conjuncts(_conjunct_ctxs, &status));
    if (!status.ok()) {
        return status;
    }

    // Init _conjuncts_manager.
    vectorized::OlapScanConjunctsManager& cm = _conjuncts_manager;
    cm.conjunct_ctxs_ptr = &_conjunct_ctxs;
    cm.tuple_desc = tuple_desc;
    cm.obj_pool = &_obj_pool;
    cm.key_column_names = &thrift_olap_scan_node.key_column_name;
    cm.runtime_filters = runtime_bloom_filters;
    cm.runtime_state = state;

    const TQueryOptions& query_options = state->query_options();
    int32_t max_scan_key_num;
    if (query_options.__isset.max_scan_key_num && query_options.max_scan_key_num > 0) {
        max_scan_key_num = query_options.max_scan_key_num;
    } else {
        max_scan_key_num = config::max_scan_key_num;
    }
    bool enable_column_expr_predicate = false;
    if (thrift_olap_scan_node.__isset.enable_column_expr_predicate) {
        enable_column_expr_predicate = thrift_olap_scan_node.enable_column_expr_predicate;
    }

    // Parse conjuncts via _conjuncts_manager.
    RETURN_IF_ERROR(cm.parse_conjuncts(true, max_scan_key_num, enable_column_expr_predicate));

    // Get key_ranges and not_push_down_conjuncts from _conjuncts_manager.
    RETURN_IF_ERROR(_conjuncts_manager.get_key_ranges(&_key_ranges));
    _conjuncts_manager.get_not_push_down_conjuncts(&_not_push_down_conjuncts);

    _dict_optimize_parser.set_mutable_dict_maps(state, state->mutable_query_global_dict_map());
    _dict_optimize_parser.rewrite_conjuncts(&_not_push_down_conjuncts, state);

    return Status::OK();
}

/// OlapScanContextFactory.
OlapScanContextPtr OlapScanContextFactory::get_or_create(int32_t driver_sequence) {
    DCHECK_LT(driver_sequence, _dop);
    // ScanOperators sharing one morsel use the same context.
    int32_t idx = _shared_morsel_queue ? 0 : driver_sequence;
    DCHECK_LT(idx, _contexts.size());

    if (_contexts[idx] == nullptr) {
        _contexts[idx] = std::make_shared<OlapScanContext>(_scan_node, _dop, _shared_scan, _chunk_buffer);
    }
    return _contexts[idx];
}

} // namespace starrocks::pipeline
