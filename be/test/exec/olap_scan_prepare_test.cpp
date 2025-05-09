// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "exec/olap_scan_prepare.h"

#include <gtest/gtest.h>

#include "exec/tablet_scanner.h"
#include "exprs/column_ref.h"
#include "exprs/in_const_predicate.hpp"
#include "formats/parquet/parquet_test_util/util.h"
#include "storage/predicate_parser.h"
#include "testutil/column_test_helper.h"
#include "testutil/exprs_test_helper.h"
#include "testutil/schema_test_helper.h"

namespace starrocks {
class ChunkPredicateBuilderTest : public testing::Test {
public:
    void SetUp() override {
        _int_tablet_schema = SchemaTestHelper::gen_schema_of_dup(TYPE_INT, 1, 3, 1);
        _date_tablet_schema = SchemaTestHelper::gen_schema_of_dup(TYPE_DATE, 1, 3, 1);

        _int_pred_parser = _pool.add(new OlapPredicateParser(_int_tablet_schema));
        _date_pred_parser = _pool.add(new OlapPredicateParser(_date_tablet_schema));

        _opts.runtime_state = &_runtime_state;
        _opts.obj_pool = &_pool;
        _opts.pred_tree_params.enable_or = true;
        _opts.key_column_names = &_key_column_names;
        _opts.conjunct_ctxs_ptr = &_expr_ctxs;
    }

protected:
    template <LogicalType Type>
    StatusOr<RuntimeFilterProbeDescriptor*> _gen_runtime_filter_desc(SlotId slot_id);
    StatusOr<RuntimeFilterProbeCollector*> _gen_runtime_filter_collector(SlotId slot_id, bool has_null);
    StatusOr<RuntimeFilterProbeCollector*> _gen_varchar_runtime_filter_collector(SlotId slot_id, bool has_null);
    template <bool IsMin>
    StatusOr<RuntimeFilterProbeCollector*> _gen_range_runtime_filter_collector(SlotId slot_id, bool has_null);

    ScanConjunctsManagerOptions _opts;
    RuntimeState _runtime_state;
    ObjectPool _pool;
    std::vector<std::string> _key_column_names;

    TabletSchemaSPtr _int_tablet_schema;
    TabletSchemaSPtr _date_tablet_schema;

    OlapPredicateParser* _int_pred_parser;
    OlapPredicateParser* _date_pred_parser;

    std::vector<TExpr> _texprs;
    std::vector<ExprContext*> _expr_ctxs;
    ColumnPredicatePtrs _predicate_free_pool;
    std::vector<BoxedExprContext> _expr_containers;
};

template <LogicalType Type>
StatusOr<RuntimeFilterProbeDescriptor*> ChunkPredicateBuilderTest::_gen_runtime_filter_desc(SlotId slot_id) {
    TRuntimeFilterDescription tRuntimeFilterDescription;
    tRuntimeFilterDescription.__set_filter_id(1);
    tRuntimeFilterDescription.__set_has_remote_targets(false);
    tRuntimeFilterDescription.__set_build_plan_node_id(1);
    tRuntimeFilterDescription.__set_build_join_mode(TRuntimeFilterBuildJoinMode::BORADCAST);
    tRuntimeFilterDescription.__set_filter_type(TRuntimeFilterBuildType::JOIN_FILTER);

    TExpr col_ref = ExprsTestHelper::create_column_ref_t_expr<Type>(slot_id, true);
    tRuntimeFilterDescription.__isset.plan_node_id_to_target_expr = true;
    tRuntimeFilterDescription.plan_node_id_to_target_expr.emplace(1, col_ref);

    auto* runtime_filter_desc = _pool.add(new RuntimeFilterProbeDescriptor());
    RETURN_IF_ERROR(runtime_filter_desc->init(&_pool, tRuntimeFilterDescription, 1, &_runtime_state));

    return runtime_filter_desc;
}

StatusOr<RuntimeFilterProbeCollector*> ChunkPredicateBuilderTest::_gen_runtime_filter_collector(SlotId slot_id,
                                                                                                bool has_null) {
    auto* rf = _pool.add(new RuntimeBloomFilter<TYPE_INT>());
    rf->insert(10);
    rf->insert(20);
    if (has_null) {
        rf->insert_null();
    }

    ASSIGN_OR_RETURN(auto* rf_desc, _gen_runtime_filter_desc<TYPE_INT>(slot_id));
    rf_desc->set_runtime_filter(rf);

    auto* rf_collector = _pool.add(new RuntimeFilterProbeCollector());
    rf_collector->add_descriptor(rf_desc);

    return rf_collector;
}

StatusOr<RuntimeFilterProbeCollector*> ChunkPredicateBuilderTest::_gen_varchar_runtime_filter_collector(SlotId slot_id,
                                                                                                        bool has_null) {
    auto* rf = _pool.add(new RuntimeBloomFilter<TYPE_VARCHAR>());
    rf->insert(Slice("111"));
    rf->insert(Slice("222"));
    if (has_null) {
        rf->insert_null();
    }

    ASSIGN_OR_RETURN(auto* rf_desc, _gen_runtime_filter_desc<TYPE_VARCHAR>(slot_id));
    rf_desc->set_runtime_filter(rf);

    auto* rf_collector = _pool.add(new RuntimeFilterProbeCollector());
    rf_collector->add_descriptor(rf_desc);

    return rf_collector;
}

template <bool IsMin>
StatusOr<RuntimeFilterProbeCollector*> ChunkPredicateBuilderTest::_gen_range_runtime_filter_collector(SlotId slot_id,
                                                                                                      bool has_null) {
    auto* rf = RuntimeBloomFilter<TYPE_INT>::create_with_range<IsMin>(&_pool, 10, false);
    if (has_null) {
        rf->insert_null();
    }

    ASSIGN_OR_RETURN(auto* rf_desc, _gen_runtime_filter_desc<TYPE_INT>(slot_id));
    rf_desc->set_runtime_filter(rf);

    auto* rf_collector = _pool.add(new RuntimeFilterProbeCollector());
    rf_collector->add_descriptor(rf_desc);

    return rf_collector;
}

TEST_F(ChunkPredicateBuilderTest, normalized_in_has_null) {
    SlotId slot_id = 1;
    RuntimeFilterProbeCollector collector;
    parquet::Utils::SlotDesc slot_descs[] = {{"c1", TYPE_INT_DESC, 1}, {"c2", TYPE_INT_DESC, 2}, {""}};
    _opts.tuple_desc = parquet::Utils::create_tuple_descriptor(&_runtime_state, &_pool, slot_descs);
    _opts.runtime_filters = &collector;

    std::vector<int32_t> values{1, 3, 5, 7, 9};
    _texprs.emplace_back(ExprsTestHelper::create_in_pred_texpr<TYPE_INT, int32_t>(slot_id, values, true));
    ASSERT_OK(ExprsTestHelper::create_and_open_conjunct_ctxs(&_pool, &_runtime_state, &_texprs, &_expr_ctxs));
    ASSERT_EQ(_expr_ctxs.size(), 1);

    _expr_containers.emplace_back(BoxedExprContext(_expr_ctxs[0]));

    ChunkPredicateBuilder<BoxedExprContext, CompoundNodeType::AND> builder(_opts, _expr_containers, true);
    ASSIGN_OR_ASSERT_FAIL(auto normalized, builder.parse_conjuncts());
    ASSERT_TRUE(normalized);

    ASSIGN_OR_ASSERT_FAIL(auto pred, builder.get_predicate_tree_root(_int_pred_parser, _predicate_free_pool));
    ASSERT_EQ(pred.debug_string(), "{\"and\":[{\"pred\":\"((columnId=1)IN(9,5,1,7,3))\"}]}");
}

TEST_F(ChunkPredicateBuilderTest, normalized_in_has_null_larger_than_1024) {
    SlotId slot_id = 1;
    RuntimeFilterProbeCollector collector;
    parquet::Utils::SlotDesc slot_descs[] = {{"c1", TYPE_INT_DESC, 1}, {"c2", TYPE_INT_DESC, 2}, {""}};
    _opts.tuple_desc = parquet::Utils::create_tuple_descriptor(&_runtime_state, &_pool, slot_descs);
    _opts.runtime_filters = &collector;

    std::vector<int32_t> values;
    for (int32_t i = 0; i < 2048; i++) {
        values.emplace_back(i);
    }
    _texprs.emplace_back(ExprsTestHelper::create_in_pred_texpr<TYPE_INT, int32_t>(slot_id, values, true));
    ASSERT_OK(ExprsTestHelper::create_and_open_conjunct_ctxs(&_pool, &_runtime_state, &_texprs, &_expr_ctxs));
    ASSERT_EQ(_expr_ctxs.size(), 1);

    _expr_containers.emplace_back(BoxedExprContext(_expr_ctxs[0]));

    ChunkPredicateBuilder<BoxedExprContext, CompoundNodeType::AND> builder(_opts, _expr_containers, true);
    ASSIGN_OR_ASSERT_FAIL(auto normalized, builder.parse_conjuncts());
    ASSERT_TRUE(normalized);

    ASSIGN_OR_ASSERT_FAIL(auto pred, builder.get_predicate_tree_root(_int_pred_parser, _predicate_free_pool));
    ASSERT_EQ(pred.debug_string(), "{\"and\":[]}");
}

TEST_F(ChunkPredicateBuilderTest, normalized_in_has_null_null_equal) {
    SlotId slot_id = 1;
    parquet::Utils::SlotDesc slot_descs[] = {{"c1", TYPE_INT_DESC, 1}, {"c2", TYPE_INT_DESC, 2}, {""}};
    _opts.tuple_desc = parquet::Utils::create_tuple_descriptor(&_runtime_state, &_pool, slot_descs);
    RuntimeFilterProbeCollector collector;
    _opts.runtime_filters = &collector;

    std::vector<int32_t> values{1, 3, 5, 7, 9};
    _texprs.emplace_back(ExprsTestHelper::create_in_pred_texpr<TYPE_INT, int32_t>(slot_id, values, true));
    ASSERT_OK(ExprsTestHelper::create_and_open_conjunct_ctxs(&_pool, &_runtime_state, &_texprs, &_expr_ctxs));
    ASSERT_EQ(_expr_ctxs.size(), 1);

    auto* in_const_expr = reinterpret_cast<VectorizedInConstPredicate<TYPE_INT>*>(_expr_ctxs[0]->root());
    in_const_expr->set_eq_null(true);
    _expr_containers.emplace_back(BoxedExprContext(_expr_ctxs[0]));

    ChunkPredicateBuilder<BoxedExprContext, CompoundNodeType::AND> builder(_opts, _expr_containers, true);
    ASSIGN_OR_ASSERT_FAIL(auto normalized, builder.parse_conjuncts());
    ASSERT_TRUE(normalized);

    ASSIGN_OR_ASSERT_FAIL(auto pred, builder.get_predicate_tree_root(_int_pred_parser, _predicate_free_pool));
    ASSERT_EQ(pred.debug_string(), "{\"and\":[]}");
}

TEST_F(ChunkPredicateBuilderTest, normalized_in_has_null_date) {
    SlotId slot_id = 1;
    RuntimeFilterProbeCollector collector;
    parquet::Utils::SlotDesc slot_descs[] = {{"c1", TYPE_DATE_DESC, 1}, {"c2", TYPE_DATE_DESC, 2}, {""}};
    _opts.tuple_desc = parquet::Utils::create_tuple_descriptor(&_runtime_state, &_pool, slot_descs);
    _opts.runtime_filters = &collector;

    _texprs.emplace_back(ExprsTestHelper::create_in_pred_texpr<TYPE_DATE, std::string>(
            slot_id, {"2014-01-01", "2014-01-02", "2014-01-03"}, true));
    ASSERT_OK(ExprsTestHelper::create_and_open_conjunct_ctxs(&_pool, &_runtime_state, &_texprs, &_expr_ctxs));
    ASSERT_EQ(_expr_ctxs.size(), 1);

    _expr_containers.emplace_back(BoxedExprContext(_expr_ctxs[0]));

    ChunkPredicateBuilder<BoxedExprContext, CompoundNodeType::AND> builder(_opts, _expr_containers, true);
    ASSIGN_OR_ASSERT_FAIL(auto normalized, builder.parse_conjuncts());
    ASSERT_TRUE(normalized);

    ASSIGN_OR_ASSERT_FAIL(auto pred, builder.get_predicate_tree_root(_date_pred_parser, _predicate_free_pool));
    ASSERT_EQ(pred.debug_string(), "{\"and\":[{\"pred\":\"((columnId=1)IN(2014-01-01,2014-01-02,2014-01-03))\"}]}");
}

TEST_F(ChunkPredicateBuilderTest, normalized_in_has_null_date_null_equal) {
    SlotId slot_id = 1;
    RuntimeFilterProbeCollector collector;
    parquet::Utils::SlotDesc slot_descs[] = {{"c1", TYPE_DATE_DESC, 1}, {"c2", TYPE_DATE_DESC, 2}, {""}};
    _opts.tuple_desc = parquet::Utils::create_tuple_descriptor(&_runtime_state, &_pool, slot_descs);
    _opts.runtime_filters = &collector;

    _texprs.emplace_back(ExprsTestHelper::create_in_pred_texpr<TYPE_DATE, std::string>(
            slot_id, {"2014-01-01", "2014-01-02", "2014-01-03"}, true));
    ASSERT_OK(ExprsTestHelper::create_and_open_conjunct_ctxs(&_pool, &_runtime_state, &_texprs, &_expr_ctxs));
    ASSERT_EQ(_expr_ctxs.size(), 1);

    auto* in_const_expr = reinterpret_cast<VectorizedInConstPredicate<TYPE_DATE>*>(_expr_ctxs[0]->root());
    in_const_expr->set_eq_null(true);
    _expr_containers.emplace_back(BoxedExprContext(_expr_ctxs[0]));

    ChunkPredicateBuilder<BoxedExprContext, CompoundNodeType::AND> builder(_opts, _expr_containers, true);
    ASSIGN_OR_ASSERT_FAIL(auto normalized, builder.parse_conjuncts());
    ASSERT_TRUE(normalized);

    ASSIGN_OR_ASSERT_FAIL(auto pred, builder.get_predicate_tree_root(_date_pred_parser, _predicate_free_pool));
    ASSERT_EQ(pred.debug_string(), "{\"and\":[]}");
}

TEST_F(ChunkPredicateBuilderTest, normalize_or_in_has_null) {
    SlotId slot_id = 1;
    parquet::Utils::SlotDesc slot_descs[] = {{"c1", TYPE_INT_DESC, 1}, {"c2", TYPE_INT_DESC, 2}, {""}};
    _opts.tuple_desc = parquet::Utils::create_tuple_descriptor(&_runtime_state, &_pool, slot_descs);

    _texprs.emplace_back(ExprsTestHelper::create_binary_pred_texpr<TYPE_INT, int32_t>(slot_id, 10));
    _texprs.emplace_back(ExprsTestHelper::create_not_in_pred_texpr<TYPE_INT, int32_t>(
            slot_id, std::vector<int32_t>{1, 3, 5, 7, 9}, true));
    ASSERT_OK(ExprsTestHelper::create_and_open_conjunct_ctxs(&_pool, &_runtime_state, &_texprs, &_expr_ctxs));
    ASSERT_EQ(_expr_ctxs.size(), 2);
    _expr_containers.emplace_back(BoxedExprContext(_expr_ctxs[0]));
    _expr_containers.emplace_back(BoxedExprContext(_expr_ctxs[1]));

    ChunkPredicateBuilder<BoxedExprContext, CompoundNodeType::OR> builder(_opts, _expr_containers, false);
    ASSIGN_OR_ASSERT_FAIL(auto normalized, builder.parse_conjuncts());
    ASSERT_TRUE(normalized);

    ASSIGN_OR_ASSERT_FAIL(auto pred, builder.get_predicate_tree_root(_int_pred_parser, _predicate_free_pool));
    ASSERT_EQ(pred.debug_string(), "{\"or\":[{\"pred\":\"(columnId(1)>10)\"}]}");
}

TEST_F(ChunkPredicateBuilderTest, normalize_or_in_has_null_date) {
    SlotId slot_id = 1;
    parquet::Utils::SlotDesc slot_descs[] = {{"c1", TYPE_DATE_DESC, 1}, {"c2", TYPE_DATE_DESC, 2}, {""}};
    _opts.tuple_desc = parquet::Utils::create_tuple_descriptor(&_runtime_state, &_pool, slot_descs);

    _texprs.emplace_back(ExprsTestHelper::create_binary_pred_texpr<TYPE_DATE, std::string>(slot_id, "2023-01-02"));
    _texprs.emplace_back(ExprsTestHelper::create_in_pred_texpr<TYPE_DATE, std::string>(
            slot_id, {"2014-01-01", "2014-01-02", "2014-01-03"}, true));
    ASSERT_OK(ExprsTestHelper::create_and_open_conjunct_ctxs(&_pool, &_runtime_state, &_texprs, &_expr_ctxs));
    ASSERT_EQ(_expr_ctxs.size(), 2);
    _expr_containers.emplace_back(BoxedExprContext(_expr_ctxs[0]));
    _expr_containers.emplace_back(BoxedExprContext(_expr_ctxs[1]));

    ChunkPredicateBuilder<BoxedExprContext, CompoundNodeType::OR> builder(_opts, _expr_containers, false);
    ASSIGN_OR_ASSERT_FAIL(auto normalized, builder.parse_conjuncts());
    ASSERT_FALSE(normalized);
}

TEST_F(ChunkPredicateBuilderTest, normalized_not_in_has_null) {
    SlotId slot_id = 1;
    parquet::Utils::SlotDesc slot_descs[] = {{"c1", TYPE_INT_DESC, 1}, {"c2", TYPE_INT_DESC, 2}, {""}};
    _opts.tuple_desc = parquet::Utils::create_tuple_descriptor(&_runtime_state, &_pool, slot_descs);
    RuntimeFilterProbeCollector collector;
    _opts.runtime_filters = &collector;

    _texprs.emplace_back(ExprsTestHelper::create_not_in_pred_texpr<TYPE_INT, int32_t>(slot_id, {1, 3, 5, 7, 9}, true));
    ASSERT_OK(ExprsTestHelper::create_and_open_conjunct_ctxs(&_pool, &_runtime_state, &_texprs, &_expr_ctxs));
    ASSERT_EQ(_expr_ctxs.size(), 1);

    _expr_containers.emplace_back(BoxedExprContext(_expr_ctxs[0]));

    ChunkPredicateBuilder<BoxedExprContext, CompoundNodeType::AND> builder(_opts, _expr_containers, true);
    auto ret = builder.parse_conjuncts();
    ASSERT_TRUE(ret.status().is_end_of_file());
}

TEST_F(ChunkPredicateBuilderTest, normalized_not_in_has_null_larger_than_1024) {
    SlotId slot_id = 1;
    parquet::Utils::SlotDesc slot_descs[] = {{"c1", TYPE_INT_DESC, 1}, {"c2", TYPE_INT_DESC, 2}, {""}};
    _opts.tuple_desc = parquet::Utils::create_tuple_descriptor(&_runtime_state, &_pool, slot_descs);
    RuntimeFilterProbeCollector collector;
    _opts.runtime_filters = &collector;

    std::vector<int32_t> values;
    for (int32_t i = 0; i < 2048; i++) {
        values.emplace_back(i);
    }

    _texprs.emplace_back(ExprsTestHelper::create_not_in_pred_texpr<TYPE_INT, int32_t>(slot_id, values, false));
    ASSERT_OK(ExprsTestHelper::create_and_open_conjunct_ctxs(&_pool, &_runtime_state, &_texprs, &_expr_ctxs));
    ASSERT_EQ(_expr_ctxs.size(), 1);

    _expr_containers.emplace_back(BoxedExprContext(_expr_ctxs[0]));

    ChunkPredicateBuilder<BoxedExprContext, CompoundNodeType::AND> builder(_opts, _expr_containers, true);
    ASSIGN_OR_ASSERT_FAIL(auto normalized, builder.parse_conjuncts());
    ASSERT_TRUE(normalized);

    ASSIGN_OR_ASSERT_FAIL(auto pred, builder.get_predicate_tree_root(_int_pred_parser, _predicate_free_pool));
    ASSERT_EQ(pred.debug_string(), "{\"and\":[]}");
}

TEST_F(ChunkPredicateBuilderTest, normalize_or_not_in_has_null) {
    SlotId slot_id = 1;
    parquet::Utils::SlotDesc slot_descs[] = {{"c1", TYPE_INT_DESC, 1}, {"c2", TYPE_INT_DESC, 2}, {""}};
    _opts.tuple_desc = parquet::Utils::create_tuple_descriptor(&_runtime_state, &_pool, slot_descs);

    _texprs.emplace_back(ExprsTestHelper::create_binary_pred_texpr<TYPE_INT, int32_t>(slot_id, 10));
    _texprs.emplace_back(ExprsTestHelper::create_in_pred_texpr<TYPE_INT, int32_t>(
            slot_id, std::vector<int32_t>{1, 3, 5, 7, 9}, true));
    ASSERT_OK(ExprsTestHelper::create_and_open_conjunct_ctxs(&_pool, &_runtime_state, &_texprs, &_expr_ctxs));
    ASSERT_EQ(_expr_ctxs.size(), 2);
    _expr_containers.emplace_back(BoxedExprContext(_expr_ctxs[0]));
    _expr_containers.emplace_back(BoxedExprContext(_expr_ctxs[1]));

    ChunkPredicateBuilder<BoxedExprContext, CompoundNodeType::OR> builder(_opts, _expr_containers, false);
    ASSIGN_OR_ASSERT_FAIL(auto normalized, builder.parse_conjuncts());
    ASSERT_FALSE(normalized);
}

TEST_F(ChunkPredicateBuilderTest, normalize_or_not_in_has_null_date) {
    SlotId slot_id = 1;
    parquet::Utils::SlotDesc slot_descs[] = {{"c1", TYPE_DATE_DESC, 1}, {"c2", TYPE_DATE_DESC, 2}, {""}};
    _opts.tuple_desc = parquet::Utils::create_tuple_descriptor(&_runtime_state, &_pool, slot_descs);

    _texprs.emplace_back(ExprsTestHelper::create_binary_pred_texpr<TYPE_DATE, std::string>(slot_id, "2023-01-02"));
    _texprs.emplace_back(ExprsTestHelper::create_in_pred_texpr<TYPE_DATE, std::string>(
            slot_id, {"2014-01-01", "2014-01-02", "2014-01-03"}, true));
    ASSERT_OK(ExprsTestHelper::create_and_open_conjunct_ctxs(&_pool, &_runtime_state, &_texprs, &_expr_ctxs));
    ASSERT_EQ(_expr_ctxs.size(), 2);
    _expr_containers.emplace_back(BoxedExprContext(_expr_ctxs[0]));
    _expr_containers.emplace_back(BoxedExprContext(_expr_ctxs[1]));

    ChunkPredicateBuilder<BoxedExprContext, CompoundNodeType::OR> builder(_opts, _expr_containers, false);
    ASSIGN_OR_ASSERT_FAIL(auto normalized, builder.parse_conjuncts());
    ASSERT_FALSE(normalized);
}

TEST_F(ChunkPredicateBuilderTest, rt_has_no_null) {
    SlotId slot_id = 1;
    parquet::Utils::SlotDesc slot_descs[] = {{"c1", TYPE_INT_DESC, 1}, {"c2", TYPE_INT_DESC, 2}, {""}};
    _opts.tuple_desc = parquet::Utils::create_tuple_descriptor(&_runtime_state, &_pool, slot_descs);

    auto ret1 = _gen_runtime_filter_collector(slot_id, false);
    ASSERT_TRUE(ret1.ok());

    _opts.runtime_filters = ret1.value();

    ChunkPredicateBuilder<BoxedExprContext, CompoundNodeType::AND> builder(_opts, _expr_containers, true);

    auto ret2 = builder.parse_conjuncts();
    ASSERT_TRUE(ret1.ok());
    ASSERT_TRUE(ret2.value());

    auto ret3 = builder.get_predicate_tree_root(_int_pred_parser, _predicate_free_pool);
    ASSERT_TRUE(ret3.ok());
    ASSERT_EQ(ret3.value().debug_string(),
              "{\"and\":[{\"pred\":\"(columnId(1)>=10)\"},{\"pred\":\"(columnId(1)<=20)\"}]}");
}

TEST_F(ChunkPredicateBuilderTest, rt_has_null) {
    SlotId slot_id = 1;
    parquet::Utils::SlotDesc slot_descs[] = {{"c1", TYPE_INT_DESC, 1}, {"c2", TYPE_INT_DESC, 2}, {""}};
    _opts.tuple_desc = parquet::Utils::create_tuple_descriptor(&_runtime_state, &_pool, slot_descs);

    ASSIGN_OR_ASSERT_FAIL(auto* rf, _gen_runtime_filter_collector(slot_id, true));

    _opts.runtime_filters = rf;

    ChunkPredicateBuilder<BoxedExprContext, CompoundNodeType::AND> builder(_opts, _expr_containers, true);

    ASSIGN_OR_ASSERT_FAIL(auto normalized, builder.parse_conjuncts());
    ASSERT_TRUE(normalized);

    auto ret3 = builder.get_predicate_tree_root(_int_pred_parser, _predicate_free_pool);
    ASSERT_TRUE(ret3.ok());
    ASSERT_EQ(ret3.value().debug_string(),
              "{\"and\":[{\"or\":[{\"pred\":\"(ColumnId(1) IS "
              "NULL)\"},{\"and\":[{\"pred\":\"(columnId(1)>=10)\"},{\"pred\":\"(columnId(1)<=20)\"}]}]}]}");
}

TEST_F(ChunkPredicateBuilderTest, varchar_rt_has_no_null) {
    SlotId slot_id = 1;
    parquet::Utils::SlotDesc slot_descs[] = {{"c1", TYPE_VARCHAR_DESC, 1}, {"c2", TYPE_VARCHAR_DESC, 2}, {""}};
    _opts.tuple_desc = parquet::Utils::create_tuple_descriptor(&_runtime_state, &_pool, slot_descs);

    auto ret1 = _gen_varchar_runtime_filter_collector(slot_id, false);
    ASSERT_TRUE(ret1.ok());

    _opts.runtime_filters = ret1.value();

    ChunkPredicateBuilder<BoxedExprContext, CompoundNodeType::AND> builder(_opts, _expr_containers, true);

    auto ret2 = builder.parse_conjuncts();
    ASSERT_TRUE(ret1.ok());
    ASSERT_TRUE(ret2.value());

    auto ret3 = builder.get_predicate_tree_root(_int_pred_parser, _predicate_free_pool);
    ASSERT_TRUE(ret3.ok());
    ASSERT_EQ(ret3.value().debug_string(),
              "{\"and\":[{\"pred\":\"(columnId(1)>=111)\"},{\"pred\":\"(columnId(1)<=222)\"}]}");
}

TEST_F(ChunkPredicateBuilderTest, varchar_rt_has_null) {
    SlotId slot_id = 1;
    parquet::Utils::SlotDesc slot_descs[] = {{"c1", TYPE_VARCHAR_DESC, 1}, {"c2", TYPE_VARCHAR_DESC, 2}, {""}};
    _opts.tuple_desc = parquet::Utils::create_tuple_descriptor(&_runtime_state, &_pool, slot_descs);

    auto ret1 = _gen_varchar_runtime_filter_collector(slot_id, true);
    ASSERT_TRUE(ret1.ok());

    _opts.runtime_filters = ret1.value();

    ChunkPredicateBuilder<BoxedExprContext, CompoundNodeType::AND> builder(_opts, _expr_containers, true);

    auto ret2 = builder.parse_conjuncts();
    ASSERT_TRUE(ret1.ok());
    ASSERT_TRUE(ret2.value());

    auto ret3 = builder.get_predicate_tree_root(_int_pred_parser, _predicate_free_pool);
    ASSERT_TRUE(ret3.ok());
    ASSERT_EQ(ret3.value().debug_string(),
              "{\"and\":[{\"or\":[{\"pred\":\"(ColumnId(1) IS "
              "NULL)\"},{\"and\":[{\"pred\":\"(columnId(1)>=111)\"},{\"pred\":\"(columnId(1)<=222)\"}]}]}]}");
}

TEST_F(ChunkPredicateBuilderTest, range_rt_has_null_min) {
    SlotId slot_id = 1;
    parquet::Utils::SlotDesc slot_descs[] = {{"c1", TYPE_INT_DESC, 1}, {"c2", TYPE_INT_DESC, 2}, {""}};
    _opts.tuple_desc = parquet::Utils::create_tuple_descriptor(&_runtime_state, &_pool, slot_descs);

    auto ret1 = _gen_range_runtime_filter_collector<true>(slot_id, true);
    ASSERT_TRUE(ret1.ok());

    _opts.runtime_filters = ret1.value();

    ChunkPredicateBuilder<BoxedExprContext, CompoundNodeType::AND> builder(_opts, _expr_containers, true);

    auto ret2 = builder.parse_conjuncts();
    ASSERT_TRUE(ret1.ok());
    ASSERT_TRUE(ret2.value());

    auto ret3 = builder.get_predicate_tree_root(_int_pred_parser, _predicate_free_pool);
    ASSERT_TRUE(ret3.ok());
    ASSERT_EQ(
            ret3.value().debug_string(),
            "{\"and\":[{\"or\":[{\"pred\":\"(ColumnId(1) IS NULL)\"},{\"and\":[{\"pred\":\"(columnId(1)>10)\"}]}]}]}");
}

TEST_F(ChunkPredicateBuilderTest, range_rt_has_null_max) {
    SlotId slot_id = 1;
    parquet::Utils::SlotDesc slot_descs[] = {{"c1", TYPE_INT_DESC, 1}, {"c2", TYPE_INT_DESC, 2}, {""}};
    _opts.tuple_desc = parquet::Utils::create_tuple_descriptor(&_runtime_state, &_pool, slot_descs);

    auto ret1 = _gen_range_runtime_filter_collector<false>(slot_id, true);
    ASSERT_TRUE(ret1.ok());

    _opts.runtime_filters = ret1.value();

    ChunkPredicateBuilder<BoxedExprContext, CompoundNodeType::AND> builder(_opts, _expr_containers, true);

    auto ret2 = builder.parse_conjuncts();
    ASSERT_TRUE(ret1.ok());
    ASSERT_TRUE(ret2.value());

    auto ret3 = builder.get_predicate_tree_root(_int_pred_parser, _predicate_free_pool);
    ASSERT_TRUE(ret3.ok());
    ASSERT_EQ(
            ret3.value().debug_string(),
            "{\"and\":[{\"or\":[{\"pred\":\"(ColumnId(1) IS NULL)\"},{\"and\":[{\"pred\":\"(columnId(1)<10)\"}]}]}]}");
}

TEST_F(ChunkPredicateBuilderTest, in_runtime_filter_has_null) {
    parquet::Utils::SlotDesc slot_descs[] = {{"c1", TYPE_INT_DESC, 1}, {"c2", TYPE_INT_DESC, 2}, {""}};
    _opts.tuple_desc = parquet::Utils::create_tuple_descriptor(&_runtime_state, &_pool, slot_descs);

    RuntimeFilterProbeCollector collector;
    _opts.runtime_filters = &collector;

    ColumnRef* col_ref = _pool.add(new ColumnRef(TYPE_INT_DESC, 1));
    VectorizedInConstPredicateBuilder builder(&_runtime_state, &_pool, col_ref);
    builder.set_null_in_set(true);
    builder.use_as_join_runtime_filter();
    Status st = builder.create();

    std::vector<int32_t> values{1, 3, 5, 7, 9};
    ColumnPtr col = ColumnTestHelper::build_column(values);
    builder.add_values(col, 0);

    ExprContext* expr_ctx = builder.get_in_const_predicate();

    _expr_containers.emplace_back(BoxedExprContext(expr_ctx));

    ChunkPredicateBuilder<BoxedExprContext, CompoundNodeType::AND> pred_builder(_opts, _expr_containers, true);
    auto ret = pred_builder.parse_conjuncts();
    ASSERT_TRUE(ret.ok());
    ASSERT_TRUE(ret.value());

    auto ret2 = pred_builder.get_predicate_tree_root(_int_pred_parser, _predicate_free_pool);
    ASSERT_TRUE(ret2.ok());
    ASSERT_EQ(ret2.value().debug_string(),
              "{\"and\":[{\"or\":[{\"pred\":\"((columnId=1)IN(9,5,1,7,3))\"},{\"pred\":\"(ColumnId(1) IS NULL)\"}]}]}");
}

TEST_F(ChunkPredicateBuilderTest, in_runtime_filter_has_no_null) {
    parquet::Utils::SlotDesc slot_descs[] = {{"c1", TYPE_INT_DESC, 1}, {"c2", TYPE_INT_DESC, 2}, {""}};
    _opts.tuple_desc = parquet::Utils::create_tuple_descriptor(&_runtime_state, &_pool, slot_descs);

    RuntimeFilterProbeCollector collector;
    _opts.runtime_filters = &collector;

    ColumnRef* col_ref = _pool.add(new ColumnRef(TYPE_INT_DESC, 1));
    VectorizedInConstPredicateBuilder builder(&_runtime_state, &_pool, col_ref);
    builder.use_as_join_runtime_filter();
    Status st = builder.create();

    std::vector<int32_t> values{1, 3, 5, 7, 9};
    ColumnPtr col = ColumnTestHelper::build_column(values);
    builder.add_values(col, 0);

    ExprContext* expr_ctx = builder.get_in_const_predicate();

    _expr_containers.emplace_back(BoxedExprContext(expr_ctx));

    ChunkPredicateBuilder<BoxedExprContext, CompoundNodeType::AND> pred_builder(_opts, _expr_containers, true);
    auto ret = pred_builder.parse_conjuncts();
    ASSERT_TRUE(ret.ok());
    ASSERT_TRUE(ret.value());

    auto ret2 = pred_builder.get_predicate_tree_root(_int_pred_parser, _predicate_free_pool);
    ASSERT_TRUE(ret2.ok());
    ASSERT_EQ(ret2.value().debug_string(), "{\"and\":[{\"pred\":\"((columnId=1)IN(9,5,1,7,3))\"}]}");
}
} // namespace starrocks