// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/be/test/olap/rowset/beta_rowset_test.cpp

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

#include "storage/rowset/rowset.h"

#include <string>
#include <vector>

#include "column/datum_tuple.h"
#include "fs/fs_util.h"
#include "gen_cpp/olap_file.pb.h"
#include "gtest/gtest.h"
#include "runtime/exec_env.h"
#include "runtime/mem_pool.h"
#include "runtime/mem_tracker.h"
#include "storage/chunk_helper.h"
#include "storage/chunk_iterator.h"
#include "storage/data_dir.h"
#include "storage/empty_iterator.h"
#include "storage/rowset/rowset_factory.h"
#include "storage/rowset/rowset_options.h"
#include "storage/rowset/rowset_writer.h"
#include "storage/rowset/rowset_writer_context.h"
#include "storage/rowset/segment_options.h"
#include "storage/storage_engine.h"
#include "storage/tablet_manager.h"
#include "storage/tablet_reader.h"
#include "storage/tablet_schema.h"
#include "storage/union_iterator.h"
#include "storage/update_manager.h"
#include "storage/vectorized_column_predicate.h"
#include "testutil/assert.h"
#include "util/defer_op.h"

using std::string;

namespace starrocks {

static StorageEngine* k_engine = nullptr;

class RowsetTest : public testing::Test {
protected:
    OlapReaderStatistics _stats;

    void SetUp() override {
        _metadata_mem_tracker = std::make_unique<MemTracker>();
        _schema_change_mem_tracker = std::make_unique<MemTracker>();
        _page_cache_mem_tracker = std::make_unique<MemTracker>();
        config::tablet_map_shard_size = 1;
        config::txn_map_shard_size = 1;
        config::txn_shard_size = 1;

        static int i = 0;
        config::storage_root_path = std::filesystem::current_path().string() + "/data_test_" + std::to_string(i);

        ASSERT_OK(fs::remove_all(config::storage_root_path));
        ASSERT_TRUE(fs::create_directories(config::storage_root_path).ok());

        std::vector<StorePath> paths;
        paths.emplace_back(config::storage_root_path);

        starrocks::EngineOptions options;
        options.store_paths = paths;
        options.metadata_mem_tracker = _metadata_mem_tracker.get();
        options.schema_change_mem_tracker = _schema_change_mem_tracker.get();
        Status s = starrocks::StorageEngine::open(options, &k_engine);
        ASSERT_TRUE(s.ok()) << s.to_string();

        ExecEnv* exec_env = starrocks::ExecEnv::GetInstance();
        exec_env->set_storage_engine(k_engine);

        const std::string rowset_dir = config::storage_root_path + "/data/rowset_test";
        ASSERT_TRUE(fs::create_directories(rowset_dir).ok());
        StoragePageCache::create_global_cache(_page_cache_mem_tracker.get(), 1000000000);
        i++;
    }

    void TearDown() override {
        k_engine->stop();
        delete k_engine;
        k_engine = nullptr;
        starrocks::ExecEnv::GetInstance()->set_storage_engine(nullptr);
        if (fs::path_exist(config::storage_root_path)) {
            ASSERT_TRUE(fs::remove_all(config::storage_root_path).ok());
        }
        StoragePageCache::release_global_cache();
    }

    // (k1 int, k2 varchar(20), k3 int) duplicated key (k1, k2)
    void create_tablet_schema(TabletSchema* tablet_schema) {
        TabletSchemaPB tablet_schema_pb;
        tablet_schema_pb.set_keys_type(DUP_KEYS);
        tablet_schema_pb.set_num_short_key_columns(2);
        tablet_schema_pb.set_num_rows_per_row_block(1024);
        tablet_schema_pb.set_compress_kind(COMPRESS_NONE);
        tablet_schema_pb.set_next_column_unique_id(4);

        ColumnPB* column_1 = tablet_schema_pb.add_column();
        column_1->set_unique_id(1);
        column_1->set_name("k1");
        column_1->set_type("INT");
        column_1->set_is_key(true);
        column_1->set_length(4);
        column_1->set_index_length(4);
        column_1->set_is_nullable(true);
        column_1->set_is_bf_column(false);

        ColumnPB* column_2 = tablet_schema_pb.add_column();
        column_2->set_unique_id(2);
        column_2->set_name("k2");
        column_2->set_type("INT"); // TODO change to varchar(20) when dict encoding for string is supported
        column_2->set_length(4);
        column_2->set_index_length(4);
        column_2->set_is_nullable(true);
        column_2->set_is_key(true);
        column_2->set_is_nullable(true);
        column_2->set_is_bf_column(false);

        ColumnPB* column_3 = tablet_schema_pb.add_column();
        column_3->set_unique_id(3);
        column_3->set_name("v1");
        column_3->set_type("INT");
        column_3->set_length(4);
        column_3->set_is_key(false);
        column_3->set_is_nullable(false);
        column_3->set_is_bf_column(false);
        column_3->set_aggregation("SUM");

        tablet_schema->init_from_pb(tablet_schema_pb);
    }

    void create_primary_tablet_schema(TabletSchema* tablet_schema) {
        TabletSchemaPB tablet_schema_pb;
        tablet_schema_pb.set_keys_type(PRIMARY_KEYS);
        tablet_schema_pb.set_num_short_key_columns(2);
        tablet_schema_pb.set_num_rows_per_row_block(1024);
        tablet_schema_pb.set_compress_kind(COMPRESS_NONE);
        tablet_schema_pb.set_next_column_unique_id(4);

        ColumnPB* column_1 = tablet_schema_pb.add_column();
        column_1->set_unique_id(1);
        column_1->set_name("k1");
        column_1->set_type("INT");
        column_1->set_is_key(true);
        column_1->set_length(4);
        column_1->set_index_length(4);
        column_1->set_is_nullable(false);
        column_1->set_is_bf_column(false);

        ColumnPB* column_2 = tablet_schema_pb.add_column();
        column_2->set_unique_id(2);
        column_2->set_name("k2");
        column_2->set_type("INT");
        column_2->set_length(4);
        column_2->set_index_length(4);
        column_2->set_is_key(true);
        column_2->set_is_nullable(false);
        column_2->set_is_bf_column(false);

        ColumnPB* column_3 = tablet_schema_pb.add_column();
        column_3->set_unique_id(3);
        column_3->set_name("v1");
        column_3->set_type("INT");
        column_3->set_length(4);
        column_3->set_is_key(false);
        column_3->set_is_nullable(false);
        column_3->set_is_bf_column(false);
        column_3->set_aggregation("REPLACE");

        tablet_schema->init_from_pb(tablet_schema_pb);
    }

    TabletSharedPtr create_tablet(int64_t tablet_id, int32_t schema_hash) {
        TCreateTabletReq request;
        request.tablet_id = tablet_id;
        request.__set_version(1);
        request.__set_version_hash(0);
        request.tablet_schema.schema_hash = schema_hash;
        request.tablet_schema.short_key_column_count = 2;
        request.tablet_schema.keys_type = TKeysType::PRIMARY_KEYS;
        request.tablet_schema.storage_type = TStorageType::COLUMN;

        TColumn k1;
        k1.column_name = "k1";
        k1.__set_is_key(true);
        k1.column_type.type = TPrimitiveType::INT;
        request.tablet_schema.columns.push_back(k1);

        TColumn k2;
        k2.column_name = "k2";
        k2.__set_is_key(true);
        k2.column_type.type = TPrimitiveType::INT;
        request.tablet_schema.columns.push_back(k2);

        TColumn v1;
        v1.column_name = "v1";
        v1.__set_is_key(false);
        v1.column_type.type = TPrimitiveType::INT;
        v1.aggregation_type = TAggregationType::REPLACE;
        request.tablet_schema.columns.push_back(v1);

        TColumn v2;
        v2.column_name = "v2";
        v2.__set_is_key(false);
        v2.column_type.type = TPrimitiveType::INT;
        v2.aggregation_type = TAggregationType::REPLACE;
        request.tablet_schema.columns.push_back(v2);

        TColumn v3;
        v3.column_name = "v3";
        v3.__set_is_key(false);
        v3.column_type.type = TPrimitiveType::INT;
        v3.aggregation_type = TAggregationType::REPLACE;
        request.tablet_schema.columns.push_back(v3);

        auto st = StorageEngine::instance()->create_tablet(request);
        CHECK(st.ok()) << st.to_string();
        return StorageEngine::instance()->tablet_manager()->get_tablet(tablet_id, false);
    }

    void create_rowset_writer_context(const TabletSchema* tablet_schema, RowsetWriterContext* rowset_writer_context) {
        RowsetId rowset_id;
        rowset_id.init(10000);
        rowset_writer_context->rowset_id = rowset_id;
        rowset_writer_context->tablet_id = 12345;
        rowset_writer_context->tablet_schema_hash = 1111;
        rowset_writer_context->partition_id = 10;
        rowset_writer_context->rowset_path_prefix = config::storage_root_path + "/data/rowset_test";
        rowset_writer_context->rowset_state = VISIBLE;
        rowset_writer_context->tablet_schema = tablet_schema;
        rowset_writer_context->version.first = 0;
        rowset_writer_context->version.second = 0;
    }

    void create_partial_rowset_writer_context(const std::vector<int32_t>& column_indexes,
                                              std::shared_ptr<TabletSchema> partial_schema,
                                              RowsetWriterContext* rowset_writer_context) {
        RowsetId rowset_id;
        rowset_id.init(10000);
        rowset_writer_context->rowset_id = rowset_id;
        rowset_writer_context->tablet_id = 12345;
        rowset_writer_context->tablet_schema_hash = 1111;
        rowset_writer_context->partition_id = 10;
        rowset_writer_context->rowset_path_prefix = config::storage_root_path + "/data/rowset_test";
        rowset_writer_context->rowset_state = VISIBLE;
        rowset_writer_context->partial_update_tablet_schema = partial_schema;
        rowset_writer_context->tablet_schema = partial_schema.get();
        rowset_writer_context->referenced_column_ids = column_indexes;
        rowset_writer_context->version.first = 0;
        rowset_writer_context->version.second = 0;
    }

private:
    std::unique_ptr<MemTracker> _metadata_mem_tracker = nullptr;
    std::unique_ptr<MemTracker> _schema_change_mem_tracker = nullptr;
    std::unique_ptr<MemTracker> _page_cache_mem_tracker = nullptr;
};

TEST_F(RowsetTest, FinalMergeTest) {
    TabletSchema tablet_schema;
    create_primary_tablet_schema(&tablet_schema);
    RowsetSharedPtr rowset;
    const uint32_t rows_per_segment = 1024;

    RowsetWriterContext writer_context(kDataFormatV2, kDataFormatV2);
    create_rowset_writer_context(&tablet_schema, &writer_context);
    writer_context.segments_overlap = OVERLAP_UNKNOWN;

    std::unique_ptr<RowsetWriter> rowset_writer;
    ASSERT_TRUE(RowsetFactory::create_rowset_writer(writer_context, &rowset_writer).ok());

    auto schema = ChunkHelper::convert_schema_to_format_v2(tablet_schema);

    {
        auto chunk = ChunkHelper::new_chunk(schema, config::vector_chunk_size);
        auto& cols = chunk->columns();
        for (auto i = 0; i < rows_per_segment; i++) {
            cols[0]->append_datum(vectorized::Datum(static_cast<int32_t>(i)));
            cols[1]->append_datum(vectorized::Datum(static_cast<int32_t>(i)));
            cols[2]->append_datum(vectorized::Datum(static_cast<int32_t>(1)));
        }
        ASSERT_OK(rowset_writer->add_chunk(*chunk.get()));
        ASSERT_OK(rowset_writer->flush());
    }

    {
        auto chunk = ChunkHelper::new_chunk(schema, config::vector_chunk_size);
        auto& cols = chunk->columns();
        for (auto i = rows_per_segment / 2; i < rows_per_segment + rows_per_segment / 2; i++) {
            cols[0]->append_datum(vectorized::Datum(static_cast<int32_t>(i)));
            cols[1]->append_datum(vectorized::Datum(static_cast<int32_t>(i)));
            cols[2]->append_datum(vectorized::Datum(static_cast<int32_t>(2)));
        }
        ASSERT_OK(rowset_writer->add_chunk(*chunk.get()));
        ASSERT_OK(rowset_writer->flush());
    }

    {
        auto chunk = ChunkHelper::new_chunk(schema, config::vector_chunk_size);
        auto& cols = chunk->columns();
        for (auto i = rows_per_segment; i < rows_per_segment * 2; i++) {
            cols[0]->append_datum(vectorized::Datum(static_cast<int32_t>(i)));
            cols[1]->append_datum(vectorized::Datum(static_cast<int32_t>(i)));
            cols[2]->append_datum(vectorized::Datum(static_cast<int32_t>(3)));
        }
        ASSERT_OK(rowset_writer->add_chunk(*chunk.get()));
        ASSERT_OK(rowset_writer->flush());
    }

    rowset = rowset_writer->build().value();
    ASSERT_TRUE(rowset != nullptr);
    ASSERT_EQ(1, rowset->rowset_meta()->num_segments());
    ASSERT_EQ(rows_per_segment * 2, rowset->rowset_meta()->num_rows());

    vectorized::SegmentReadOptions seg_options;
    ASSIGN_OR_ABORT(seg_options.fs, FileSystem::CreateSharedFromString("posix://"));
    seg_options.stats = &_stats;

    std::string segment_file =
            Rowset::segment_file_path(writer_context.rowset_path_prefix, writer_context.rowset_id, 0);

    auto segment = *Segment::open(_metadata_mem_tracker.get(), seg_options.fs, segment_file, 0, &tablet_schema);
    ASSERT_NE(segment->num_rows(), 0);
    auto res = segment->new_iterator(schema, seg_options);
    ASSERT_FALSE(res.status().is_end_of_file() || !res.ok() || res.value() == nullptr);
    auto seg_iterator = res.value();

    seg_iterator->init_encoded_schema(vectorized::EMPTY_GLOBAL_DICTMAPS);

    auto chunk = ChunkHelper::new_chunk(seg_iterator->schema(), 100);
    size_t count = 0;
    while (true) {
        auto st = seg_iterator->get_next(chunk.get());
        if (st.is_end_of_file()) {
            break;
        }
        ASSERT_FALSE(!st.ok());
        for (auto i = 0; i < chunk->num_rows(); i++) {
            auto index = count + i;
            if (0 <= index && index < rows_per_segment / 2) {
                EXPECT_EQ(1, chunk->get(i)[2].get_int32());
            } else if (rows_per_segment / 2 <= index && index < rows_per_segment) {
                EXPECT_EQ(2, chunk->get(i)[2].get_int32());
            } else if (rows_per_segment <= index && index < rows_per_segment * 2) {
                EXPECT_EQ(3, chunk->get(i)[2].get_int32());
            }
        }
        count += chunk->num_rows();
        chunk->reset();
    }
    EXPECT_EQ(count, rows_per_segment * 2);
}

TEST_F(RowsetTest, FinalMergeVerticalTest) {
    auto tablet = create_tablet(12345, 1111);
    RowsetSharedPtr rowset;
    const uint32_t rows_per_segment = 1024;
    config::vertical_compaction_max_columns_per_group = 1;
    RowsetWriterContext writer_context(kDataFormatV2, kDataFormatV2);
    create_rowset_writer_context(&tablet->tablet_schema(), &writer_context);
    writer_context.segments_overlap = OVERLAP_UNKNOWN;

    std::unique_ptr<RowsetWriter> rowset_writer;
    ASSERT_TRUE(RowsetFactory::create_rowset_writer(writer_context, &rowset_writer).ok());

    auto schema = ChunkHelper::convert_schema_to_format_v2(tablet->tablet_schema());

    {
        auto chunk = ChunkHelper::new_chunk(schema, config::vector_chunk_size);
        auto& cols = chunk->columns();
        for (auto i = 0; i < rows_per_segment; i++) {
            cols[0]->append_datum(vectorized::Datum(static_cast<int32_t>(i)));
            cols[1]->append_datum(vectorized::Datum(static_cast<int32_t>(i)));
            cols[2]->append_datum(vectorized::Datum(static_cast<int32_t>(1)));
            cols[3]->append_datum(vectorized::Datum(static_cast<int32_t>(1)));
            cols[4]->append_datum(vectorized::Datum(static_cast<int32_t>(1)));
        }
        ASSERT_OK(rowset_writer->add_chunk(*chunk.get()));
        ASSERT_OK(rowset_writer->flush());
    }

    {
        auto chunk = ChunkHelper::new_chunk(schema, config::vector_chunk_size);
        auto& cols = chunk->columns();
        for (auto i = rows_per_segment / 2; i < rows_per_segment + rows_per_segment / 2; i++) {
            cols[0]->append_datum(vectorized::Datum(static_cast<int32_t>(i)));
            cols[1]->append_datum(vectorized::Datum(static_cast<int32_t>(i)));
            cols[2]->append_datum(vectorized::Datum(static_cast<int32_t>(2)));
            cols[3]->append_datum(vectorized::Datum(static_cast<int32_t>(2)));
            cols[4]->append_datum(vectorized::Datum(static_cast<int32_t>(2)));
        }
        ASSERT_OK(rowset_writer->add_chunk(*chunk.get()));
        ASSERT_OK(rowset_writer->flush());
    }

    {
        auto chunk = ChunkHelper::new_chunk(schema, config::vector_chunk_size);
        auto& cols = chunk->columns();
        for (auto i = rows_per_segment; i < rows_per_segment * 2; i++) {
            cols[0]->append_datum(vectorized::Datum(static_cast<int32_t>(i)));
            cols[1]->append_datum(vectorized::Datum(static_cast<int32_t>(i)));
            cols[2]->append_datum(vectorized::Datum(static_cast<int32_t>(3)));
            cols[3]->append_datum(vectorized::Datum(static_cast<int32_t>(3)));
            cols[4]->append_datum(vectorized::Datum(static_cast<int32_t>(3)));
        }
        ASSERT_OK(rowset_writer->add_chunk(*chunk.get()));
        ASSERT_OK(rowset_writer->flush());
    }

    rowset = rowset_writer->build().value();
    ASSERT_TRUE(rowset != nullptr);
    ASSERT_EQ(1, rowset->rowset_meta()->num_segments());
    ASSERT_EQ(rows_per_segment * 2, rowset->rowset_meta()->num_rows());

    vectorized::SegmentReadOptions seg_options;
    ASSIGN_OR_ABORT(seg_options.fs, FileSystem::CreateSharedFromString("posix://"));
    seg_options.stats = &_stats;

    std::string segment_file =
            Rowset::segment_file_path(writer_context.rowset_path_prefix, writer_context.rowset_id, 0);
    auto segment =
            *Segment::open(_metadata_mem_tracker.get(), seg_options.fs, segment_file, 0, &tablet->tablet_schema());
    ASSERT_NE(segment->num_rows(), 0);
    auto res = segment->new_iterator(schema, seg_options);
    ASSERT_FALSE(res.status().is_end_of_file() || !res.ok() || res.value() == nullptr);
    auto seg_iterator = res.value();

    seg_iterator->init_encoded_schema(vectorized::EMPTY_GLOBAL_DICTMAPS);

    auto chunk = ChunkHelper::new_chunk(seg_iterator->schema(), 100);
    size_t count = 0;

    while (true) {
        auto st = seg_iterator->get_next(chunk.get());
        if (st.is_end_of_file()) {
            break;
        }
        ASSERT_FALSE(!st.ok());
        for (auto i = 0; i < chunk->num_rows(); i++) {
            auto index = count + i;
            if (0 <= index && index < rows_per_segment / 2) {
                EXPECT_EQ(1, chunk->get(i)[2].get_int32());
                EXPECT_EQ(1, chunk->get(i)[3].get_int32());
                EXPECT_EQ(1, chunk->get(i)[4].get_int32());
            } else if (rows_per_segment / 2 <= index && index < rows_per_segment) {
                EXPECT_EQ(2, chunk->get(i)[2].get_int32());
                EXPECT_EQ(2, chunk->get(i)[3].get_int32());
                EXPECT_EQ(2, chunk->get(i)[4].get_int32());
            } else if (rows_per_segment <= index && index < rows_per_segment * 2) {
                EXPECT_EQ(3, chunk->get(i)[2].get_int32());
                EXPECT_EQ(3, chunk->get(i)[3].get_int32());
                EXPECT_EQ(3, chunk->get(i)[4].get_int32());
            }
        }
        count += chunk->num_rows();
        chunk->reset();
    }
    EXPECT_EQ(count, rows_per_segment * 2);
}

static vectorized::ChunkIteratorPtr create_tablet_iterator(vectorized::TabletReader& reader,
                                                           vectorized::Schema& schema) {
    vectorized::TabletReaderParams params;
    if (!reader.prepare().ok()) {
        LOG(ERROR) << "reader prepare failed";
        return nullptr;
    }
    std::vector<ChunkIteratorPtr> seg_iters;
    if (!reader.get_segment_iterators(params, &seg_iters).ok()) {
        LOG(ERROR) << "reader get segment iterators fail";
        return nullptr;
    }
    if (seg_iters.empty()) {
        return vectorized::new_empty_iterator(schema, DEFAULT_CHUNK_SIZE);
    }
    return vectorized::new_union_iterator(seg_iters);
}

static ssize_t read_and_compare(const vectorized::ChunkIteratorPtr& iter, int64_t nkeys) {
    auto full_chunk = ChunkHelper::new_chunk(iter->schema(), nkeys);
    auto& cols = full_chunk->columns();
    for (size_t i = 0; i < nkeys / 4; i++) {
        cols[0]->append_datum(vectorized::Datum(static_cast<int32_t>(i)));
        cols[1]->append_datum(vectorized::Datum(static_cast<int32_t>(i)));
        cols[2]->append_datum(vectorized::Datum(static_cast<int32_t>(1)));
        cols[3]->append_datum(vectorized::Datum(static_cast<int32_t>(1)));
    }
    for (size_t i = nkeys / 4; i < nkeys / 2; i++) {
        cols[0]->append_datum(vectorized::Datum(static_cast<int32_t>(i)));
        cols[1]->append_datum(vectorized::Datum(static_cast<int32_t>(i)));
        cols[2]->append_datum(vectorized::Datum(static_cast<int32_t>(2)));
        cols[3]->append_datum(vectorized::Datum(static_cast<int32_t>(2)));
    }
    for (size_t i = nkeys / 2; i < nkeys; i++) {
        cols[0]->append_datum(vectorized::Datum(static_cast<int32_t>(i)));
        cols[1]->append_datum(vectorized::Datum(static_cast<int32_t>(i)));
        cols[2]->append_datum(vectorized::Datum(static_cast<int32_t>(3)));
        cols[3]->append_datum(vectorized::Datum(static_cast<int32_t>(3)));
    }
    size_t count = 0;
    auto chunk = ChunkHelper::new_chunk(iter->schema(), 100);
    while (true) {
        auto st = iter->get_next(chunk.get());
        if (st.is_end_of_file()) {
            break;
        } else if (st.ok()) {
            for (auto i = 0; i < chunk->num_rows(); i++) {
                EXPECT_EQ(full_chunk->get(count + i).compare(iter->schema(), chunk->get(i)), 0);
            }
            count += chunk->num_rows();
            chunk->reset();
        } else {
            return -1;
        }
    }
    return count;
}

static ssize_t read_tablet_and_compare(const TabletSharedPtr& tablet, std::shared_ptr<TabletSchema> partial_schema,
                                       int64_t version, int64_t nkeys) {
    vectorized::Schema schema = ChunkHelper::convert_schema_to_format_v2(*partial_schema.get());
    vectorized::TabletReader reader(tablet, Version(0, version), schema);
    auto iter = create_tablet_iterator(reader, schema);
    if (iter == nullptr) {
        return -1;
    }
    return read_and_compare(iter, nkeys);
}

TEST_F(RowsetTest, FinalMergeVerticalPartialTest) {
    auto tablet = create_tablet(12345, 1111);
    const uint32_t rows_per_segment = 1024;
    config::vertical_compaction_max_columns_per_group = 1;
    RowsetWriterContext writer_context(kDataFormatV2, kDataFormatV2);
    std::vector<int32_t> column_indexes = {0, 1, 2, 3};
    std::shared_ptr<TabletSchema> partial_schema = TabletSchema::create(tablet->tablet_schema(), column_indexes);
    create_partial_rowset_writer_context(column_indexes, partial_schema, &writer_context);
    writer_context.segments_overlap = OVERLAP_UNKNOWN;
    writer_context.rowset_path_prefix = tablet->schema_hash_path();

    std::unique_ptr<RowsetWriter> rowset_writer;
    ASSERT_TRUE(RowsetFactory::create_rowset_writer(writer_context, &rowset_writer).ok());

    auto schema = ChunkHelper::convert_schema_to_format_v2(*partial_schema.get());

    {
        auto chunk = ChunkHelper::new_chunk(schema, config::vector_chunk_size);
        auto& cols = chunk->columns();
        for (auto i = 0; i < rows_per_segment; i++) {
            cols[0]->append_datum(vectorized::Datum(static_cast<int32_t>(i)));
            cols[1]->append_datum(vectorized::Datum(static_cast<int32_t>(i)));
            cols[2]->append_datum(vectorized::Datum(static_cast<int32_t>(1)));
            cols[3]->append_datum(vectorized::Datum(static_cast<int32_t>(1)));
        }
        ASSERT_OK(rowset_writer->add_chunk(*chunk.get()));
        ASSERT_OK(rowset_writer->flush());
    }

    {
        auto chunk = ChunkHelper::new_chunk(schema, config::vector_chunk_size);
        auto& cols = chunk->columns();
        for (auto i = rows_per_segment / 2; i < rows_per_segment + rows_per_segment / 2; i++) {
            cols[0]->append_datum(vectorized::Datum(static_cast<int32_t>(i)));
            cols[1]->append_datum(vectorized::Datum(static_cast<int32_t>(i)));
            cols[2]->append_datum(vectorized::Datum(static_cast<int32_t>(2)));
            cols[3]->append_datum(vectorized::Datum(static_cast<int32_t>(2)));
        }
        ASSERT_OK(rowset_writer->add_chunk(*chunk.get()));
        ASSERT_OK(rowset_writer->flush());
    }

    {
        auto chunk = ChunkHelper::new_chunk(schema, config::vector_chunk_size);
        auto& cols = chunk->columns();
        for (auto i = rows_per_segment; i < rows_per_segment * 2; i++) {
            cols[0]->append_datum(vectorized::Datum(static_cast<int32_t>(i)));
            cols[1]->append_datum(vectorized::Datum(static_cast<int32_t>(i)));
            cols[2]->append_datum(vectorized::Datum(static_cast<int32_t>(3)));
            cols[3]->append_datum(vectorized::Datum(static_cast<int32_t>(3)));
        }
        ASSERT_OK(rowset_writer->add_chunk(*chunk.get()));
        ASSERT_OK(rowset_writer->flush());
    }

    auto rowset = rowset_writer->build().value();
    ASSERT_TRUE(rowset != nullptr);
    ASSERT_EQ(1, rowset->rowset_meta()->num_segments());
    ASSERT_EQ(rows_per_segment * 2, rowset->rowset_meta()->num_rows());

    ASSERT_TRUE(tablet->rowset_commit(2, rowset).ok());
    EXPECT_EQ(rows_per_segment * 2, read_tablet_and_compare(tablet, partial_schema, 2, rows_per_segment * 2));
    ASSERT_OK(starrocks::ExecEnv::GetInstance()->storage_engine()->update_manager()->on_rowset_finished(tablet.get(),
                                                                                                        rowset.get()));
}

TEST_F(RowsetTest, VerticalWriteTest) {
    TabletSchema tablet_schema;
    create_tablet_schema(&tablet_schema);

    RowsetWriterContext writer_context(kDataFormatV2, kDataFormatV2);
    create_rowset_writer_context(&tablet_schema, &writer_context);
    writer_context.max_rows_per_segment = 5000;
    writer_context.writer_type = kVertical;

    std::unique_ptr<RowsetWriter> rowset_writer;
    ASSERT_TRUE(RowsetFactory::create_rowset_writer(writer_context, &rowset_writer).ok());

    int32_t chunk_size = 3000;
    size_t num_rows = 10000;

    {
        // k1 k2
        std::vector<uint32_t> column_indexes{0, 1};
        auto schema = ChunkHelper::convert_schema_to_format_v2(tablet_schema, column_indexes);
        auto chunk = ChunkHelper::new_chunk(schema, chunk_size);
        for (auto i = 0; i < num_rows % chunk_size; ++i) {
            chunk->reset();
            auto& cols = chunk->columns();
            for (auto j = 0; j < chunk_size && i * chunk_size + j < num_rows; ++j) {
                cols[0]->append_datum(vectorized::Datum(static_cast<int32_t>(i * chunk_size + j)));
                cols[1]->append_datum(vectorized::Datum(static_cast<int32_t>(i * chunk_size + j + 1)));
            }
            ASSERT_OK(rowset_writer->add_columns(*chunk, column_indexes, true));
        }
        ASSERT_OK(rowset_writer->flush_columns());
    }

    {
        // v1
        std::vector<uint32_t> column_indexes{2};
        auto schema = ChunkHelper::convert_schema_to_format_v2(tablet_schema, column_indexes);
        auto chunk = ChunkHelper::new_chunk(schema, chunk_size);
        for (auto i = 0; i < num_rows % chunk_size; ++i) {
            chunk->reset();
            auto& cols = chunk->columns();
            for (auto j = 0; j < chunk_size && i * chunk_size + j < num_rows; ++j) {
                cols[0]->append_datum(vectorized::Datum(static_cast<int32_t>(i * chunk_size + j + 2)));
            }
            ASSERT_OK(rowset_writer->add_columns(*chunk, column_indexes, false));
        }
        ASSERT_OK(rowset_writer->flush_columns());
    }
    ASSERT_OK(rowset_writer->final_flush());

    // check rowset
    RowsetSharedPtr rowset = rowset_writer->build().value();
    ASSERT_EQ(num_rows, rowset->rowset_meta()->num_rows());
    ASSERT_EQ(3, rowset->rowset_meta()->num_segments());

    RowsetReadOptions rs_opts;
    rs_opts.is_primary_keys = false;
    rs_opts.sorted = true;
    rs_opts.version = 0;
    rs_opts.stats = &_stats;
    auto schema = ChunkHelper::convert_schema_to_format_v2(tablet_schema);
    auto res = rowset->new_iterator(schema, rs_opts);
    ASSERT_FALSE(res.status().is_end_of_file() || !res.ok() || res.value() == nullptr);

    auto iterator = res.value();
    int count = 0;
    auto chunk = ChunkHelper::new_chunk(schema, chunk_size);
    while (true) {
        chunk->reset();
        auto st = iterator->get_next(chunk.get());
        if (st.is_end_of_file()) {
            break;
        }
        ASSERT_FALSE(!st.ok());
        for (auto i = 0; i < chunk->num_rows(); ++i) {
            EXPECT_EQ(count, chunk->get(i)[0].get_int32());
            EXPECT_EQ(count + 1, chunk->get(i)[1].get_int32());
            EXPECT_EQ(count + 2, chunk->get(i)[2].get_int32());
            ++count;
        }
    }
    EXPECT_EQ(count, num_rows);
}

} // namespace starrocks
