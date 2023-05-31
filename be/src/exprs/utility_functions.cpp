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

#include "exprs/utility_functions.h"

#include "gen_cpp/FrontendService_types.h"
#include "runtime/client_cache.h"

#ifdef __SSE4_2__
#include <emmintrin.h>
#endif
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <random>
#include <thread>

#include "column/column_builder.h"
#include "column/column_viewer.h"
#include "column/vectorized_fwd.h"
#include "common/config.h"
#include "common/version.h"
#include "exec/pipeline/fragment_context.h"
#include "exprs/function_context.h"
#include "gutil/casts.h"
#include "runtime/runtime_state.h"
#include "service/backend_options.h"
#include "types/logical_type.h"
#include "util/cidr.h"
#include "util/monotime.h"
#include "util/network_util.h"
#include "util/thread.h"
#include "util/thrift_rpc_helper.h"
#include "util/time.h"
#include "util/uid_util.h"

namespace starrocks {

StatusOr<ColumnPtr> UtilityFunctions::version(FunctionContext* context, const Columns& columns) {
    return ColumnHelper::create_const_column<TYPE_VARCHAR>("5.1.0", 1);
}

StatusOr<ColumnPtr> UtilityFunctions::current_version(FunctionContext* context, const Columns& columns) {
    static std::string version = std::string(STARROCKS_VERSION) + " " + STARROCKS_COMMIT_HASH;
    return ColumnHelper::create_const_column<TYPE_VARCHAR>(version, 1);
}

StatusOr<ColumnPtr> UtilityFunctions::sleep(FunctionContext* context, const Columns& columns) {
    ColumnViewer<TYPE_INT> data_column(columns[0]);

    auto size = columns[0]->size();
    ColumnBuilder<TYPE_BOOLEAN> result(size);
    for (int row = 0; row < size; ++row) {
        if (data_column.is_null(row)) {
            result.append_null();
            continue;
        }

        auto value = data_column.value(row);
        SleepFor(MonoDelta::FromSeconds(value));
        result.append(true);
    }

    return result.build(ColumnHelper::is_all_const(columns));
}

StatusOr<ColumnPtr> UtilityFunctions::last_query_id(FunctionContext* context, const Columns& columns) {
    starrocks::RuntimeState* state = context->state();
    const std::string& id = state->last_query_id();
    if (!id.empty()) {
        return ColumnHelper::create_const_column<TYPE_VARCHAR>(id, 1);
    } else {
        return ColumnHelper::create_const_null_column(1);
    }
}

// UUID fixed 33 bytes.
// 16bytes-16bytes
// UUIDs generated by 128 bit uuid_numeric()
// The first 48 bits are a timestamp, representing milliseconds since the chosen epoch.
// The next 16 bits represent a machine ID. (IP ^ PORT, later we will use a backend ID)
// The next 16 bits are random value.
// The next 16 bits are thread id.
// The next 32 bits are increasement value.
StatusOr<ColumnPtr> UtilityFunctions::uuid(FunctionContext* ctx, const Columns& columns) {
    int32_t num_rows = ColumnHelper::get_const_value<TYPE_INT>(columns.back());

    ASSIGN_OR_RETURN(auto col, UtilityFunctions::uuid_numeric(ctx, columns));
    auto& uuid_data = down_cast<Int128Column*>(col.get())->get_data();

    auto res = BinaryColumn::create();
    auto& bytes = res->get_bytes();
    auto& offsets = res->get_offset();

    offsets.resize(num_rows + 1);
    bytes.resize(36 * num_rows);

    char* ptr = reinterpret_cast<char*>(bytes.data());

    for (int i = 0; i < num_rows; ++i) {
        offsets[i + 1] = offsets[i] + 36;
    }

#ifdef __SSE4_2__
    alignas(16) static constexpr const char hex_chars[16] = {'0', '1', '2', '3', '4', '5', '6', '7',
                                                             '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    const __m128i mask = _mm_set1_epi8(0xF);
    const __m128i chars = _mm_load_si128(reinterpret_cast<const __m128i*>(hex_chars));
#endif

    for (int i = 0; i < num_rows; ++i) {
        char buff[32];
        memset(ptr, '-', 36);
#ifdef __SSE4_2__
        // SIMD::to_hex
        __m128i value = _mm_loadu_si64(reinterpret_cast<const __m128i*>(&uuid_data[i]));
        // 0x1234
        //-> [0x34, 0x12]
        //-> [0x23, 0x01] right shift
        //-> [0x34, 0x23, 0x12, 0x01] pack
        //-> [0x04, 0x03, 0x02, 0x01] mask operator
        //-> shuffle
        value = _mm_and_si128(_mm_unpacklo_epi8(_mm_srli_epi64(value, 4), value), mask);
        value = _mm_shuffle_epi8(chars, value);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(buff), value);

        value = _mm_loadu_si64(reinterpret_cast<const __m128i*>(reinterpret_cast<const int64_t*>(&uuid_data[i]) + 1));
        value = _mm_and_si128(_mm_unpacklo_epi8(_mm_srli_epi64(value, 4), value), mask);
        value = _mm_shuffle_epi8(chars, value);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(buff + 16), value);
#else
        to_hex(uuid_data[i], buff);
        std::reverse(buff, buff + 32);
#endif

        // UUID format 8-4-4-4-12

        memcpy(ptr, buff, 8);
        memcpy(ptr + 8 + 1, buff + 8, 4);
        memcpy(ptr + 8 + 4 + 2, buff + 8 + 4, 4);
        memcpy(ptr + 8 + 4 + 4 + 3, buff + 8 + 4 + 4, 4);
        memcpy(ptr + 8 + 4 + 4 + 4 + 4, buff + 8 + 4 + 4 + 4, 12);

        ptr += 36;
    }

    return res;
}

inline int128_t next_uuid(int64_t timestamp, int16_t backendId, int16_t rand, int16_t tid, int32_t inc) {
    union {
        struct {
            int64_t timestamp : 48;
            int64_t instance : 16;
            int16_t rand;
            int16_t tid;
            int32_t inc;
        } data;
        int128_t res;
    } v;
    v.data.timestamp = timestamp;
    v.data.instance = backendId;
    v.data.rand = rand;
    v.data.tid = tid;
    v.data.inc = inc;
    return v.res;
}

static std::atomic<int32_t> s_counter{};
// thread ids
// The number of executor threads is fixed.
static std::atomic<int16_t> inc{};
//
static thread_local int uniq_tid = -1;

int16_t get_uniq_tid() {
    if (uniq_tid == -1) {
        uniq_tid = inc.fetch_add(1);
    }
    return uniq_tid;
}

StatusOr<ColumnPtr> UtilityFunctions::uuid_numeric(FunctionContext*, const Columns& columns) {
    int32_t num_rows = ColumnHelper::get_const_value<TYPE_INT>(columns.back());
    auto result = Int128Column::create(num_rows);

    static std::random_device rd;
    static std::mt19937 mt(rd());

    std::uniform_int_distribution<int16_t> dist(std::numeric_limits<int16_t>::min(),
                                                std::numeric_limits<int16_t>::max());

    auto& data = result->get_data();

    uint32_t intip;
    CIDR::ip_to_int(BackendOptions::get_localhost(), &intip);
    intip ^= config::brpc_port;
    // current thread id
    int tid = get_uniq_tid();
    int64_t timestamp = GetCurrentTimeMicros();

    int16_t rand = dist(mt);
    int32_t inc = s_counter.fetch_add(num_rows);

    for (int i = 0; i < num_rows; ++i) {
        data[i] = next_uuid(timestamp, intip, rand, tid, inc - i);
    }

    return result;
}

StatusOr<ColumnPtr> UtilityFunctions::assert_true(FunctionContext* context, const Columns& columns) {
    auto column = columns[0];
    std::string msg = "assert_true failed due to false value";
    if (columns.size() > 1 && columns[1]->is_constant()) {
        msg = ColumnHelper::get_const_value<TYPE_VARCHAR>(columns[1]).to_string();
    }

    const auto size = column->size();

    if (column->has_null()) {
        throw std::runtime_error("assert_true failed due to null value");
    }

    if (column->is_constant()) {
        bool const_value = ColumnHelper::get_const_value<TYPE_BOOLEAN>(column);
        if (!const_value) {
            throw std::runtime_error(msg);
        }
    } else {
        if (column->is_nullable()) {
            column = FunctionHelper::get_data_column_of_nullable(column);
        }
        auto bool_column = ColumnHelper::cast_to<TYPE_BOOLEAN>(column);
        auto data = bool_column->get_data();
        for (size_t i = 0; i < size; ++i) {
            if (!data[i]) {
                throw std::runtime_error(msg);
            }
        }
    }
    return ColumnHelper::create_const_column<TYPE_BOOLEAN>(true, size);
}

StatusOr<ColumnPtr> UtilityFunctions::host_name(FunctionContext* context, const Columns& columns) {
    std::string host_name;
    auto status = get_hostname(&host_name);
    if (status.ok()) {
        return ColumnHelper::create_const_column<TYPE_VARCHAR>(host_name, 1);
    } else {
        host_name = "error";
        return ColumnHelper::create_const_column<TYPE_VARCHAR>(host_name, 1);
    }
}

StatusOr<ColumnPtr> UtilityFunctions::get_query_profile(FunctionContext* context, const Columns& columns) {
    RETURN_IF_COLUMNS_ONLY_NULL(columns);
    ColumnViewer<TYPE_VARCHAR> viewer(columns[0]);
    auto* state = context->state();
    if (state->fragment_ctx() == nullptr) {
        return Status::NotSupported("unsupport get_query_profile for no-pipeline");
    }

    const auto& fe_addr = state->fragment_ctx()->fe_addr();
    TGetProfileResponse res;
    TGetProfileRequest req;

    std::vector<std::string> query_ids;
    for (size_t i = 0; i < columns[0]->size(); ++i) {
        query_ids.emplace_back(viewer.value(i));
    }
    req.__set_query_id(query_ids);

    RETURN_IF_ERROR(ThriftRpcHelper::rpc<FrontendServiceClient>(
            fe_addr.hostname, fe_addr.port,
            [&](FrontendServiceConnection& client) { client->getQueryProfile(res, req); }));

    ColumnBuilder<TYPE_VARCHAR> builder(state->chunk_size());
    for (const auto& result : res.query_result) {
        builder.append(result);
    }

    return builder.build(false);
}

} // namespace starrocks
