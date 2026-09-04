/*
   Copyright 2026 Urmas Rist <urmas@urist.ee>

   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS  If not, see <http://www.gnu.org/licenses/>.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

#include "metrics.h"
#include "common/serialization.h"

struct DeserializedMetric {
    std::string name;
    metrics::MetricType type;
    uint64_t uint64Value = 0;
};

std::vector<DeserializedMetric> deserializeMasterMetrics(
    const metrics::MetricSerialized buffer)
{
    std::vector<DeserializedMetric> result;

    const uint8_t* source = buffer.data;
    auto bytesLeft = static_cast<uint32_t>(buffer.size);
	auto currBufferSize = buffer.size;
	uint32_t amountOfMetrics = 0;
	bytesLeft = deserialize(source, bytesLeft, amountOfMetrics);
	source += currBufferSize - bytesLeft;
	currBufferSize = bytesLeft;
	EXPECT_NE(amountOfMetrics, 0);

    while (bytesLeft > 0) {
        DeserializedMetric metric;

        uint8_t serializedType = 0;

        bytesLeft = deserialize(source, bytesLeft, metric.name);
		source += currBufferSize - bytesLeft;
		currBufferSize = bytesLeft;
		EXPECT_GE(bytesLeft, 9);
        bytesLeft = deserialize(source, bytesLeft, serializedType);
		source += currBufferSize - bytesLeft;
		currBufferSize = bytesLeft;
		EXPECT_GE(bytesLeft, 8);

        metric.type =
            static_cast<metrics::MetricType>(serializedType);

        switch (metric.type) {
			case metrics::MetricType::UINT64:
				bytesLeft = deserialize(source, bytesLeft, metric.uint64Value);
				source += currBufferSize - bytesLeft;
				currBufferSize = bytesLeft;
				break;

			default:
				ADD_FAILURE() << "Unknown metric type: "
					   << static_cast<unsigned>(serializedType);
        }

        result.push_back(std::move(metric));
    }

	EXPECT_EQ(result.size(), amountOfMetrics);

    return result;
}

TEST(MetricsSerializationTest, ExistingMetricOrderIsStable)
{
    const auto buffer =
        metrics::serializeMasterMetrics();

    const auto metrics = deserializeMasterMetrics(buffer);

    ASSERT_EQ(
        metrics.size(),
        static_cast<size_t>(metrics::master::U64::COUNT));

    EXPECT_EQ(
        metrics[static_cast<size_t>(
            metrics::master::U64::METADATA_CLIENT_BYTES_RECEIVED_INCREMENT)].name,
        "METADATA_CLIENT_BYTES_RECEIVED_INCREMENT");

    EXPECT_EQ(
        metrics[static_cast<size_t>(
            metrics::master::U64::METADATA_CLIENT_BYTES_SENT_INCREMENT)].name,
        "METADATA_CLIENT_BYTES_SENT_INCREMENT");

    EXPECT_EQ(
        metrics[static_cast<size_t>(
            metrics::master::U64::METADATA_CLIENT_PACKETS_RECEIVED_INCREMENT)].name,
        "METADATA_CLIENT_PACKETS_RECEIVED_INCREMENT");

    EXPECT_EQ(
        metrics[static_cast<size_t>(
            metrics::master::U64::METADATA_CLIENT_PACKETS_SENT_INCREMENT)].name,
        "METADATA_CLIENT_PACKETS_SENT_INCREMENT");

    EXPECT_EQ(
        metrics[static_cast<size_t>(
            metrics::master::U64::NUMBER_OF_CHUNKS_GAUGE)].name,
        "NUMBER_OF_CHUNKS_GAUGE");

    EXPECT_EQ(
        metrics[static_cast<size_t>(
            metrics::master::U64::METADATA_CHUNK_DELETE_INCREMENT)].name,
        "METADATA_CHUNK_DELETE_INCREMENT");

    EXPECT_EQ(
        metrics[static_cast<size_t>(
            metrics::master::U64::METADATA_CHUNK_REPLICATE_INCREMENT)].name,
        "METADATA_CHUNK_REPLICATE_INCREMENT");

    EXPECT_EQ(
        metrics[static_cast<size_t>(
            metrics::master::U64::METADATA_FS_STATFS_INCREMENT)].name,
        "METADATA_FS_STATFS_INCREMENT");

    EXPECT_EQ(
        metrics[static_cast<size_t>(
            metrics::master::U64::METADATA_FS_GETATTR_INCREMENT)].name,
        "METADATA_FS_GETATTR_INCREMENT");

    EXPECT_EQ(
        metrics[static_cast<size_t>(
            metrics::master::U64::METADATA_FS_SETATTR_INCREMENT)].name,
        "METADATA_FS_SETATTR_INCREMENT");

    EXPECT_EQ(
        metrics[static_cast<size_t>(
            metrics::master::U64::METADATA_FS_LOOKUP_INCREMENT)].name,
        "METADATA_FS_LOOKUP_INCREMENT");

    EXPECT_EQ(
        metrics[static_cast<size_t>(
            metrics::master::U64::METADATA_FS_MKDIR_INCREMENT)].name,
        "METADATA_FS_MKDIR_INCREMENT");

    EXPECT_EQ(
        metrics[static_cast<size_t>(
            metrics::master::U64::METADATA_FS_RMDIR_INCREMENT)].name,
        "METADATA_FS_RMDIR_INCREMENT");

    EXPECT_EQ(
        metrics[static_cast<size_t>(
            metrics::master::U64::METADATA_FS_SYMLINK_INCREMENT)].name,
        "METADATA_FS_SYMLINK_INCREMENT");

    EXPECT_EQ(
        metrics[static_cast<size_t>(
            metrics::master::U64::METADATA_FS_READLINK_INCREMENT)].name,
        "METADATA_FS_READLINK_INCREMENT");

    EXPECT_EQ(
        metrics[static_cast<size_t>(
            metrics::master::U64::METADATA_FS_MKNOD_INCREMENT)].name,
        "METADATA_FS_MKNOD_INCREMENT");

    EXPECT_EQ(
        metrics[static_cast<size_t>(
            metrics::master::U64::METADATA_FS_UNLINK_INCREMENT)].name,
        "METADATA_FS_UNLINK_INCREMENT");

    EXPECT_EQ(
        metrics[static_cast<size_t>(
            metrics::master::U64::METADATA_FS_RENAME_INCREMENT)].name,
        "METADATA_FS_RENAME_INCREMENT");

    EXPECT_EQ(
        metrics[static_cast<size_t>(
            metrics::master::U64::METADATA_FS_LINK_INCREMENT)].name,
        "METADATA_FS_LINK_INCREMENT");

    EXPECT_EQ(
        metrics[static_cast<size_t>(
            metrics::master::U64::METADATA_FS_READDIR_INCREMENT)].name,
        "METADATA_FS_READDIR_INCREMENT");

    EXPECT_EQ(
        metrics[static_cast<size_t>(
            metrics::master::U64::METADATA_FS_OPEN_INCREMENT)].name,
        "METADATA_FS_OPEN_INCREMENT");

    EXPECT_EQ(
        metrics[static_cast<size_t>(
            metrics::master::U64::METADATA_FS_READ_INCREMENT)].name,
        "METADATA_FS_READ_INCREMENT");

    EXPECT_EQ(
        metrics[static_cast<size_t>(
            metrics::master::U64::METADATA_FS_WRITE_INCREMENT)].name,
        "METADATA_FS_WRITE_INCREMENT");

    // If this test fails, you probably need to add your new metrics here
    ASSERT_EQ(static_cast<uint64_t>(
            metrics::master::U64::METADATA_FS_WRITE_INCREMENT), static_cast<uint64_t>(
            metrics::master::U64::COUNT) - 1);
}


TEST(MetricsSerializationTest, MetricValuesChange) {
	constexpr auto GAUGE_AMOUNT = 50;
	metrics::set(metrics::master::U64::METADATA_CLIENT_BYTES_SENT_INCREMENT, 0);
	metrics::increment(metrics::master::U64::METADATA_CLIENT_BYTES_SENT_INCREMENT);
	metrics::set(metrics::master::U64::NUMBER_OF_CHUNKS_GAUGE, GAUGE_AMOUNT);
    const auto buffer =
        metrics::serializeMasterMetrics();
	const auto result = deserializeMasterMetrics(buffer);
	bool foundIncrement = false;
	bool foundGauge = false;
    for (const auto& metric : result) {
		if (metric.name == "METADATA_CLIENT_BYTES_SENT_INCREMENT") {
			foundIncrement = true;
			ASSERT_EQ(metric.uint64Value, 1);
		}
		if (metric.name == "NUMBER_OF_CHUNKS_GAUGE") {
			foundGauge = true;
			ASSERT_EQ(metric.uint64Value, GAUGE_AMOUNT);
		}
	}
	if (!foundIncrement) {
		FAIL() << "Could not find increment metric!\n";
	}
	if (!foundGauge) {
		FAIL() << "Could not find gauge metric!\n";
	}
};

// If you are adding more types, please add a similar test for those as well
// This helps avoid serialization errors where the last byte(s) is/are missing
TEST(MetricsSerializationTest, LastUint64MetricWorks)
{
	metrics::set(metrics::master::U64(int(metrics::master::U64::COUNT) - 1), 1);
    const auto buffer =
        metrics::serializeMasterMetrics();
	const auto result = deserializeMasterMetrics(buffer);
	ASSERT_EQ(result[result.size() - 1].uint64Value, 1);
}


TEST(MetricsSerializationTest, AllCurrentMetricsUseUint64)
{
    const auto buffer =
        metrics::serializeMasterMetrics();

    const auto metrics = deserializeMasterMetrics(buffer);

    ASSERT_EQ(
        metrics.size(),
        static_cast<size_t>(metrics::master::U64::COUNT));

    for (const auto& metric : metrics) {
        EXPECT_EQ(
            metric.type,
            metrics::MetricType::UINT64)
            << "Metric " << metric.name
            << " was serialized with an unexpected type";
    }
}
