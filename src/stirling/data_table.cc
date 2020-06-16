#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "src/common/base/base.h"
#include "src/shared/types/type_utils.h"
#include "src/stirling/data_table.h"
#include "src/stirling/types.h"

namespace pl {
namespace stirling {

using types::ColumnWrapper;
using types::DataType;

DataTable::DataTable(const DataTableSchema& schema) : table_schema_(schema) {}

void DataTable::InitBuffers(types::ColumnWrapperRecordBatch* record_batch_ptr) {
  DCHECK(record_batch_ptr != nullptr);
  DCHECK(record_batch_ptr->empty());

  for (const auto& element : table_schema_.elements()) {
    pl::types::DataType type = element.type();

#define TYPE_CASE(_dt_)                           \
  auto col = types::ColumnWrapper::Make(_dt_, 0); \
  col->Reserve(kTargetCapacity);                  \
  record_batch_ptr->push_back(col);
    PL_SWITCH_FOREACH_DATATYPE(type, TYPE_CASE);
#undef TYPE_CASE
  }
}

Tablet* DataTable::GetTablet(types::TabletIDView tablet_id) {
  auto& tablet = tablets_[tablet_id];
  if (tablet.records.empty()) {
    InitBuffers(&tablet.records);
  }
  return &tablet;
}

namespace {
// Computes a reorder vector that specifies the sorted order.
// Note 1: ColumnWrapper itself is not modified.
// Note 2: There are different ways to define the reorder indexes.
// Here we use the form where the result, idx, is used to sort x according to:
//    { x[idx[0]], x[idx[1]], x[idx[2]], ... }
// From https://stackoverflow.com/questions/1577475/c-sorting-and-keeping-track-of-indexes
std::vector<size_t> SortedIndexes(const std::vector<uint64_t> v) {
  // Initialize original index locations.
  std::vector<size_t> idx(v.size());
  std::iota(idx.begin(), idx.end(), 0);

  // Sort indexes based on comparing values in v using std::stable_sort instead of std::sort
  // to avoid unnecessary index re-orderings when v contains elements of equal values.
  std::stable_sort(idx.begin(), idx.end(), [&v](size_t i1, size_t i2) { return v[i1] < v[i2]; });

  return idx;
}
}  // namespace

std::vector<TaggedRecordBatch> DataTable::ConsumeRecords(uint64_t end_time) {
  std::vector<TaggedRecordBatch> tablets_out;
  absl::flat_hash_map<types::TabletID, Tablet> carryover_tablets;
  uint64_t next_start_time = start_time_;

  for (auto& [tablet_id, tablet] : tablets_) {
    // Sort based on times.
    // TODO(oazizi): Could keep track of whether tablet is already sorted to avoid some work.
    //               Many tables will naturally be in sorted order.
    std::vector<size_t> sort_indexes = SortedIndexes(tablet.times);

    // Split the indexes into three groups:
    // 1) Expired indexes: these are too old to return.
    // 2) Pushable indexes: these are the ones that we return.
    // 3) Carryover indexes: these are too new to return, so hold on to them until the next round.
    // TODO(oazizi): Switch to binary search.
    int num_expired = 0;
    int num_pushable = 0;
    int num_carryover = 0;
    for (const auto& t : tablet.times) {
      if (t < start_time_) {
        ++num_expired;
      } else if (t <= end_time) {
        ++num_pushable;
      } else {
        ++num_carryover;
      }
    }

    // Case 1: Expired records. Just print a message.
    LOG_IF(WARNING, num_expired != 0)
        << absl::Substitute("$0 records dropped due to late arrival", num_expired);

    // Case 2: Pushable records. Copy to output.
    if (num_pushable > 0) {
      // TODO(oazizi): Consider VectorView to avoid copying.
      std::vector<size_t> push_indexes(sort_indexes.begin() + num_expired,
                                       sort_indexes.end() - num_carryover);
      types::ColumnWrapperRecordBatch pushable_records;
      for (auto& col : tablet.records) {
        pushable_records.push_back(col->CopyIndexes(push_indexes));
      }
      uint64_t last_time = tablet.times[push_indexes.back()];
      next_start_time = std::max(next_start_time, last_time);
      tablets_out.push_back(TaggedRecordBatch{tablet_id, std::move(pushable_records)});
    }

    // Case 3: Carryover records.
    if (num_carryover > 0) {
      // TODO(oazizi): Consider VectorView to avoid copying.
      std::vector<size_t> carryover_indexes(sort_indexes.begin() + num_pushable,
                                            sort_indexes.end());
      types::ColumnWrapperRecordBatch carryover_records;
      for (auto& col : tablet.records) {
        carryover_records.push_back(col->CopyIndexes(carryover_indexes));
      }

      std::vector<uint64_t> times(carryover_indexes.size());
      for (size_t i = 0; i < times.size(); ++i) {
        times[i] = tablet.times[carryover_indexes[i]];
      }
      carryover_tablets[tablet_id] =
          Tablet{tablet_id, std::move(times), std::move(carryover_records)};
    }
  }
  tablets_ = std::move(carryover_tablets);

  start_time_ = next_start_time;

  return tablets_out;
}

}  // namespace stirling
}  // namespace pl
