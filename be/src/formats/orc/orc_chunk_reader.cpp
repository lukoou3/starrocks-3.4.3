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

#include "formats/orc/orc_chunk_reader.h"

#include <glog/logging.h>

#include <exception>
#include <set>
#include <unordered_map>
#include <utility>

#include "cctz/civil_time.h"
#include "cctz/time_zone.h"
#include "column/array_column.h"
#include "column/vectorized_fwd.h"
#include "exprs/cast_expr.h"
#include "exprs/literal.h"
#include "formats/orc/orc_mapping.h"
#include "formats/orc/orc_memory_pool.h"
#include "formats/orc/utils.h"
#include "gutil/casts.h"
#include "gutil/strings/substitute.h"
#include "orc_schema_builder.h"
#include "simd/simd.h"
#include "types/logical_type.h"
#include "util/stack_util.h"
#include "util/timezone_utils.h"

namespace starrocks {

OrcChunkReader::OrcChunkReader(int chunk_size, std::vector<SlotDescriptor*> src_slot_descriptors)
        : _src_slot_descriptors(std::move(src_slot_descriptors)),
          _read_chunk_size(chunk_size),
          _tzinfo(cctz::utc_time_zone()),
          _tzoffset_in_seconds(0),
          _drop_nanoseconds_in_datetime(false),
          _broker_load_mode(true),
          _strict_mode(true),
          _broker_load_filter(nullptr),
          _num_rows_filtered(0),
          _error_message_counter(0),
          _lazy_load_ctx(nullptr) {
    if (_read_chunk_size == 0) {
        _read_chunk_size = 4096;
    }

    // some caller of `OrcChunkReader` may pass nullptr
    // This happens when there are extra fields in broker load specification
    // but those extra fields don't match any fields in native table.
    // For more details, refer to https://github.com/StarRocks/starrocks/issues/1378
    for (auto iter_slot = _src_slot_descriptors.begin(); iter_slot != _src_slot_descriptors.end(); /*iter_slot++*/) {
        if (*iter_slot == nullptr) {
            iter_slot = _src_slot_descriptors.erase(iter_slot);
        } else {
            ++iter_slot;
        }
    }

    for (size_t i = 0; i < _src_slot_descriptors.size(); i++) {
        _slot_id_to_pos_in_src_slot_descs.emplace(_src_slot_descriptors[i]->id(), i);
    }
}

Status OrcChunkReader::init(std::unique_ptr<orc::InputStream> input_stream, const OrcPredicates* orc_predicates) {
    try {
        _reader_options.setMemoryPool(*getOrcMemoryPool());
        auto reader = orc::createReader(std::move(input_stream), _reader_options);
        RETURN_IF_ERROR(init(std::move(reader), orc_predicates));
    } catch (std::exception& e) {
        auto s = strings::Substitute("OrcChunkReader::init failed. reason = $0, file = $1", e.what(),
                                     _current_file_name);
        LOG(WARNING) << s;
        return Status::InternalError(s);
    }
    return Status::OK();
}

void OrcChunkReader::build_column_name_set(std::unordered_set<std::string>* name_set,
                                           const std::vector<std::string>* hive_column_names,
                                           const orc::Type& root_type, bool case_sensitive, bool use_orc_column_names) {
    name_set->clear();
    if (hive_column_names != nullptr && hive_column_names->size() > 0 && !use_orc_column_names) {
        // build hive column names index.
        int size = std::min(hive_column_names->size(), root_type.getSubtypeCount());
        for (int i = 0; i < size; i++) {
            std::string col_name = Utils::format_name(hive_column_names->at(i), case_sensitive);
            name_set->insert(col_name);
        }
    } else {
        // build orc column names index.
        for (int i = 0; i < root_type.getSubtypeCount(); i++) {
            std::string col_name = Utils::format_name(root_type.getFieldName(i), case_sensitive);
            name_set->insert(col_name);
        }
    }
}

Status OrcChunkReader::_init_include_columns(const std::unique_ptr<OrcMapping>& mapping) {
    std::list<uint64_t> include_column_id;

    // NOTICE: No need to explicit include root column id, otherwise it will read out all fields.
    // Include root column id.
    // include_column_id.emplace_back(0);
    for (size_t i = 0; i < _src_slot_descriptors.size(); i++) {
        SlotDescriptor* desc = _src_slot_descriptors[i];
        // column not existed
        if (!mapping->contains(i)) continue;
        RETURN_IF_ERROR(mapping->set_include_column_id(i, desc->type(), &include_column_id));
    }

    _row_reader_options.includeTypes(include_column_id);

    if (_lazy_load_ctx != nullptr) {
        std::list<uint64_t> lazy_load_column_id;

        for (size_t pos : _lazy_load_ctx->lazy_load_indices) {
            if (!mapping->contains(pos)) continue;
            RETURN_IF_ERROR(mapping->set_lazy_load_column_id(pos, &lazy_load_column_id));
        }

        _row_reader_options.includeLazyLoadColumnIndexes(lazy_load_column_id);
    }

    return Status::OK();
}

const std::vector<bool>& OrcChunkReader::TEST_get_selected_column_id_list() {
    return _row_reader->getSelectedColumns();
}

const std::vector<bool>& OrcChunkReader::TEST_get_lazyload_column_id_list() {
    return _row_reader->getLazyLoadColumns();
}

Status OrcChunkReader::init(std::unique_ptr<orc::Reader> reader, const OrcPredicates* orc_predicates) {
    _reader = std::move(reader);
    // ORC writes empty schema (struct<>) to ORC files containing zero rows.
    // Hive 0.12
    if (_reader->getNumberOfRows() == 0) {
        return Status::EndOfFile("number of rows is 0");
    }

    if (_hive_column_names == nullptr) {
        // If hive_column_names is nullptr, we have to use orc's column name.
        set_use_orc_column_names(true);
    }

    // Build root_mapping, including all columns in orc.
    _orc_mapping_options.filename = _current_file_name;
    _orc_mapping_options.case_sensitive = _case_sensitive;
    _orc_mapping_options.invalid_as_null = _invalid_as_null;
    ASSIGN_OR_RETURN(_root_mapping,
                     OrcMappingFactory::build_mapping(_src_slot_descriptors, _reader->getType(), _use_orc_column_names,
                                                      _hive_column_names, _orc_mapping_options));
    DCHECK(_root_mapping != nullptr);
    RETURN_IF_ERROR(_init_include_columns(_root_mapping));
    RETURN_IF_ERROR(_init_src_types(_root_mapping));

    if (orc_predicates != nullptr) {
        RETURN_IF_ERROR(build_search_argument_by_predicates(orc_predicates));
    }

    // ensure search argument is not null.
    // we are going to put row reader filter into search argument applier
    // and search argument applier only be constructed when search argument is not null.
    if (_row_reader_options.getSearchArgument() == nullptr) {
        std::unique_ptr<orc::SearchArgumentBuilder> builder = orc::SearchArgumentFactory::newBuilder();
        builder->literal(orc::TruthValue::YES_NO_NULL);
        _row_reader_options.searchArgument(builder->build());
    }

    try {
        _row_reader = _reader->createRowReader(_row_reader_options);
    } catch (std::exception& e) {
        auto s = strings::Substitute("OrcChunkReader::init failed. reason = $0, file = $1", e.what(),
                                     _current_file_name);
        LOG(WARNING) << s;
        return Status::InternalError(s);
    }

    // _batch can't be reused because the schema between files may be different
    _batch.reset();

    // TODO(SmithCruise) delete _init_position_in_orc() when develop subfield lazy load.
    RETURN_IF_ERROR(_init_position_in_orc());
    RETURN_IF_ERROR(_init_cast_exprs());
    RETURN_IF_ERROR(_init_column_readers());
    return Status::OK();
}

Status OrcChunkReader::_init_position_in_orc() const {
    const size_t column_size = _src_slot_descriptors.size();
    std::vector<int32_t> position_in_orc_vector_batch;
    position_in_orc_vector_batch.resize(column_size);

    std::unordered_map<uint64_t, size_t> column_id_to_pos_in_vector_batch;

    const auto& type = _row_reader->getSelectedType();
    for (size_t i = 0; i < type.getSubtypeCount(); i++) {
        const auto& sub_type = type.getSubtype(i);
        uint64_t col_id = sub_type->getColumnId();
        column_id_to_pos_in_vector_batch.emplace(col_id, i);
    }

    for (int i = 0; i < column_size; i++) {
        if (!_root_mapping->contains(i)) {
            // slot column name didn't existed in orc file,  set -1 for it.
            position_in_orc_vector_batch[i] = -1;
            continue;
        }

        uint64_t column_id = _root_mapping->get_orc_type_child_mapping(i).orc_type->getColumnId();
        auto it2 = column_id_to_pos_in_vector_batch.find(column_id);
        if (it2 == column_id_to_pos_in_vector_batch.end()) {
            auto s = strings::Substitute(
                    "OrcChunkReader::init_position_in_orc. failed to find position. col_id = $0, file = $1",
                    std::to_string(column_id), _current_file_name);
            return Status::NotFound(s);
        }
        position_in_orc_vector_batch[i] = it2->second;
    }

    if (_lazy_load_ctx != nullptr) {
        for (int i = 0; i < _lazy_load_ctx->active_load_slots.size(); i++) {
            int src_index = _lazy_load_ctx->active_load_indices[i];
            int pos = position_in_orc_vector_batch[src_index];
            _lazy_load_ctx->active_load_orc_positions[i] = pos;
        }
        for (int i = 0; i < _lazy_load_ctx->lazy_load_slots.size(); i++) {
            int src_index = _lazy_load_ctx->lazy_load_indices[i];
            int pos = position_in_orc_vector_batch[src_index];
            _lazy_load_ctx->lazy_load_orc_positions[i] = pos;
        }
    }
    return Status::OK();
}

static Status _create_type_descriptor_by_orc(const TypeDescriptor& origin_type, const orc::Type* orc_type,
                                             const OrcMappingPtr& mapping, TypeDescriptor* result) {
    orc::TypeKind kind = orc_type->getKind();
    switch (kind) {
    case orc::LIST:
        [[fallthrough]];
    case orc::MAP:
        [[fallthrough]];
    case orc::STRUCT:
        if (mapping == nullptr) {
            return Status::InvalidArgument(strings::Substitute("orc mapping is null in $0", orc_type->toString()));
        }
        break;
    default:
        break;
    }

    if (kind == orc::LIST) {
        result->type = TYPE_ARRAY;
        DCHECK_EQ(0, result->children.size());
        result->children.emplace_back();

        TypeDescriptor& element_type = result->children.back();
        RETURN_IF_ERROR(_create_type_descriptor_by_orc(origin_type.children.at(0), orc_type->getSubtype(0),
                                                       mapping->get_orc_type_child_mapping(0).orc_mapping,
                                                       &element_type));
    } else if (kind == orc::MAP) {
        result->type = TYPE_MAP;
        DCHECK_EQ(0, result->children.size());
        TypeDescriptor& key_type = result->children.emplace_back();
        if (origin_type.children[0].is_unknown_type()) {
            key_type.type = TYPE_UNKNOWN;
        } else {
            RETURN_IF_ERROR(_create_type_descriptor_by_orc(origin_type.children.at(0), orc_type->getSubtype(0),
                                                           mapping->get_orc_type_child_mapping(0).orc_mapping,
                                                           &key_type));
        }

        TypeDescriptor& value_type = result->children.emplace_back();
        if (origin_type.children[1].is_unknown_type()) {
            value_type.type = TYPE_UNKNOWN;
        } else {
            RETURN_IF_ERROR(_create_type_descriptor_by_orc(origin_type.children.at(1), orc_type->getSubtype(1),
                                                           mapping->get_orc_type_child_mapping(1).orc_mapping,
                                                           &value_type));
        }
    } else if (kind == orc::STRUCT) {
        DCHECK_EQ(0, result->children.size());

        result->type = TYPE_STRUCT;

        size_t field_size = origin_type.children.size();

        // We need copy the all struct subfields
        for (size_t index = 0; index < field_size; index++) {
            result->field_names.emplace_back(origin_type.field_names[index]);
            TypeDescriptor& sub_field_type = result->children.emplace_back();
            RETURN_IF_ERROR(_create_type_descriptor_by_orc(
                    origin_type.children.at(index), mapping->get_orc_type_child_mapping(index).orc_type,
                    mapping->get_orc_type_child_mapping(index).orc_mapping, &sub_field_type));
        }
    } else {
        auto precision = (int)orc_type->getPrecision();
        auto scale = (int)orc_type->getScale();
        auto len = (int)orc_type->getMaximumLength();
        auto iter = g_orc_starrocks_logical_type_mapping.find(kind);
        if (iter == g_orc_starrocks_logical_type_mapping.end()) {
            return Status::NotSupported("Unsupported ORC type: " + orc_type->toString());
        }
        result->type = iter->second;
        result->len = len;
        result->precision = precision;
        result->scale = scale;
        // To support iceberg table time type
        // When orc type is bigint and orc attribute iceberg.long-type is TIME, then convert result type to TYPE_TIME
        if (result->type == TYPE_BIGINT && orc_type->hasAttributeKey("iceberg.long-type")) {
            if ("TIME" == orc_type->getAttributeValue("iceberg.long-type")) {
                result->type = TYPE_TIME;
            }
        }
    }
    return Status::OK();
}

void OrcChunkReader::_try_implicit_cast(TypeDescriptor* from, const TypeDescriptor& to) {
    auto is_integer_type = [](LogicalType t) { return g_starrocks_int_type.count(t) > 0; };
    auto is_decimal_type = [](LogicalType t) { return g_starrocks_decimal_type.count(t) > 0; };

    LogicalType t1 = from->type;
    LogicalType t2 = to.type;
    if (t1 == LogicalType::TYPE_ARRAY && t2 == LogicalType::TYPE_ARRAY) {
        _try_implicit_cast(&from->children[0], to.children[0]);
    } else if (t1 == LogicalType::TYPE_MAP && t2 == LogicalType::TYPE_MAP) {
        _try_implicit_cast(&from->children.at(0), to.children.at(0));
        _try_implicit_cast(&from->children.at(1), to.children.at(1));
    } else if (t1 == LogicalType::TYPE_STRUCT && t2 == LogicalType::TYPE_STRUCT) {
        DCHECK_EQ(from->children.size(), to.children.size());
        size_t field_size = from->children.size();
        for (size_t i = 0; i < field_size; i++) {
            _try_implicit_cast(&from->children.at(i), to.children.at(i));
        }
    } else if (is_integer_type(t1) && is_integer_type(t2)) {
        from->type = t2;
    } else if (is_decimal_type(t1) && is_decimal_type(t2)) {
        // if target type is decimal v3 type, the from->type should be assigned to a correct
        // logical type according to the precision in original orc files so that the invariant
        // 0 <= scale <= precision <= decimal_precision_limit<RuntimeCppType<from_type>> is not
        // violated during creating a DecimalV3Column via ColumnHelper::create(...).
        if (t2 == LogicalType::TYPE_DECIMALV2) {
            from->type = t2;
        } else if (from->precision > decimal_precision_limit<int64_t>) {
            from->type = LogicalType::TYPE_DECIMAL128;
        } else if (from->precision > decimal_precision_limit<int32_t> || (to.type == LogicalType::TYPE_DECIMAL64)) {
            from->type = LogicalType::TYPE_DECIMAL64;
        } else {
            from->type = LogicalType::TYPE_DECIMAL32;
        }
    } else if (_broker_load_mode && is_string_type(t1) && is_string_type(t2)) {
        // For broker load, the orc field length is larger than the maximum length of the starrocks field
        // will cause load failure in non-strict mode. Here we keep the maximum length of the orc field
        // the same as the maximum length of the starrocks field.
        from->len = to.len;
    } else {
        // nothing to do.
    }
}

Status OrcChunkReader::_init_src_types(const std::unique_ptr<OrcMapping>& mapping) {
    int column_size = _src_slot_descriptors.size();
    // update source types.
    _src_types.clear();
    _src_types.resize(column_size);
    for (int i = 0; i < column_size; i++) {
        auto slot_desc = _src_slot_descriptors[i];
        if (!mapping->contains(i)) {
            // target column didn't existed in orc file, so we just using column's type directly
            _src_types[i] = slot_desc->type();
            continue;
        }
        const orc::Type* orc_type = mapping->get_orc_type_child_mapping(i).orc_type;
        RETURN_IF_ERROR(_create_type_descriptor_by_orc(
                slot_desc->type(), orc_type, mapping->get_orc_type_child_mapping(i).orc_mapping, &_src_types[i]));
        _try_implicit_cast(&_src_types[i], slot_desc->type());
    }
    return Status::OK();
}

Status OrcChunkReader::_init_cast_exprs() {
    int column_size = _src_slot_descriptors.size();
    _cast_exprs.clear();
    _cast_exprs.resize(column_size);
    _pool.clear();

    for (int column_pos = 0; column_pos < column_size; ++column_pos) {
        auto slot_desc = _src_slot_descriptors[column_pos];
        auto& orc_type = _src_types[column_pos];
        auto& starrocks_type = slot_desc->type();
        Expr* slot = _pool.add(new ColumnRef(slot_desc));
        if (starrocks_type.is_assignable(orc_type) || !_root_mapping->contains(column_pos)) {
            // if column didn't existed in orc file, we don't need case_expr anymore, using ColumnRef directly
            _cast_exprs[column_pos] = slot;
            continue;
        }
        Expr* cast = VectorizedCastExprFactory::from_type(orc_type, starrocks_type, slot, &_pool);
        if (cast == nullptr) {
            return Status::InternalError(strings::Substitute("Not support cast $0 to $1. file = $2",
                                                             orc_type.debug_string(), starrocks_type.debug_string(),
                                                             _current_file_name));
        }
        _cast_exprs[column_pos] = cast;
    }
    return Status::OK();
}

Status OrcChunkReader::_init_column_readers() {
    const size_t column_size = _src_slot_descriptors.size();
    _column_readers.clear();

    for (size_t column_pos = 0; column_pos < column_size; ++column_pos) {
        const auto& slot_desc = _src_slot_descriptors[column_pos];
        std::unique_ptr<ORCColumnReader> column_reader;
        if (_root_mapping->contains(column_pos)) {
            ASSIGN_OR_RETURN(
                    column_reader,
                    ORCColumnReader::create(_src_types[column_pos],
                                            _root_mapping->get_orc_type_child_mapping(column_pos).orc_type,
                                            slot_desc->is_nullable(),
                                            _root_mapping->get_orc_type_child_mapping(column_pos).orc_mapping, this));
        } else {
            ASSIGN_OR_RETURN(column_reader, ORCColumnReader::create_default_column_reader(
                                                    _src_types[column_pos], slot_desc->is_nullable(), this));
        }
        _column_readers.emplace_back(std::move(column_reader));
    }
    DCHECK_EQ(_column_readers.size(), column_size);
    return Status::OK();
}

Status OrcChunkReader::read_next(orc::RowReader::ReadPosition* pos) {
    if (_batch == nullptr) {
        _batch = _row_reader->createRowBatch(_read_chunk_size);
    }
    try {
        if (!_row_reader->next(*_batch, pos)) {
            return Status::EndOfFile("");
        }
    } catch (std::exception& e) {
        LOG(WARNING) << get_stack_trace();
        auto s = strings::Substitute("ORC reader read file $0 failed. Reason is $1.", _current_file_name, e.what());
        LOG(WARNING) << s;
        return Status::InternalError(s);
    }
    return Status::OK();
}

size_t OrcChunkReader::get_cvb_size() {
    return _batch->numElements;
}

Status OrcChunkReader::_fill_chunk(ChunkPtr* chunk, const std::vector<SlotDescriptor*>& src_slot_descriptors,
                                   const std::vector<int>* indices) {
    int column_size = src_slot_descriptors.size();
    DCHECK_GT(_batch->numElements, 0);
    const auto& batch_vec = down_cast<orc::StructVectorBatch*>(_batch.get());
    if (_broker_load_mode) {
        // always allocate load filter. it's much easier to use in fill chunk function.
        if (_broker_load_filter == nullptr) {
            _broker_load_filter = std::make_shared<Filter>(_read_chunk_size);
        }
        _broker_load_filter->assign(_batch->numElements, 1);
    }
    for (int column_pos = 0; column_pos < column_size; ++column_pos) {
        SlotDescriptor* slot_desc = src_slot_descriptors[column_pos];
        if (slot_desc == nullptr) {
            continue;
        }
        int src_index = column_pos;
        if (indices != nullptr) {
            src_index = (*indices)[src_index];
        }
        set_current_slot(slot_desc);

        orc::ColumnVectorBatch* cvb = nullptr;
        if (_root_mapping->contains(src_index)) {
            cvb = batch_vec->fieldsColumnIdMap[_root_mapping->get_orc_type_child_mapping(src_index)
                                                       .orc_type->getColumnId()];
            if (!slot_desc->is_nullable() && cvb->hasNulls) {
                if (_broker_load_mode) {
                    std::string error_msg =
                            strings::Substitute("NULL value in non-nullable column '$0'", _current_slot->col_name());
                    report_error_message(error_msg);
                    bool all_zero = false;
                    ColumnHelper::merge_two_filters(_broker_load_filter.get(),
                                                    reinterpret_cast<uint8_t*>(cvb->notNull.data()), &all_zero);
                    if (all_zero) {
                        (*chunk)->set_num_rows(0);
                        break;
                    }
                } else {
                    auto s = strings::Substitute("column '$0' is not nullable", slot_desc->col_name());
                    return Status::InternalError(s);
                }
            }
        }
        ColumnPtr& col = (*chunk)->get_column_by_slot_id(slot_desc->id());
        RETURN_IF_ERROR(_column_readers[src_index]->get_next(cvb, col, 0, _batch->numElements));
    }

    if (_broker_load_mode) {
        if ((*chunk)->num_rows() != 0) {
            size_t zero_count = SIMD::count_zero(_broker_load_filter->data(), _broker_load_filter->size());
            if (zero_count != 0) {
                _num_rows_filtered = zero_count;
                (*chunk)->filter(*_broker_load_filter);
            }
        } else {
            _num_rows_filtered = _broker_load_filter->size();
        }
    }

    return Status::OK();
}

ChunkPtr OrcChunkReader::_create_chunk(const std::vector<SlotDescriptor*>& src_slot_descriptors,
                                       const std::vector<int>* indices) {
    auto chunk = std::make_shared<Chunk>();
    int column_size = src_slot_descriptors.size();
    chunk->columns().reserve(column_size);

    for (int column_pos = 0; column_pos < column_size; ++column_pos) {
        auto slot_desc = src_slot_descriptors[column_pos];
        if (slot_desc == nullptr) {
            continue;
        }
        int src_index = column_pos;
        if (indices != nullptr) {
            src_index = (*indices)[src_index];
        }
        auto col = ColumnHelper::create_column(_src_types[src_index], slot_desc->is_nullable());
        chunk->append_column(std::move(col), slot_desc->id());
    }
    return chunk;
}

StatusOr<ChunkPtr> OrcChunkReader::_cast_chunk(ChunkPtr* chunk,
                                               const std::vector<SlotDescriptor*>& src_slot_descriptors,
                                               const std::vector<int>* indices) {
    ChunkPtr& src = (*chunk);
    size_t chunk_size = src->num_rows();
    ChunkPtr cast_chunk = std::make_shared<Chunk>();
    int column_size = src_slot_descriptors.size();
    for (int column_pos = 0; column_pos < column_size; ++column_pos) {
        auto slot = src_slot_descriptors[column_pos];
        if (slot == nullptr) {
            continue;
        }
        int src_index = column_pos;
        if (indices != nullptr) {
            src_index = (*indices)[src_index];
        }
        // TODO(murphy) check status
        ASSIGN_OR_RETURN(ColumnPtr col, _cast_exprs[src_index]->evaluate_checked(nullptr, src.get()));
        col = ColumnHelper::unfold_const_column(slot->type(), chunk_size, col);

        // If we feed nullable column to cast_expr, it may return non-nullable column if it really doesn't have null values
        if (slot->is_nullable()) {
            // wrap nullable column if necessary
            col = NullableColumn::wrap_if_necessary(col);
        }

        DCHECK_LE(col->size(), chunk_size);
        cast_chunk->append_column(std::move(col), slot->id());
    }
    return cast_chunk;
}

ChunkPtr OrcChunkReader::create_chunk() {
    return _create_chunk(_src_slot_descriptors, nullptr);
}
Status OrcChunkReader::fill_chunk(ChunkPtr* chunk) {
    return _fill_chunk(chunk, _src_slot_descriptors, nullptr);
}

StatusOr<ChunkPtr> OrcChunkReader::cast_chunk_checked(ChunkPtr* chunk) {
    return _cast_chunk(chunk, _src_slot_descriptors, nullptr);
}

StatusOr<ChunkPtr> OrcChunkReader::get_chunk() {
    ChunkPtr ptr = create_chunk();
    RETURN_IF_ERROR(fill_chunk(&ptr));
    return cast_chunk_checked(&ptr);
}

StatusOr<ChunkPtr> OrcChunkReader::get_active_chunk() {
    ChunkPtr ptr = _create_chunk(_lazy_load_ctx->active_load_slots, &_lazy_load_ctx->active_load_indices);
    RETURN_IF_ERROR(_fill_chunk(&ptr, _lazy_load_ctx->active_load_slots, &_lazy_load_ctx->active_load_indices));
    return _cast_chunk(&ptr, _lazy_load_ctx->active_load_slots, &_lazy_load_ctx->active_load_indices);
}

void OrcChunkReader::lazy_filter_on_cvb(Filter* filter) {
    size_t true_size = SIMD::count_nonzero(*filter);
    if (filter->size() != true_size) {
        _batch->filterOnFields(filter->data(), filter->size(), true_size, _lazy_load_ctx->lazy_load_orc_positions,
                               true);
    }
}

StatusOr<ChunkPtr> OrcChunkReader::get_lazy_chunk() {
    ChunkPtr ptr = _create_chunk(_lazy_load_ctx->lazy_load_slots, &_lazy_load_ctx->lazy_load_indices);
    RETURN_IF_ERROR(_fill_chunk(&ptr, _lazy_load_ctx->lazy_load_slots, &_lazy_load_ctx->lazy_load_indices));
    return _cast_chunk(&ptr, _lazy_load_ctx->lazy_load_slots, &_lazy_load_ctx->lazy_load_indices);
}

Status OrcChunkReader::lazy_read_next(size_t numValues) {
    try {
        // It may throw orc::ParseError exception
        _row_reader->lazyLoadNext(*_batch, numValues);
    } catch (std::exception& e) {
        auto s = strings::Substitute("OrcChunkReader::lazy_read_next failed. reason = $0, file = $1", e.what(),
                                     _current_file_name);
        LOG(WARNING) << s;
        return Status::InternalError(s);
    }
    return Status::OK();
}

Status OrcChunkReader::lazy_seek_to(size_t rowInStripe) {
    try {
        // It may throw orc::ParseError exception
        _row_reader->lazyLoadSeekTo(rowInStripe);
    } catch (std::exception& e) {
        auto s = strings::Substitute("OrcChunkReader::lazy_seek_to failed. reason = $0, file = $1", e.what(),
                                     _current_file_name);
        LOG(WARNING) << s;
        return Status::InternalError(s);
    }
    return Status::OK();
}

void OrcChunkReader::set_row_reader_filter(std::shared_ptr<orc::RowReaderFilter> filter) {
    _row_reader_options.rowReaderFilter(std::move(filter));
}

static const std::unordered_set<TExprOpcode::type> _supported_binary_ops = {
        TExprOpcode::EQ,          TExprOpcode::NE,        TExprOpcode::LT,
        TExprOpcode::LE,          TExprOpcode::GT,        TExprOpcode::GE,
        TExprOpcode::EQ_FOR_NULL, TExprOpcode::FILTER_IN, TExprOpcode::FILTER_NOT_IN,
};

static const std::unordered_set<TExprNodeType::type> _supported_literal_types = {
        TExprNodeType::type::BOOL_LITERAL,   TExprNodeType::type::DATE_LITERAL,      TExprNodeType::type::FLOAT_LITERAL,
        TExprNodeType::type::INT_LITERAL,    TExprNodeType::type::DECIMAL_LITERAL,   TExprNodeType::type::NULL_LITERAL,
        TExprNodeType::type::STRING_LITERAL, TExprNodeType::type::LARGE_INT_LITERAL,
};

static const std::unordered_set<TExprNodeType::type> _supported_expr_node_types = {
        // predicates
        TExprNodeType::type::COMPOUND_PRED,
        TExprNodeType::type::BINARY_PRED,
        TExprNodeType::type::IN_PRED,
        TExprNodeType::type::IS_NULL_PRED,
        // is null & is not null
        TExprNodeType::type::FUNCTION_CALL,
        // literal & slot ref
        TExprNodeType::type::BOOL_LITERAL,
        TExprNodeType::type::DATE_LITERAL,
        TExprNodeType::type::FLOAT_LITERAL,
        TExprNodeType::type::INT_LITERAL,
        TExprNodeType::type::DECIMAL_LITERAL,
        TExprNodeType::type::NULL_LITERAL,
        TExprNodeType::type::SLOT_REF,
        TExprNodeType::type::STRING_LITERAL,
        TExprNodeType::type::LARGE_INT_LITERAL,
};

static const std::unordered_map<LogicalType, orc::PredicateDataType> _supported_logical_types = {
        {LogicalType::TYPE_BOOLEAN, orc::PredicateDataType::BOOLEAN},
        {LogicalType::TYPE_TINYINT, orc::PredicateDataType::LONG},
        {LogicalType::TYPE_SMALLINT, orc::PredicateDataType::LONG},
        {LogicalType::TYPE_INT, orc::PredicateDataType::LONG},
        {LogicalType::TYPE_BIGINT, orc::PredicateDataType::LONG},
        // TYPE_LARGEINT, /* 7 */
        {LogicalType::TYPE_FLOAT, orc::PredicateDataType::FLOAT},
        {LogicalType::TYPE_DOUBLE, orc::PredicateDataType::FLOAT},
        {LogicalType::TYPE_VARCHAR, orc::PredicateDataType::STRING},
        {LogicalType::TYPE_DATE, orc::PredicateDataType::DATE},
        //TYPE_DATETIME, /* 12 */
        {LogicalType::TYPE_BINARY, orc::PredicateDataType::STRING},
        {LogicalType::TYPE_VARBINARY, orc::PredicateDataType::STRING},
        {LogicalType::TYPE_CHAR, orc::PredicateDataType::STRING},
        {LogicalType::TYPE_DECIMALV2, orc::PredicateDataType::DECIMAL},
        // TYPE_TIME,       /* 21 */
        {LogicalType::TYPE_DECIMAL32, orc::PredicateDataType::DECIMAL},
        {LogicalType::TYPE_DECIMAL64, orc::PredicateDataType::DECIMAL},
        {LogicalType::TYPE_DECIMAL128, orc::PredicateDataType::DECIMAL},
};

bool OrcChunkReader::_ok_to_add_compound_conjunct(
        const Expr* conjunct, const std::unordered_map<SlotId, size_t>& slot_id_to_pos_in_src_slot_descriptors) {
    DCHECK_EQ(TExprNodeType::COMPOUND_PRED, conjunct->node_type());
    const TExprOpcode::type& op_type = conjunct->op();

    if (!(op_type == TExprOpcode::COMPOUND_AND || op_type == TExprOpcode::COMPOUND_NOT ||
          op_type == TExprOpcode::COMPOUND_OR)) {
        return false;
    }

    for (const Expr* c : conjunct->children()) {
        if (!_ok_to_add_conjunct(c, slot_id_to_pos_in_src_slot_descriptors)) {
            return false;
        }
    }
    return true;
}

bool OrcChunkReader::_ok_to_add_binary_in_conjunct(
        const Expr* conjunct, const std::unordered_map<SlotId, size_t>& slot_id_to_pos_in_src_slot_descriptors) {
    const TExprNodeType::type& node_type = conjunct->node_type();
    const TExprOpcode::type& op_type = conjunct->op();
    DCHECK(node_type == TExprNodeType::BINARY_PRED || node_type == TExprNodeType::IN_PRED);

    if (!_supported_binary_ops.contains(op_type)) {
        return false;
    }

    // first child should be slot
    // and others should be literal.
    Expr* c = conjunct->get_child(0);
    if (c->node_type() != TExprNodeType::type::SLOT_REF) {
        return false;
    }
    const SlotId slot_id = down_cast<ColumnRef*>(c)->slot_id();

    // slot can't be not found.
    const auto& iter = slot_id_to_pos_in_src_slot_descriptors.find(slot_id);
    if (iter == slot_id_to_pos_in_src_slot_descriptors.end()) {
        return false;
    }
    if (!_root_mapping->contains(iter->second)) {
        // column not existed in this orc file
        return false;
    }

    const SlotDescriptor* slot_desc = _src_slot_descriptors[iter->second];
    // It's unsafe to do eval on char type because of padding problems.
    if (slot_desc->type().type == TYPE_CHAR) {
        return false;
    }

    if (conjunct->get_num_children() == 1) {
        return false;
    }

    for (int i = 1; i < conjunct->get_num_children(); i++) {
        c = conjunct->get_child(i);
        if (!_supported_literal_types.contains(c->node_type())) {
            return false;
        }
    }
    for (int i = 0; i < conjunct->get_num_children(); i++) {
        if (!_supported_logical_types.contains(conjunct->get_child(i)->type().type)) {
            return false;
        }
    }

    return true;
}

bool OrcChunkReader::_ok_to_add_is_null_conjunct(
        const Expr* conjunct, const std::unordered_map<SlotId, size_t>& slot_id_to_pos_in_src_slot_descriptors) {
    const TExprNodeType::type& node_type = conjunct->node_type();

    DCHECK(node_type == TExprNodeType::IS_NULL_PRED || node_type == TExprNodeType::FUNCTION_CALL);

    // first child should be slot
    // and others should be literal.
    Expr* c = conjunct->get_child(0);
    if (c->node_type() != TExprNodeType::type::SLOT_REF) {
        return false;
    }
    const SlotId slot_id = down_cast<ColumnRef*>(c)->slot_id();

    // slot can't be not found.
    const auto& iter = slot_id_to_pos_in_src_slot_descriptors.find(slot_id);
    if (iter == slot_id_to_pos_in_src_slot_descriptors.end()) {
        return false;
    }
    if (!_root_mapping->contains(iter->second)) {
        // column not existed in this orc file
        return false;
    }

    if (node_type == TExprNodeType::FUNCTION_CALL) {
        // check is null & is not null
        std::string null_str;
        if (!conjunct->is_null_scalar_function(null_str)) {
            return false;
        }
    }

    return true;
}

bool OrcChunkReader::_ok_to_add_conjunct(
        const Expr* conjunct, const std::unordered_map<SlotId, size_t>& slot_id_to_pos_in_src_slot_descriptors) {
    const TExprNodeType::type& node_type = conjunct->node_type();
    if (!_supported_expr_node_types.contains(node_type)) {
        return false;
    }

    // compound pred: and, or not.
    if (node_type == TExprNodeType::COMPOUND_PRED) {
        return _ok_to_add_compound_conjunct(conjunct, slot_id_to_pos_in_src_slot_descriptors);
    }

    // binary pred: EQ, NE, LT etc.
    if (node_type == TExprNodeType::BINARY_PRED || node_type == TExprNodeType::IN_PRED) {
        return _ok_to_add_binary_in_conjunct(conjunct, slot_id_to_pos_in_src_slot_descriptors);
    }

    if (node_type == TExprNodeType::IS_NULL_PRED || node_type == TExprNodeType::FUNCTION_CALL) {
        return _ok_to_add_is_null_conjunct(conjunct, slot_id_to_pos_in_src_slot_descriptors);
    }
    // TODO We can't pushdown `select * from tbl where col` now. Because conjunct is a cast expr here
    return false;
}

static inline orc::Int128 to_orc128(int128_t value) {
    return {int64_t(value >> 64), uint64_t(value)};
}

static StatusOr<orc::Literal> translate_to_orc_literal(Expr* lit, orc::PredicateDataType pred_type) {
    TExprNodeType::type node_type = lit->node_type();
    LogicalType ltype = lit->type().type;
    if (node_type == TExprNodeType::type::NULL_LITERAL) {
        return {pred_type};
    }

    auto* vlit = down_cast<VectorizedLiteral*>(lit);
    ASSIGN_OR_RETURN(auto ptr, vlit->evaluate_checked(nullptr, nullptr));
    if (ptr->only_null()) {
        return {pred_type};
    }

    const Datum& datum = ptr->get(0);
    switch (ltype) {
    case LogicalType::TYPE_BOOLEAN:
        return {bool(datum.get_int8())};
    case LogicalType::TYPE_TINYINT:
        return {int64_t(datum.get_int8())};
    case LogicalType::TYPE_SMALLINT:
        return {int64_t(datum.get_int16())};
    case LogicalType::TYPE_INT:
        return {int64_t(datum.get_int32())};
    case LogicalType::TYPE_BIGINT:
        return {datum.get_int64()};
    case LogicalType::TYPE_FLOAT:
        return {double(datum.get_float())};
    case LogicalType::TYPE_DOUBLE:
        return {datum.get_double()};
    case LogicalType::TYPE_VARCHAR:
    case LogicalType::TYPE_CHAR:
    case LogicalType::TYPE_VARBINARY:
    case LogicalType::TYPE_BINARY: {
        const Slice& slice = datum.get_slice();
        return orc::Literal{slice.data, slice.size};
    }
    case LogicalType::TYPE_DATE:
        return orc::Literal{orc::PredicateDataType::DATE, OrcDateHelper::native_date_to_orc_date(datum.get_date())};
    case LogicalType::TYPE_DECIMAL:
    case LogicalType::TYPE_DECIMALV2: {
        const DecimalV2Value& value = datum.get_decimal();
        return orc::Literal{to_orc128(value.value()), value.PRECISION, value.SCALE};
    }
    case LogicalType::TYPE_DECIMAL32:
        return orc::Literal{orc::Int128(datum.get_int32()), lit->type().precision, lit->type().scale};
    case LogicalType::TYPE_DECIMAL64:
        return orc::Literal{orc::Int128(datum.get_int64()), lit->type().precision, lit->type().scale};
    case LogicalType::TYPE_DECIMAL128:
        return orc::Literal{to_orc128(datum.get_int128()), lit->type().precision, lit->type().scale};
    default:
        DCHECK(false);
    }
    LOG(WARNING) << "failed to handle logical type = " << std::to_string(ltype) << " in translate_to_orc_literal()";
    return Status::InternalError("failed to handle logical type = " + std::to_string(ltype));
}

Status OrcChunkReader::_add_conjunct(const Expr* conjunct,
                                     const std::unordered_map<SlotId, size_t>& slot_id_to_pos_in_src_slot_descriptors,
                                     std::unique_ptr<orc::SearchArgumentBuilder>& builder) {
    const TExprNodeType::type& node_type = conjunct->node_type();
    const TExprOpcode::type& op_type = conjunct->op();

    // If conjunct is slot ref, like SELECT * FROM tbl where col;
    // We build SearchArgument about col=true directly.
    if (node_type == TExprNodeType::type::SLOT_REF) {
        auto* ref = down_cast<const ColumnRef*>(conjunct);
        DCHECK(conjunct->type().type == LogicalType::TYPE_BOOLEAN);
        const SlotId& slot_id = ref->slot_id();
        const uint64_t column_id =
                _root_mapping->get_orc_type_child_mapping(slot_id_to_pos_in_src_slot_descriptors.at(slot_id))
                        .orc_type->getColumnId();
        builder->equals(column_id, orc::PredicateDataType::BOOLEAN, true);
        return Status::OK();
    }

    if (node_type == TExprNodeType::type::COMPOUND_PRED) {
        if (op_type == TExprOpcode::COMPOUND_AND) {
            builder->startAnd();
        } else if (op_type == TExprOpcode::COMPOUND_OR) {
            builder->startOr();
        } else if (op_type == TExprOpcode::COMPOUND_NOT) {
            builder->startNot();
        } else {
            return Status::InternalError("unexpected op_type in compound_pred type. op_type = " +
                                         std::to_string(op_type));
        }
        for (Expr* c : conjunct->children()) {
            RETURN_IF_ERROR(_add_conjunct(c, slot_id_to_pos_in_src_slot_descriptors, builder));
        }
        builder->end();
        return Status::OK();
    }

    // handle conjuncts
    // where (NULL) or (slot == $val)
    // where (true) or (slot == $val)
    // If FE can simplify this predicate, then literal processing is no longer needed here
    if (node_type == TExprNodeType::BOOL_LITERAL || node_type == TExprNodeType::NULL_LITERAL) {
        orc::TruthValue val = orc::TruthValue::NO;
        if (node_type == TExprNodeType::BOOL_LITERAL) {
            Expr* literal = const_cast<Expr*>(conjunct);
            auto ptr = literal->evaluate_checked(nullptr, nullptr).value();
            const Datum& datum = ptr->get(0);
            if (datum.get_int8()) {
                val = orc::TruthValue::YES;
            }
        }
        builder->literal(val);
        return Status::OK();
    }

    Expr* slot = conjunct->get_child(0);
    DCHECK(slot->is_slotref());
    const SlotId& slot_id = down_cast<ColumnRef*>(slot)->slot_id();
    const size_t pos = slot_id_to_pos_in_src_slot_descriptors.at(slot_id);
    const uint64_t column_id = _root_mapping->get_orc_type_child_mapping(pos).orc_type->getColumnId();

    if (node_type == TExprNodeType::IS_NULL_PRED || node_type == TExprNodeType::FUNCTION_CALL) {
        std::string null_function_name;
        if (conjunct->is_null_scalar_function(null_function_name)) {
            if (null_function_name == "null") {
                builder->isNull(column_id, orc::PredicateDataType::BOOLEAN);
            } else if (null_function_name == "not null") {
                builder->startNot();
                builder->isNull(column_id, orc::PredicateDataType::BOOLEAN);
                builder->end();
            }
        }
        return Status::OK();
    }

    orc::PredicateDataType pred_type = _supported_logical_types.at(slot->type().type);

    if (node_type == TExprNodeType::type::BINARY_PRED) {
        Expr* lit = conjunct->get_child(1);
        ASSIGN_OR_RETURN(orc::Literal literal, translate_to_orc_literal(lit, pred_type));

        switch (op_type) {
        case TExprOpcode::EQ:
            builder->equals(column_id, pred_type, literal);
            break;

        case TExprOpcode::NE:
            builder->startNot();
            builder->equals(column_id, pred_type, literal);
            builder->end();
            break;

        case TExprOpcode::LT:
            builder->lessThan(column_id, pred_type, literal);
            break;

        case TExprOpcode::LE:
            builder->lessThanEquals(column_id, pred_type, literal);
            break;

        case TExprOpcode::GT:
            builder->startNot();
            builder->lessThanEquals(column_id, pred_type, literal);
            builder->end();
            break;

        case TExprOpcode::GE:
            builder->startNot();
            builder->lessThan(column_id, pred_type, literal);
            builder->end();
            break;

        case TExprOpcode::EQ_FOR_NULL:
            builder->nullSafeEquals(column_id, pred_type, literal);
            break;

        default:
            return Status::InternalError("unexpected op_type in binary_pred type. op_type = " +
                                         std::to_string(op_type));
        }
        return Status::OK();
    }

    if (node_type == TExprNodeType::IN_PRED) {
        bool neg = (op_type == TExprOpcode::FILTER_NOT_IN);
        if (neg) {
            builder->startNot();
        }
        std::vector<orc::Literal> literals;
        for (int i = 1; i < conjunct->get_num_children(); i++) {
            Expr* lit = conjunct->get_child(i);
            ASSIGN_OR_RETURN(orc::Literal literal, translate_to_orc_literal(lit, pred_type));
            literals.emplace_back(literal);
        }
        builder->in(column_id, pred_type, literals);
        if (neg) {
            builder->end();
        }
        return Status::OK();
    }

    return Status::InternalError("unexpected node_type = " + std::to_string(node_type));
}

#define ADD_RF_TO_BUILDER                                     \
    {                                                         \
        builder->lessThanEquals(column_id, pred_type, upper); \
        builder->startNot();                                  \
        builder->lessThan(column_id, pred_type, lower);       \
        builder->end();                                       \
        return true;                                          \
    }

#define ADD_RF_BOOLEAN_TYPE(type)                                      \
    case type: {                                                       \
        auto* xrf = dynamic_cast<const RuntimeBloomFilter<type>*>(rf); \
        if (xrf == nullptr) return false;                              \
        auto lower = orc::Literal(bool(xrf->min_value()));             \
        auto upper = orc::Literal(bool(xrf->max_value()));             \
        ADD_RF_TO_BUILDER                                              \
    }

#define ADD_RF_INT_TYPE(type)                                          \
    case type: {                                                       \
        auto* xrf = dynamic_cast<const RuntimeBloomFilter<type>*>(rf); \
        if (xrf == nullptr) return false;                              \
        auto lower = orc::Literal(int64_t(xrf->min_value()));          \
        auto upper = orc::Literal(int64_t(xrf->max_value()));          \
        ADD_RF_TO_BUILDER                                              \
    }

#define ADD_RF_DOUBLE_TYPE(type)                                       \
    case type: {                                                       \
        auto* xrf = dynamic_cast<const RuntimeBloomFilter<type>*>(rf); \
        if (xrf == nullptr) return false;                              \
        auto lower = orc::Literal(double(xrf->min_value()));           \
        auto upper = orc::Literal(double(xrf->max_value()));           \
        ADD_RF_TO_BUILDER                                              \
    }

#define ADD_RF_STRING_TYPE(type)                                                 \
    case type: {                                                                 \
        auto* xrf = dynamic_cast<const RuntimeBloomFilter<type>*>(rf);           \
        if (xrf == nullptr) return false;                                        \
        auto lower = orc::Literal(xrf->min_value().data, xrf->min_value().size); \
        auto upper = orc::Literal(xrf->max_value().data, xrf->max_value().size); \
        ADD_RF_TO_BUILDER                                                        \
    }

#define ADD_RF_DATE_TYPE(type)                                                                                        \
    case type: {                                                                                                      \
        auto* xrf = dynamic_cast<const RuntimeBloomFilter<type>*>(rf);                                                \
        if (xrf == nullptr) return false;                                                                             \
        auto lower =                                                                                                  \
                orc::Literal(orc::PredicateDataType::DATE, OrcDateHelper::native_date_to_orc_date(xrf->min_value())); \
        auto upper =                                                                                                  \
                orc::Literal(orc::PredicateDataType::DATE, OrcDateHelper::native_date_to_orc_date(xrf->max_value())); \
        ADD_RF_TO_BUILDER                                                                                             \
    }

#define ADD_RF_DECIMALV2_TYPE(type)                                                                                    \
    case type: {                                                                                                       \
        auto* xrf = dynamic_cast<const RuntimeBloomFilter<type>*>(rf);                                                 \
        if (xrf == nullptr) return false;                                                                              \
        auto lower =                                                                                                   \
                orc::Literal(to_orc128(xrf->min_value().value()), xrf->min_value().PRECISION, xrf->min_value().SCALE); \
        auto upper =                                                                                                   \
                orc::Literal(to_orc128(xrf->max_value().value()), xrf->max_value().PRECISION, xrf->max_value().SCALE); \
        ADD_RF_TO_BUILDER                                                                                              \
    }

#define ADD_RF_DECIMALV3_TYPE(xtype)                                                                          \
    case xtype: {                                                                                             \
        auto* xrf = dynamic_cast<const RuntimeBloomFilter<xtype>*>(rf);                                       \
        if (xrf == nullptr) return false;                                                                     \
        auto lower = orc::Literal(orc::Int128(xrf->min_value()), slot->type().precision, slot->type().scale); \
        auto upper = orc::Literal(orc::Int128(xrf->max_value()), slot->type().precision, slot->type().scale); \
        ADD_RF_TO_BUILDER                                                                                     \
    }

#define ADD_RF_DECIMAL128_TYPE(xtype)                                                                            \
    case xtype: {                                                                                                \
        auto* xrf = dynamic_cast<const RuntimeBloomFilter<xtype>*>(rf);                                          \
        if (xrf == nullptr) return false;                                                                        \
        auto lower = orc::Literal(orc::Int128(xrf->min_value() >> 64, xrf->min_value()), slot->type().precision, \
                                  slot->type().scale);                                                           \
        auto upper = orc::Literal(orc::Int128(xrf->max_value() >> 64, xrf->max_value()), slot->type().precision, \
                                  slot->type().scale);                                                           \
        ADD_RF_TO_BUILDER                                                                                        \
    }

bool OrcChunkReader::_add_runtime_filter(const uint64_t column_id, const SlotDescriptor* slot,
                                         const JoinRuntimeFilter* rf,
                                         std::unique_ptr<orc::SearchArgumentBuilder>& builder) {
    LogicalType ltype = slot->type().type;
    auto type_it = _supported_logical_types.find(ltype);
    if (type_it == _supported_logical_types.end()) return false;
    orc::PredicateDataType pred_type = type_it->second;
    switch (ltype) {
        ADD_RF_BOOLEAN_TYPE(LogicalType::TYPE_BOOLEAN);
        ADD_RF_INT_TYPE(LogicalType::TYPE_TINYINT);
        ADD_RF_INT_TYPE(LogicalType::TYPE_SMALLINT);
        ADD_RF_INT_TYPE(LogicalType::TYPE_INT);
        ADD_RF_INT_TYPE(LogicalType::TYPE_BIGINT);
        ADD_RF_DOUBLE_TYPE(LogicalType::TYPE_DOUBLE);
        ADD_RF_DOUBLE_TYPE(LogicalType::TYPE_FLOAT);
        ADD_RF_STRING_TYPE(LogicalType::TYPE_VARCHAR);
        ADD_RF_STRING_TYPE(LogicalType::TYPE_CHAR);
        // ADD_RF_STRING_TYPE(LogicalType::TYPE_BINARY);
        ADD_RF_DATE_TYPE(LogicalType::TYPE_DATE);
        // ADD_RF_DECIMALV2_TYPE(LogicalType::TYPE_DECIMAL);
        ADD_RF_DECIMALV2_TYPE(LogicalType::TYPE_DECIMALV2);
        ADD_RF_DECIMALV3_TYPE(LogicalType::TYPE_DECIMAL32);
        ADD_RF_DECIMALV3_TYPE(LogicalType::TYPE_DECIMAL64);
        ADD_RF_DECIMAL128_TYPE(LogicalType::TYPE_DECIMAL128);
    default:;
    }
    return false;
}

Status OrcChunkReader::build_search_argument_by_predicates(const OrcPredicates* orc_predicates) {
    if (_root_mapping == nullptr) {
        DCHECK(false);
        return Status::InternalError("You should call build OrcMapping before build_search_argument_by_predicates()");
    }

    if (orc_predicates->conjuncts == nullptr) {
        DCHECK(false);
        return Status::InternalError("conjuncts must not be null");
    }

    std::unordered_map<SlotId, size_t> slot_id_to_pos_in_src_slot_descriptors{};
    for (size_t i = 0; i < _src_slot_descriptors.size(); i++) {
        slot_id_to_pos_in_src_slot_descriptors.emplace(_src_slot_descriptors[i]->id(), i);
    }

    std::unique_ptr<orc::SearchArgumentBuilder> builder = orc::SearchArgumentFactory::newBuilder();
    int ok = 0;
    builder->startAnd();
    for (Expr* expr : *orc_predicates->conjuncts) {
        bool applied = _ok_to_add_conjunct(expr, slot_id_to_pos_in_src_slot_descriptors);
        VLOG_FILE << "OrcChunkReader: add_conjunct: " << expr->debug_string() << ", applied: " << applied;
        if (!applied) {
            continue;
        }
        ok += 1;
        RETURN_IF_ERROR(_add_conjunct(expr, slot_id_to_pos_in_src_slot_descriptors, builder));
    }

    if (orc_predicates->rf_collector != nullptr) {
        for (auto& it : orc_predicates->rf_collector->descriptors()) {
            RuntimeFilterProbeDescriptor* rf_desc = it.second;
            const JoinRuntimeFilter* filter = rf_desc->runtime_filter(-1);
            SlotId probe_slot_id;
            if (filter == nullptr || filter->has_null() || !rf_desc->is_probe_slot_ref(&probe_slot_id)) continue;
            auto it2 = slot_id_to_pos_in_src_slot_descriptors.find(probe_slot_id);
            if (it2 == slot_id_to_pos_in_src_slot_descriptors.end()) continue;
            if (!_root_mapping->contains(it2->second)) continue;
            const SlotDescriptor* slot_desc = _src_slot_descriptors[it2->second];
            const uint64_t column_id = _root_mapping->get_orc_type_child_mapping(it2->second).orc_type->getColumnId();
            if (_add_runtime_filter(column_id, slot_desc, filter, builder)) {
                ok += 1;
            }
        }
    }

    if (ok) {
        builder->end();
        std::unique_ptr<orc::SearchArgument> sargs = builder->build();
        VLOG_FILE << "OrcChunkReader::set_conjuncts. search argument = " << sargs->toString();
        _row_reader_options.searchArgument(std::move(sargs));
    }
    return Status::OK();
}

StatusOr<ColumnPtr> OrcChunkReader::get_row_delete_filter(const SkipRowsContextPtr& skip_rows_ctx) {
    int64_t start_pos = _row_reader->getRowNumber();
    auto num_rows = _batch->numElements;
    ColumnPtr filter_column = BooleanColumn::create(num_rows, 1);
    auto& filter = static_cast<BooleanColumn*>(filter_column.get())->get_data();

    if (skip_rows_ctx == nullptr || !skip_rows_ctx->has_skip_rows()) {
        return filter_column;
    }

    StatusOr<bool> status = skip_rows_ctx->deletion_bitmap->fill_filter(start_pos, start_pos + num_rows, filter);
    if (!status.ok()) {
        LOG(WARNING) << "OrcChunkReader::get_row_delete_filter, Failed to fill filter: " << status.status().message();
        return Status::InternalError(
                strings::Substitute("OrcChunkReader Failed to fill filter: $0", status.status().message()));
    }
    return filter_column;
}

size_t OrcChunkReader::get_row_delete_number(const SkipRowsContextPtr& skip_rows_ctx) {
    if (skip_rows_ctx == nullptr || !skip_rows_ctx->has_skip_rows()) {
        return 0;
    }
    int64_t start_pos = _row_reader->getRowNumber();
    auto num_rows = _batch->numElements;

    return skip_rows_ctx->deletion_bitmap->get_range_cardinality(start_pos, start_pos + num_rows);
}

Status OrcChunkReader::apply_dict_filter_eval_cache(const std::unordered_map<SlotId, FilterPtr>& dict_filter_eval_cache,
                                                    Filter* filter) {
    if (dict_filter_eval_cache.size() == 0) {
        return Status::OK();
    }

    const uint32_t size = _batch->numElements;
    filter->assign(size, 1);
    const auto& struct_batch = down_cast<orc::StructVectorBatch*>(_batch.get());
    bool filter_all = false;

    for (const auto& it : dict_filter_eval_cache) {
        size_t pos_in_src_slot_descs = _slot_id_to_pos_in_src_slot_descs[it.first];
        if (!_root_mapping->contains(pos_in_src_slot_descs)) {
            DCHECK(false) << "shouldn't happen";
            continue;
        }
        uint64_t column_id = _root_mapping->get_orc_type_child_mapping(pos_in_src_slot_descs).orc_type->getColumnId();

        const Filter& dict_filter = (*it.second);
        ColumnPtr data_filter = BooleanColumn::create(size);
        Filter& data = static_cast<BooleanColumn*>(data_filter.get())->get_data();
        DCHECK(data.size() == size);

        auto* batch = down_cast<orc::StringVectorBatch*>(struct_batch->fieldsColumnIdMap[column_id]);
        for (uint32_t i = 0; i < size; i++) {
            int64_t code = batch->codes[i];
            DCHECK(code < dict_filter.size());
            data[i] = dict_filter[code];
        }

        bool all_zero = false;
        ColumnHelper::merge_two_filters(data_filter, filter, &all_zero);
        if (all_zero) {
            filter_all = true;
            break;
        }
    }

    if (!filter_all) {
        uint32_t one_count = filter->size() - SIMD::count_zero(*filter);
        if (one_count != filter->size()) {
            if (has_lazy_load_context()) {
                _batch->filterOnFields(filter->data(), filter->size(), one_count,
                                       _lazy_load_ctx->active_load_orc_positions, false);
            } else {
                _batch->filter(filter->data(), filter->size(), one_count);
            }
        }
    } else {
        _batch->numElements = 0;
    }
    return Status::OK();
}

Status OrcChunkReader::set_timezone(const std::string& tz) {
    if (!TimezoneUtils::find_cctz_time_zone(tz, _tzinfo)) {
        return Status::InternalError(strings::Substitute("can not find cctz time zone $0", tz));
    }
    _tzoffset_in_seconds = TimezoneUtils::to_utc_offset(_tzinfo);
    return Status::OK();
}

static const int MAX_ERROR_MESSAGE_COUNTER = 100;
void OrcChunkReader::report_error_message(const std::string& error_msg) {
    if (_state == nullptr) return;
    if (_error_message_counter > MAX_ERROR_MESSAGE_COUNTER) return;
    _error_message_counter += 1;
    _state->append_error_msg_to_file("", error_msg);
}

const orc::Type* OrcChunkReader::get_orc_type_by_slot_id(const SlotId& slot_id) const {
    const auto& it = _slot_id_to_pos_in_src_slot_descs.find(slot_id);
    if (it == _slot_id_to_pos_in_src_slot_descs.end()) {
        // partition column don't existed in _src_slot_descriptors
        return nullptr;
    }
    if (_root_mapping->contains(it->second)) {
        return _root_mapping->get_orc_type_child_mapping(it->second).orc_type;
    } else {
        return nullptr;
    }
}

bool OrcChunkReader::is_implicit_castable(TypeDescriptor& starrocks_type, const TypeDescriptor& orc_type) {
    if (starrocks_type.is_decimal_type() && orc_type.is_decimal_type()) {
        return true;
    }
    return false;
}

Status OrcChunkReader::get_schema(std::vector<SlotDescriptor>* schema) {
    auto const& root = _reader->getType();

    auto cnt = root.getSubtypeCount();
    for (uint64_t i = 0; i < cnt; i++) {
        TypeDescriptor tp;
        auto name = root.getFieldName(i);

        auto subtype = root.getSubtype(i);

        RETURN_IF_ERROR(get_orc_type(subtype, &tp));

        schema->emplace_back(i, name, tp);
    }
    return Status::OK();
}

std::string OrcChunkReader::get_search_argument_string() const {
    if (_row_reader_options.getSearchArgument() == nullptr) {
        return "None";
    } else {
        return _row_reader_options.getSearchArgument()->toString();
    }
}

} // namespace starrocks
