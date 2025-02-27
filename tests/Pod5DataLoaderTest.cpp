#include "MessageSinkUtils.h"
#include "TestUtils.h"
#include "data_loader/DataLoader.h"
#include "read_pipeline/ReadPipeline.h"

#include <catch2/catch.hpp>

#define TEST_GROUP "Pod5DataLoaderTest: "

namespace {

class MockSink : public dorado::MessageSink {
public:
    MockSink() : MessageSink(1000) {}
    size_t get_read_count();
};

size_t MockSink::get_read_count() {
    size_t read_count = 0;
    dorado::Message read;
    while (m_work_queue.try_pop(read))
        ++read_count;
    return read_count;
}

}  // namespace

TEST_CASE(TEST_GROUP "Test loading single-read POD5 files") {
    // Create a mock sink for testing output of reads
    MockSink mock_sink;

    std::string data_path(get_pod5_data_dir());
    dorado::DataLoader loader(mock_sink, "cpu", 1);
    loader.load_reads(data_path, false);

    REQUIRE(mock_sink.get_read_count() == 1);
}

TEST_CASE(TEST_GROUP "Test loading single-read POD5 file, empty read list") {
    // Create a mock sink for testing output of reads
    MockSink mock_sink;

    auto read_list = std::unordered_set<std::string>();
    std::string data_path(get_pod5_data_dir());
    dorado::DataLoader loader(mock_sink, "cpu", 1, 0, read_list);
    loader.load_reads(data_path, false);

    REQUIRE(mock_sink.get_read_count() == 0);
}

TEST_CASE(TEST_GROUP "Test loading single-read POD5 file, no read list") {
    // Create a mock sink for testing output of reads
    MockSink mock_sink;

    std::string data_path(get_pod5_data_dir());
    dorado::DataLoader loader(mock_sink, "cpu", 1, 0, std::nullopt);
    loader.load_reads(data_path, false);

    REQUIRE(mock_sink.get_read_count() == 1);
}

TEST_CASE(TEST_GROUP "Test loading single-read POD5 file, mismatched read list") {
    // Create a mock sink for testing output of reads
    MockSink mock_sink;

    auto read_list = std::unordered_set<std::string>();
    read_list.insert("read_1");
    std::string data_path(get_pod5_data_dir());
    dorado::DataLoader loader(mock_sink, "cpu", 1, 0, read_list);
    loader.load_reads(data_path, false);

    REQUIRE(mock_sink.get_read_count() == 0);
}

TEST_CASE(TEST_GROUP "Test loading single-read POD5 file, matched read list") {
    // Create a mock sink for testing output of reads
    MockSink mock_sink;

    auto read_list = std::unordered_set<std::string>();
    read_list.insert("002bd127-db82-436f-b828-28567c3d505d");  // read present in POD5
    std::string data_path(get_pod5_data_dir());
    dorado::DataLoader loader(mock_sink, "cpu", 1, 0, read_list);
    loader.load_reads(data_path, false);

    REQUIRE(mock_sink.get_read_count() == 1);
}

TEST_CASE(TEST_GROUP "Test calculating number of reads from pod5, read ids list.") {
    // Create a mock sink for testing output of reads
    std::string data_path(get_pod5_data_dir());

    SECTION("pod5 file only, no read ids list") {
        CHECK(dorado::DataLoader::get_num_reads(data_path, std::nullopt) == 1);
    }

    SECTION("pod5 file and read ids with 0 reads") {
        auto read_list = std::unordered_set<std::string>();
        CHECK(dorado::DataLoader::get_num_reads(data_path, read_list) == 0);
    }
    SECTION("pod5 file and read ids with 2 reads") {
        auto read_list = std::unordered_set<std::string>();
        read_list.insert("1");
        read_list.insert("2");
        CHECK(dorado::DataLoader::get_num_reads(data_path, read_list) == 1);
    }
}

TEST_CASE(TEST_GROUP "Find sample rate from pod5.") {
    // Create a mock sink for testing output of reads
    std::string data_path(get_pod5_data_dir());

    CHECK(dorado::DataLoader::get_sample_rate(data_path) == 4000);
}

TEST_CASE(TEST_GROUP "Find sample rate from nested pod5.") {
    // Create a mock sink for testing output of reads
    std::string data_path(get_nested_pod5_data_dir());

    CHECK(dorado::DataLoader::get_sample_rate(data_path, true) == 4000);
}

TEST_CASE(TEST_GROUP "Load data sorted by channel id.") {
    std::string data_path(get_data_dir("multi_read_pod5"));

    MessageSinkToVector<std::shared_ptr<dorado::Read>> sink(100);
    dorado::DataLoader loader(sink, "cpu", 1, 0);
    loader.load_reads(data_path, true, dorado::DataLoader::ReadOrder::BY_CHANNEL);

    auto reads = sink.get_messages();
    int start_channel_id = -1;
    for (auto &i : reads) {
        CHECK(i->attributes.channel_number >= start_channel_id);
        start_channel_id = i->attributes.channel_number;
    }
}

TEST_CASE(TEST_GROUP "Test loading POD5 file with read ignore list") {
    std::string data_path(get_data_dir("multi_read_pod5"));

    SECTION("read ignore list with 1 read") {
        auto read_ignore_list = std::unordered_set<std::string>();
        read_ignore_list.insert("0007f755-bc82-432c-82be-76220b107ec5");  // read present in POD5

        CHECK(dorado::DataLoader::get_num_reads(data_path, std::nullopt, read_ignore_list) == 3);

        MockSink mock_sink;
        dorado::DataLoader loader(mock_sink, "cpu", 1, 0, std::nullopt, read_ignore_list);
        loader.load_reads(data_path, false);

        REQUIRE(mock_sink.get_read_count() == 3);
    }

    SECTION("same read in read_ids and ignore list") {
        auto read_list = std::unordered_set<std::string>();
        read_list.insert("0007f755-bc82-432c-82be-76220b107ec5");  // read present in POD5
        auto read_ignore_list = std::unordered_set<std::string>();
        read_ignore_list.insert("0007f755-bc82-432c-82be-76220b107ec5");  // read present in POD5

        CHECK(dorado::DataLoader::get_num_reads(data_path, read_list, read_ignore_list) == 0);

        MockSink mock_sink;
        dorado::DataLoader loader(mock_sink, "cpu", 1, 0, read_list, read_ignore_list);
        loader.load_reads(data_path, false);

        REQUIRE(mock_sink.get_read_count() == 0);
    }
}
