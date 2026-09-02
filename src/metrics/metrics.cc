#include <cstdint>
#include <array>
#include <vector>

#include "metrics.h"

#include "common/serialization.h"

namespace {

using namespace metrics;

constexpr auto makeMetadataMetricList()
{
    return std::array{
#define X(name) Metric{#name, MetricType::UINT64, {uint64_t{}}},
        METRIC_METADATA_UINT64_LIST(X) // You can add more types below
#undef X
    };
}

// Remember to add to the total as more types expand
constexpr auto MetadataMetricListSize = static_cast<size_t>(master::U64::COUNT);
static_assert(MetadataMetricListSize == makeMetadataMetricList().size());

// This is used to calculate string sizes in compile time
constexpr std::array<Metric, MetadataMetricListSize> ConstMetadataMetricList = makeMetadataMetricList();
// This is the actual runtime array used to modify the values
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables, cert-err58-cpp)
std::array<Metric, MetadataMetricListSize> MetadataMetricList = makeMetadataMetricList();

template<std::size_t SIZE>
constexpr uint32_t getTotalStringSize(const std::array<Metric, SIZE> &arr) {
	uint32_t size = 0;
	for (const auto &metric: arr) {
		size += metric.name.size() + 4 + 1; // 32-bit string size in protocol + NULL
	}
	return size;
}

constexpr Metric& metric(const master::U64 enu)
{
    return MetadataMetricList.at(static_cast<size_t>(enu));
}

template<std::size_t SIZE>
MetricSerialized serializeMetrics(std::array<Metric, SIZE> &arr, const size_t size) {
	// This size should never wary between calls
	static std::vector<unsigned char> buffer(size);
	auto *bufferPtr = buffer.data();
	// Remember to add more Metric COUNTS to the total as types increase here
	serialize(&bufferPtr, static_cast<uint32_t>(arr.size()));

	for (const auto &value : arr) {
		serialize(&bufferPtr, std::string(value.name));
		switch (value.type) {
			case MetricType::UINT64:
				serialize(&bufferPtr, static_cast<uint8_t>(MetricType::UINT64));
				// NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access)
				serialize(&bufferPtr, value.value.u64.load());
				break;
			case MetricType::FLOAT64:
				serialize(&bufferPtr, static_cast<uint8_t>(MetricType::FLOAT64));
				// NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access)
				serialize(&bufferPtr, value.value.f64.load());
		}
	}

	return {.size = static_cast<uint32_t>(bufferPtr - buffer.data()), .data = buffer.data()};
}

} // namespace

namespace metrics {

// N.B: This function should be only called from a single thread, never from
// more than one threads (due to the static vector used in the
// serializeMetrics, which avoids array initialization since this function
// could be called very frequently)
MetricSerialized serializeMasterMetrics() {
	constexpr auto stringSize =  getTotalStringSize(ConstMetadataMetricList);
	constexpr auto u64TypeSize = ((sizeof(uint64_t) + sizeof(uint8_t)) * static_cast<uint32_t>(master::U64::COUNT));
	constexpr size_t size = sizeof(uint32_t) + u64TypeSize + stringSize;
	return serializeMetrics(MetadataMetricList, size);
}

void increment(const master::U64 enu) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access)
    metric(enu).value.u64++;
}

void set(const master::U64 enu, uint64_t val) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access)
	metric(enu).value.u64 = val;
}

} // metricsNew
