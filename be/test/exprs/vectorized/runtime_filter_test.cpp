// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

#include "exprs/vectorized/runtime_filter.h"

#include <glog/logging.h>
#include <gtest/gtest.h>

#include <random>

#include "column/column_helper.h"
#include "exprs/vectorized/runtime_filter_bank.h"
#include "simd/simd.h"

namespace starrocks {
namespace vectorized {

class RuntimeFilterTest : public ::testing::Test {
public:
    void SetUp() {}
    void TearDown() {}

public:
};

TEST_F(RuntimeFilterTest, TestSimdBlockFilter) {
    SimdBlockFilter bf0;
    bf0.init(100);
    for (int i = 1; i <= 200; i += 17) {
        bf0.insert_hash(i);
    }
    for (int i = 1; i <= 200; i += 17) {
        EXPECT_FALSE(bf0.test_hash(i + 1));
        EXPECT_TRUE(bf0.test_hash(i));
    }
}

TEST_F(RuntimeFilterTest, TestSimdBlockFilterSerialize) {
    SimdBlockFilter bf0;
    bf0.init(100);
    for (int i = 1; i <= 200; i += 17) {
        bf0.insert_hash(i);
    }
    size_t ser_size = bf0.max_serialized_size();
    std::vector<uint8_t> buf(ser_size, 0);
    EXPECT_EQ(bf0.serialize(buf.data()), ser_size);

    SimdBlockFilter bf1;
    EXPECT_EQ(bf1.deserialize(buf.data()), ser_size);
    for (int i = 1; i <= 200; i += 17) {
        EXPECT_FALSE(bf1.test_hash(i + 1));
        EXPECT_TRUE(bf1.test_hash(i));
    }

    EXPECT_TRUE(bf0.check_equal(bf1));
}

TEST_F(RuntimeFilterTest, TestSimdBlockFilterMerge) {
    SimdBlockFilter bf0;
    bf0.init(100);
    for (int i = 1; i <= 200; i += 17) {
        bf0.insert_hash(i);
    }

    SimdBlockFilter bf1;
    bf1.init(100);
    for (int i = 2; i <= 200; i += 17) {
        bf1.insert_hash(i);
    }

    SimdBlockFilter bf2;
    bf2.init(100);
    bf2.merge(bf0);
    bf2.merge(bf1);
    for (int i = 1; i <= 200; i += 17) {
        EXPECT_TRUE(bf2.test_hash(i));
        EXPECT_TRUE(bf2.test_hash(i + 1));
        EXPECT_FALSE(bf2.test_hash(i + 2));
    }
}
static std::string alphabet0 =
        "abcdefgh"
        "igklmnop"
        "qrstuvwx"
        "yzABCDEF"
        "GHIGKLMN"
        "OPQRSTUV"
        "WXYZ=%01"
        "23456789";

static std::string alphabet1 = "~!@#$%^&*()_+{}|:\"<>?[]\\;',./";

static std::shared_ptr<BinaryColumn> gen_random_binary_column(const std::string& alphabet, size_t avg_length,
                                                              size_t num_rows) {
    auto col = BinaryColumn::create();
    col->reserve(num_rows);
    std::random_device rd;
    std::uniform_int_distribution<size_t> length_g(0, 2 * avg_length);
    std::uniform_int_distribution<size_t> g(0, alphabet.size());

    for (auto i = 0; i < num_rows; ++i) {
        size_t length = length_g(rd);
        std::string s;
        s.reserve(length);
        for (auto i = 0; i < length; ++i) {
            s.push_back(alphabet[g(rd)]);
        }
        col->append(Slice(s));
    }
    return col;
}

TEST_F(RuntimeFilterTest, TestJoinRuntimeFilter) {
    RuntimeBloomFilter<TYPE_INT> bf;
    JoinRuntimeFilter* rf = &bf;
    bf.init(100);
    for (int i = 0; i <= 200; i += 17) {
        bf.insert(&i);
    }
    EXPECT_EQ(bf.min_value(), 0);
    EXPECT_EQ(bf.max_value(), 187);
    for (int i = 0; i <= 200; i += 17) {
        EXPECT_TRUE(bf.test_data(i));
        EXPECT_FALSE(bf.test_data(i + 1));
    }
    EXPECT_FALSE(rf->has_null());
    bf.insert(nullptr);
    EXPECT_TRUE(rf->has_null());
    EXPECT_EQ(bf.min_value(), 0);
    EXPECT_EQ(bf.max_value(), 187);

    // test evaluate.
    TypeDescriptor type_desc(TYPE_INT);
    ColumnPtr column = ColumnHelper::create_column(type_desc, false);
    auto* col = ColumnHelper::as_raw_column<RunTimeTypeTraits<TYPE_INT>::ColumnType>(column);
    for (int i = 0; i <= 200; i += 1) {
        col->append(i);
    }
    Chunk chunk;
    chunk.append_column(column, 0);
    JoinRuntimeFilter::RunningContext ctx;
    auto& selection = ctx.selection;
    selection.assign(column->size(), 1);
    rf->evaluate(column.get(), &ctx);
    chunk.filter(selection);
    // 0 17 34 ... 187
    EXPECT_EQ(chunk.num_rows(), 12);
}

TEST_F(RuntimeFilterTest, TestJoinRuntimeFilterSlice) {
    RuntimeBloomFilter<TYPE_VARCHAR> bf;
    // JoinRuntimeFilter* rf = &bf;
    std::vector<std::string> data = {"aa", "bb", "cc", "dd"};
    std::vector<Slice> values;
    for (const auto& s : data) {
        values.emplace_back(Slice(s));
    }
    bf.init(100);
    for (auto& s : values) {
        bf.insert(&s);
    }
    EXPECT_EQ(bf.min_value(), values[0]);
    EXPECT_EQ(bf.max_value(), values[values.size() - 1]);
    for (auto& s : values) {
        EXPECT_TRUE(bf.test_data(s));
    }
    std::vector<std::string> ex_data = {"ee", "ff", "gg"};
    for (const auto& s : ex_data) {
        EXPECT_FALSE(bf.test_data(Slice(s)));
    }
}

TEST_F(RuntimeFilterTest, TestJoinRuntimeFilterSerialize) {
    RuntimeBloomFilter<TYPE_INT> bf0;
    JoinRuntimeFilter* rf0 = &bf0;
    bf0.init(100);
    for (int i = 0; i <= 200; i += 17) {
        bf0.insert(&i);
    }

    size_t max_size = RuntimeFilterHelper::max_runtime_filter_serialized_size(rf0);
    std::vector<uint8_t> buffer(max_size, 0);
    size_t actual_size = RuntimeFilterHelper::serialize_runtime_filter(rf0, buffer.data());
    buffer.resize(actual_size);

    JoinRuntimeFilter* rf1 = nullptr;
    ObjectPool pool;
    RuntimeFilterHelper::deserialize_runtime_filter(&pool, &rf1, buffer.data(), actual_size);
    EXPECT_TRUE(rf1->check_equal(*rf0));
}

TEST_F(RuntimeFilterTest, TestJoinRuntimeFilterSerialize2) {
    RuntimeBloomFilter<TYPE_INT> bf0;
    JoinRuntimeFilter* rf0 = &bf0;
    bf0.init(100);
    for (int i = 0; i <= 200; i += 17) {
        bf0.insert(&i);
    }
    EXPECT_EQ(bf0.min_value(), 0);
    EXPECT_EQ(bf0.max_value(), 187);

    RuntimeBloomFilter<TYPE_VARCHAR> bf1;
    JoinRuntimeFilter* rf1 = &bf1;
    std::vector<std::string> data = {"aa", "bb", "cc", "dd"};
    std::vector<Slice> values;
    for (const auto& s : data) {
        values.emplace_back(Slice(s));
    }
    bf1.init(200);
    for (auto& s : values) {
        bf1.insert(&s);
    }
    EXPECT_EQ(bf1.min_value(), values[0]);
    EXPECT_EQ(bf1.max_value(), values[values.size() - 1]);

    ObjectPool pool;
    size_t max_size = RuntimeFilterHelper::max_runtime_filter_serialized_size(rf0);
    std::vector<uint8_t> buffer0(max_size, 0);
    size_t actual_size = RuntimeFilterHelper::serialize_runtime_filter(rf0, buffer0.data());
    buffer0.resize(actual_size);
    JoinRuntimeFilter* rf2 = nullptr;
    RuntimeFilterHelper::deserialize_runtime_filter(&pool, &rf2, buffer0.data(), actual_size);

    max_size = RuntimeFilterHelper::max_runtime_filter_serialized_size(rf1);
    buffer0.assign(max_size, 0);
    actual_size = RuntimeFilterHelper::serialize_runtime_filter(rf1, buffer0.data());
    buffer0.resize(actual_size);
    JoinRuntimeFilter* rf3 = nullptr;
    RuntimeFilterHelper::deserialize_runtime_filter(&pool, &rf3, buffer0.data(), actual_size);

    EXPECT_TRUE(rf2->check_equal(*rf0));
    EXPECT_TRUE(rf3->check_equal(*rf1));
}

TEST_F(RuntimeFilterTest, TestJoinRuntimeFilterMerge) {
    RuntimeBloomFilter<TYPE_INT> bf0;
    JoinRuntimeFilter* rf0 = &bf0;
    bf0.init(100);
    for (int i = 0; i <= 200; i += 17) {
        bf0.insert(&i);
    }
    EXPECT_EQ(bf0.min_value(), 0);
    EXPECT_EQ(bf0.max_value(), 187);

    RuntimeBloomFilter<TYPE_INT> bf1;
    JoinRuntimeFilter* rf1 = &bf1;
    bf1.init(100);
    for (int i = 1; i <= 200; i += 17) {
        bf1.insert(&i);
    }
    EXPECT_EQ(bf1.min_value(), 1);
    EXPECT_EQ(bf1.max_value(), 188);

    RuntimeBloomFilter<TYPE_INT> bf2;
    bf2.init(100);
    bf2.merge(rf0);
    bf2.merge(rf1);
    for (int i = 0; i <= 200; i += 17) {
        EXPECT_TRUE(bf2.test_data(i));
        EXPECT_TRUE(bf2.test_data(i + 1));
        EXPECT_FALSE(bf2.test_data(i + 2));
    }
    EXPECT_EQ(bf2.min_value(), 0);
    EXPECT_EQ(bf2.max_value(), 188);
}

TEST_F(RuntimeFilterTest, TestJoinRuntimeFilterMerge2) {
    RuntimeBloomFilter<TYPE_VARCHAR> bf0;
    JoinRuntimeFilter* rf0 = &bf0;
    std::vector<std::string> data = {"bb", "cc", "dd"};
    {
        std::vector<Slice> values;
        for (const auto& s : data) {
            values.emplace_back(Slice(s));
        }
        bf0.init(100);
        for (auto& s : values) {
            bf0.insert(&s);
        }
        // bb - dd
        EXPECT_EQ(bf0.min_value(), values[0]);
        EXPECT_EQ(bf0.max_value(), values[values.size() - 1]);
    }

    RuntimeBloomFilter<TYPE_VARCHAR> bf1;
    JoinRuntimeFilter* rf1 = &bf1;
    std::vector<std::string> data2 = {"aa", "bb", "cc", "dc"};

    {
        std::vector<Slice> values;
        for (const auto& s : data2) {
            values.emplace_back(Slice(s));
        }
        bf1.init(100);
        for (auto& s : values) {
            bf1.insert(&s);
        }
        // aa - dc
        EXPECT_EQ(bf1.min_value(), values[0]);
        EXPECT_EQ(bf1.max_value(), values[values.size() - 1]);
    }

    // range aa - dd
    rf0->merge(rf1);
    EXPECT_EQ(bf0.min_value(), Slice("aa", 2));
    EXPECT_EQ(bf0.max_value(), Slice("dd", 2));
}

TEST_F(RuntimeFilterTest, TestJoinRuntimeFilterMerge3) {
    RuntimeBloomFilter<TYPE_VARCHAR> bf0;
    JoinRuntimeFilter* rf0 = &bf0;
    ObjectPool pool;
    {
        std::vector<std::string> data = {"bb", "cc", "dd"};
        std::vector<Slice> values;
        for (const auto& s : data) {
            values.emplace_back(Slice(s));
        }
        bf0.init(100);
        for (auto& s : values) {
            bf0.insert(&s);
        }

        size_t max_size = RuntimeFilterHelper::max_runtime_filter_serialized_size(rf0);
        std::string buf(max_size, 0);
        size_t actual_size = RuntimeFilterHelper::serialize_runtime_filter(rf0, (uint8_t*)buf.data());
        buf.resize(actual_size);

        RuntimeFilterHelper::deserialize_runtime_filter(&pool, &rf0, (const uint8_t*)buf.data(), actual_size);
    }

    auto* pbf0 = static_cast<RuntimeBloomFilter<TYPE_VARCHAR>*>(rf0);
    EXPECT_EQ(pbf0->min_value(), Slice("bb", 2));
    EXPECT_EQ(pbf0->max_value(), Slice("dd", 2));

    RuntimeBloomFilter<TYPE_VARCHAR> bf1;
    JoinRuntimeFilter* rf1 = &bf1;
    {
        std::vector<std::string> data = {"aa", "cc", "dc"};
        std::vector<Slice> values;
        for (const auto& s : data) {
            values.emplace_back(Slice(s));
        }
        bf1.init(100);
        for (auto& s : values) {
            bf1.insert(&s);
        }

        size_t max_size = RuntimeFilterHelper::max_runtime_filter_serialized_size(rf1);
        std::string buf(max_size, 0);
        size_t actual_size = RuntimeFilterHelper::serialize_runtime_filter(rf1, (uint8_t*)buf.data());
        buf.resize(actual_size);
        RuntimeFilterHelper::deserialize_runtime_filter(&pool, &rf1, (const uint8_t*)buf.data(), actual_size);
    }

    auto* pbf1 = static_cast<RuntimeBloomFilter<TYPE_VARCHAR>*>(rf1);
    EXPECT_EQ(pbf1->min_value(), Slice("aa", 2));
    EXPECT_EQ(pbf1->max_value(), Slice("dc", 2));

    // range aa - dd
    rf0->merge(rf1);
    // out of scope, we expect aa and dd would be still alive.
    EXPECT_EQ(pbf0->min_value(), Slice("aa", 2));
    EXPECT_EQ(pbf0->max_value(), Slice("dd", 2));
}

typedef std::function<void(BinaryColumn*, std::vector<uint32_t>&, std::vector<size_t>&)> PartitionByFunc;
typedef std::function<void(JoinRuntimeFilter*, JoinRuntimeFilter::RunningContext*)> GrfConfigFunc;

void test_grf_helper(size_t num_rows, size_t num_partitions, PartitionByFunc part_func, GrfConfigFunc grf_config_func) {
    std::vector<RuntimeBloomFilter<TYPE_VARCHAR>> bfs(num_partitions);
    std::vector<JoinRuntimeFilter*> rfs(num_partitions);
    for (auto p = 0; p < num_partitions; ++p) {
        rfs[p] = &bfs[p];
    }

    ObjectPool pool;
    auto column = gen_random_binary_column(alphabet0, 100, num_rows);
    std::vector<size_t> num_rows_per_partitions(num_partitions, 0);
    std::vector<uint32_t> hash_values;
    part_func(column.get(), hash_values, num_rows_per_partitions);
    for (auto p = 0; p < num_partitions; ++p) {
        bfs[p].init(num_rows_per_partitions[p]);
    }

    for (auto i = 0; i < num_rows; ++i) {
        auto slice = column->get_slice(i);
        bfs[hash_values[i]].insert(&slice);
    }

    std::vector<std::string> serialized_rfs(num_partitions);
    for (auto p = 0; p < num_partitions; ++p) {
        size_t max_size = RuntimeFilterHelper::max_runtime_filter_serialized_size(rfs[p]);
        serialized_rfs[p].resize(max_size, 0);
        size_t actual_size = RuntimeFilterHelper::serialize_runtime_filter(rfs[p], (uint8_t*)serialized_rfs[p].data());
        serialized_rfs[p].resize(actual_size);
    }

    RuntimeBloomFilter<TYPE_VARCHAR> grf;
    for (auto p = 0; p < num_partitions; ++p) {
        JoinRuntimeFilter* grf_component;
        RuntimeFilterHelper::deserialize_runtime_filter(&pool, &grf_component, (const uint8_t*)serialized_rfs[p].data(),
                                                        serialized_rfs[p].size());
        ASSERT_EQ(grf_component->size(), num_rows_per_partitions[p]);
        grf.concat(grf_component);
    }
    ASSERT_EQ(grf.size(), num_rows);
    JoinRuntimeFilter::RunningContext running_ctx;
    grf_config_func(&grf, &running_ctx);
    {
        running_ctx.selection.assign(num_rows, 1);
        auto true_count = grf.evaluate(column.get(), &running_ctx);
        auto true_count2 = SIMD::count_nonzero(running_ctx.selection.data(), num_rows);
        ASSERT_EQ(true_count, num_rows);
        ASSERT_EQ(true_count, true_count2);
    }
    {
        auto negative_column = gen_random_binary_column(alphabet1, 100, num_rows);
        running_ctx.selection.assign(num_rows, 1);
        auto true_count = grf.evaluate(negative_column.get(), &running_ctx);
        auto true_count2 = SIMD::count_nonzero(running_ctx.selection.data(), num_rows);
        ASSERT_LE((double)true_count / num_rows, 0.5);
        ASSERT_EQ(true_count, true_count2);
    }
}
void test_colocate_grf_helper(size_t num_rows, size_t num_partitions, size_t num_buckets,
                              std::vector<int> bucketseq_to_partition) {
    auto part_by_func = [num_rows, num_partitions, num_buckets, &bucketseq_to_partition](
                                BinaryColumn* column, std::vector<uint32_t>& hash_values,
                                std::vector<size_t>& num_rows_per_partitions) {
        hash_values.assign(num_rows, 0);
        column->crc32_hash(hash_values.data(), 0, num_rows);
        for (auto i = 0; i < num_rows; ++i) {
            hash_values[i] %= num_buckets;
            hash_values[i] = bucketseq_to_partition[hash_values[i]];
            ++num_rows_per_partitions[hash_values[i]];
        }
    };
    auto grf_config_func = [&bucketseq_to_partition](JoinRuntimeFilter* grf, JoinRuntimeFilter::RunningContext* ctx) {
        grf->set_join_mode(TRuntimeFilterBuildJoinMode::COLOCATE);
        ctx->bucketseq_to_partition = &bucketseq_to_partition;
    };
    test_grf_helper(num_rows, num_partitions, part_by_func, grf_config_func);
}

TEST_F(RuntimeFilterTest, TestColocateRuntimeFilter1) {
    test_colocate_grf_helper(100, 3, 6, {1, 1, 0, 0, 2, 2});
}

TEST_F(RuntimeFilterTest, TestColocateRuntimeFilter2) {
    test_colocate_grf_helper(100, 3, 7, {0, 1, 2, 0, 1, 2, 1});
}

TEST_F(RuntimeFilterTest, TestColocateRuntimeFilter3) {
    test_colocate_grf_helper(100, 1, 7, {0, 0, 0, 0, 0, 0, 0});
}

void test_partitioned_or_shuffle_hash_bucket_grf_helper(size_t num_rows, size_t num_partitions,
                                                        TRuntimeFilterBuildJoinMode::type type) {
    auto part_by_func = [num_rows, num_partitions](BinaryColumn* column, std::vector<uint32_t>& hash_values,
                                                   std::vector<size_t>& num_rows_per_partitions) {
        hash_values.assign(num_rows, HashUtil::FNV_SEED);
        column->fnv_hash(hash_values.data(), 0, num_rows);
        for (auto i = 0; i < num_rows; ++i) {
            hash_values[i] %= num_partitions;
            ++num_rows_per_partitions[hash_values[i]];
        }
    };
    auto grf_config_func = [type](JoinRuntimeFilter* grf, JoinRuntimeFilter::RunningContext* ctx) {
        grf->set_join_mode(type);
    };
    test_grf_helper(num_rows, num_partitions, part_by_func, grf_config_func);
}

void test_partitioned_grf_helper(size_t num_rows, size_t num_partitions) {
    test_partitioned_or_shuffle_hash_bucket_grf_helper(num_rows, num_partitions,
                                                       TRuntimeFilterBuildJoinMode::PARTITIONED);
}
void test_shuffle_hash_bucket_grf_helper(size_t num_rows, size_t num_partitions) {
    test_partitioned_or_shuffle_hash_bucket_grf_helper(num_rows, num_partitions,
                                                       TRuntimeFilterBuildJoinMode::SHUFFLE_HASH_BUCKET);
}
TEST_F(RuntimeFilterTest, TestPartitionedRuntimeFilter1) {
    test_partitioned_grf_helper(100, 1);
}
TEST_F(RuntimeFilterTest, TestPartitionedRuntimeFilter2) {
    test_partitioned_grf_helper(100, 3);
}
TEST_F(RuntimeFilterTest, TestPartitionedRuntimeFilter3) {
    test_partitioned_grf_helper(100, 5);
}

TEST_F(RuntimeFilterTest, TestShuffleHashBucketRuntimeFilter1) {
    test_shuffle_hash_bucket_grf_helper(100, 1);
}
TEST_F(RuntimeFilterTest, TestShuffleHashBucketRuntimeFilter2) {
    test_shuffle_hash_bucket_grf_helper(100, 3);
}
TEST_F(RuntimeFilterTest, TestShuffleHashBucketRuntimeFilter3) {
    test_shuffle_hash_bucket_grf_helper(100, 5);
}

void test_local_hash_bucket_grf_helper(size_t num_rows, size_t num_partitions) {
    auto part_by_func = [num_rows, num_partitions](BinaryColumn* column, std::vector<uint32_t>& hash_values,
                                                   std::vector<size_t>& num_rows_per_partitions) {
        hash_values.assign(num_rows, 0);
        column->crc32_hash(hash_values.data(), 0, num_rows);
        for (auto i = 0; i < num_rows; ++i) {
            hash_values[i] %= num_partitions;
            ++num_rows_per_partitions[hash_values[i]];
        }
    };
    auto grf_config_func = [](JoinRuntimeFilter* grf, JoinRuntimeFilter::RunningContext* ctx) {
        grf->set_join_mode(TRuntimeFilterBuildJoinMode::LOCAL_HASH_BUCKET);
    };
    test_grf_helper(num_rows, num_partitions, part_by_func, grf_config_func);
}

TEST_F(RuntimeFilterTest, TestLocalHashBucketRuntimeFilter1) {
    test_local_hash_bucket_grf_helper(100, 1);
}

TEST_F(RuntimeFilterTest, TestLocalHashBucketRuntimeFilter2) {
    test_local_hash_bucket_grf_helper(100, 3);
}

TEST_F(RuntimeFilterTest, TestLocalHashBucketRuntimeFilter3) {
    test_local_hash_bucket_grf_helper(100, 5);
}

} // namespace vectorized
} // namespace starrocks
