//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/storage/compression/patas/patas_scan.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/storage/compression/chimp/chimp.hpp"
#include "duckdb/storage/compression/chimp/algorithm/packed_data.hpp"
#include "duckdb/storage/compression/chimp/algorithm/byte_reader.hpp"
#include "duckdb/storage/compression/patas/shared.hpp"
#include "duckdb/storage/compression/patas/algorithm/patas.hpp"
#include "duckdb/storage/compression/patas/patas.hpp"

#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/function/compression_function.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/compression/compression_segment_reader.hpp"

#include "duckdb/storage/table/column_segment.hpp"
#include "duckdb/storage/table/scan_state.hpp"

namespace duckdb {

[[noreturn]] void ThrowPatasInvalidBackwardReference();
[[noreturn]] void ThrowPatasInvalidPackedValueMetadata();
[[noreturn]] void ThrowPatasMetadataBeforeHeader();
[[noreturn]] void ThrowPatasMetadataTableOutOfBounds();
[[noreturn]] void ThrowPatasDataOffsetOutOfBounds();

//! Do not change order of these variables
struct PatasUnpackedValueStats {
	uint8_t significant_bytes;
	uint8_t trailing_zeros;
	uint8_t index_diff;
};

template <class EXACT_TYPE>
struct PatasGroupState {
public:
	void Init(const uint8_t *data) {
		byte_reader.SetStream(data);
	}

	idx_t BytesRead() const {
		return byte_reader.Index();
	}

	void Reset() {
		index = 0;
	}

	void LoadPackedData(const PatasPrimitives::PACKED_DATA_TYPE *packed_data, idx_t count) {
		for (idx_t i = 0; i < count; i++) {
			auto &unpacked = unpacked_data[i];
			PackedDataUtils<EXACT_TYPE>::Unpack(packed_data[i], (UnpackedData &)unpacked);
		}
	}

	template <bool SKIP = false>
	void Scan(uint8_t *dest, idx_t count) {
		if (!SKIP) {
			memcpy(dest, (void *)(values + index), sizeof(EXACT_TYPE) * count);
		}
		index += count;
	}

	template <bool SKIP>
	void LoadValues(EXACT_TYPE *value_buffer, idx_t count) {
		if (SKIP) {
			return;
		}
		value_buffer[0] = (EXACT_TYPE)0;
		for (idx_t i = 0; i < count; i++) {
			if (unpacked_data[i].index_diff > i) {
				ThrowPatasInvalidBackwardReference();
			}
			if (unpacked_data[i].significant_bytes > sizeof(EXACT_TYPE) ||
			    unpacked_data[i].trailing_zeros >= sizeof(EXACT_TYPE) * 8) {
				ThrowPatasInvalidPackedValueMetadata();
			}

			value_buffer[i] = patas::PatasDecompression<EXACT_TYPE>::DecompressValue(
			    byte_reader, unpacked_data[i].significant_bytes, unpacked_data[i].trailing_zeros,
			    value_buffer[i - unpacked_data[i].index_diff]);
		}
	}

public:
	idx_t index;
	PatasUnpackedValueStats unpacked_data[PatasPrimitives::PATAS_GROUP_SIZE];
	EXACT_TYPE values[PatasPrimitives::PATAS_GROUP_SIZE];

private:
	ByteReader byte_reader;
};

template <class T>
struct PatasScanState : public SegmentScanState {
public:
	using EXACT_TYPE = typename FloatingToExact<T>::TYPE;

	struct SegmentLayout {
		CompressionSegmentReader data;
		CompressionSegmentReader metadata;
	};

	static SegmentLayout ReadSegmentLayout(const BufferHandle &handle, ColumnSegment &segment, idx_t count) {
		auto reader = CompressionSegmentReader::FromSegment(handle, segment, "Patas segment");
		auto metadata_end = reader.template Read<PatasPrimitives::METADATA_POINTER_TYPE>();
		if (metadata_end < PatasPrimitives::HEADER_SIZE) {
			ThrowPatasMetadataBeforeHeader();
		}
		reader = reader.GetSubReader(0, metadata_end, "Patas segment");

		auto group_count = count / PatasPrimitives::PATAS_GROUP_SIZE + (count % PatasPrimitives::PATAS_GROUP_SIZE != 0);
		auto metadata_capacity = metadata_end - PatasPrimitives::HEADER_SIZE;
		if (group_count > metadata_capacity / PatasPrimitives::GROUP_OFFSET_SIZE) {
			ThrowPatasMetadataTableOutOfBounds();
		}
		auto group_offsets_size = group_count * PatasPrimitives::GROUP_OFFSET_SIZE;
		metadata_capacity -= group_offsets_size;
		if (count > metadata_capacity / PatasPrimitives::PACKED_DATA_SIZE) {
			ThrowPatasMetadataTableOutOfBounds();
		}
		auto metadata_size = group_offsets_size + count * PatasPrimitives::PACKED_DATA_SIZE;
		auto metadata_start = metadata_end - metadata_size;
		auto data = reader.GetSubReader(PatasPrimitives::HEADER_SIZE, metadata_start - PatasPrimitives::HEADER_SIZE,
		                                "Patas data");
		auto metadata = reader.GetSubReader(metadata_start, metadata_size, "Patas metadata");
		return {data, metadata};
	}

	explicit PatasScanState(BufferHandle handle_p, ColumnSegment &segment)
	    : handle(std::move(handle_p)), count(segment.count), layout(ReadSegmentLayout(handle, segment, count)),
	      metadata_position(layout.metadata.Size()) {
	}

	BufferHandle handle;
	idx_t count;
	SegmentLayout layout;
	idx_t metadata_position;
	idx_t total_value_count = 0;
	PatasGroupState<EXACT_TYPE> group_state;

	idx_t LeftInGroup() const {
		return PatasPrimitives::PATAS_GROUP_SIZE - (total_value_count % PatasPrimitives::PATAS_GROUP_SIZE);
	}

	inline bool GroupFinished() const {
		return (total_value_count % PatasPrimitives::PATAS_GROUP_SIZE) == 0;
	}

	// Scan up to a group boundary
	template <class EXACT_TYPE, bool SKIP = false>
	void ScanGroup(EXACT_TYPE *values, idx_t group_size) {
		D_ASSERT(group_size <= PatasPrimitives::PATAS_GROUP_SIZE);
		D_ASSERT(group_size <= LeftInGroup());

		if (GroupFinished() && total_value_count < count) {
			if (group_size == PatasPrimitives::PATAS_GROUP_SIZE) {
				LoadGroup<SKIP>(values);
				total_value_count += group_size;
				return;
			} else {
				// Even if SKIP is given, group size is not big enough to be able to fully skip the entire group
				LoadGroup<false>(group_state.values);
			}
		}
		group_state.template Scan<SKIP>((uint8_t *)values, group_size);

		total_value_count += group_size;
	}

	// Using the metadata, we can avoid loading any of the data if we don't care about the group at all
	void SkipGroup() {
		idx_t group_size = MinValue((idx_t)PatasPrimitives::PATAS_GROUP_SIZE, count - total_value_count);
		auto group_metadata_size = PatasPrimitives::GROUP_OFFSET_SIZE + PatasPrimitives::PACKED_DATA_SIZE * group_size;
		D_ASSERT(group_metadata_size <= metadata_position);
		metadata_position -= group_metadata_size;
		total_value_count += group_size;
	}

	template <bool SKIP = false>
	void LoadGroup(EXACT_TYPE *value_buffer) {
		group_state.Reset();

		idx_t group_size = MinValue((idx_t)PatasPrimitives::PATAS_GROUP_SIZE, count - total_value_count);
		auto group_metadata_size = PatasPrimitives::GROUP_OFFSET_SIZE + PatasPrimitives::PACKED_DATA_SIZE * group_size;
		D_ASSERT(group_metadata_size <= metadata_position);
		metadata_position -= group_metadata_size;
		auto metadata = layout.metadata.GetSubReader(metadata_position, group_metadata_size, "Patas group metadata");
		metadata.SetPosition(metadata.Size());
		auto data_byte_offset = metadata.template ReadBackward<PatasPrimitives::GROUP_OFFSET_TYPE>();
		if (data_byte_offset < PatasPrimitives::HEADER_SIZE ||
		    data_byte_offset - PatasPrimitives::HEADER_SIZE > layout.data.Size()) {
			ThrowPatasDataOffsetOutOfBounds();
		}

		auto data_offset = data_byte_offset - PatasPrimitives::HEADER_SIZE;
		group_state.Init(layout.data.GetBytes(data_offset, layout.data.Size() - data_offset).data());

		auto packed_data = metadata.template GetArray<PatasPrimitives::PACKED_DATA_TYPE>(0, group_size);
		group_state.LoadPackedData(packed_data.data(), group_size);

		// Read all the values to the specified 'value_buffer'
		group_state.template LoadValues<SKIP>(value_buffer, group_size);
	}

public:
	//! Skip the next 'skip_count' values, we don't store the values
	void Skip(ColumnSegment &segment, idx_t skip_count) {
		using EXACT_TYPE = typename FloatingToExact<T>::TYPE;

		if (total_value_count != 0 && !GroupFinished()) {
			// Finish skipping the current group
			idx_t to_skip = MinValue<idx_t>(skip_count, LeftInGroup());
			ScanGroup<EXACT_TYPE, true>(nullptr, to_skip);
			skip_count -= to_skip;
		}
		// Figure out how many entire groups we can skip
		// For these groups, we don't even need to process the metadata or values
		idx_t groups_to_skip = skip_count / PatasPrimitives::PATAS_GROUP_SIZE;
		for (idx_t i = 0; i < groups_to_skip; i++) {
			SkipGroup();
		}
		skip_count -= PatasPrimitives::PATAS_GROUP_SIZE * groups_to_skip;
		if (skip_count == 0) {
			return;
		}
		// For the last group that this skip (partially) touches, we do need to
		// load the metadata and values into the group_state
		ScanGroup<EXACT_TYPE, true>(nullptr, skip_count);
	}
};

template <class T>
unique_ptr<SegmentScanState> PatasInitScan(const QueryContext &context, ColumnSegment &segment) {
	auto &buffer_manager = BufferManager::GetBufferManager(segment.GetDatabase());
	auto handle = buffer_manager.Pin(context, segment.GetBlockHandle());
	auto result = make_uniq_base<SegmentScanState, PatasScanState<T>>(std::move(handle), segment);
	return result;
}

//===--------------------------------------------------------------------===//
// Scan base data
//===--------------------------------------------------------------------===//
template <class T>
void PatasScanPartial(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result,
                      idx_t result_offset) {
	using EXACT_TYPE = typename FloatingToExact<T>::TYPE;
	auto &scan_state = (PatasScanState<T> &)*state.scan_state;

	// Get the pointer to the result values
	auto current_result_ptr = FlatVector::GetDataMutableUnsafe<EXACT_TYPE>(result);
	result.SetVectorType(VectorType::FLAT_VECTOR);
	current_result_ptr += result_offset;

	idx_t scanned = 0;
	while (scanned < scan_count) {
		const auto remaining = scan_count - scanned;
		const idx_t to_scan = MinValue(remaining, scan_state.LeftInGroup());

		scan_state.template ScanGroup<EXACT_TYPE>(current_result_ptr + scanned, to_scan);
		scanned += to_scan;
	}
}

template <class T>
void PatasSkip(ColumnSegment &segment, ColumnScanState &state, idx_t skip_count) {
	auto &scan_state = (PatasScanState<T> &)*state.scan_state;
	scan_state.Skip(segment, skip_count);
}

template <class T>
void PatasScan(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result) {
	PatasScanPartial<T>(segment, state, scan_count, result, 0);
}

} // namespace duckdb
