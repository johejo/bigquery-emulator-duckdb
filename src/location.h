#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace bigquery_emulator_duckdb {

// Where a dataset is created, and a job runs, when nothing says otherwise.
inline constexpr char kDefaultLocation[] = "US";

// `location` as BigQuery spells it, the multi-regions in upper case and the regions in lower
// case, compared without case; nothing for a location BigQuery does not have. Throws
// ApiError::Unsupported for a BigQuery Omni location, whose datasets hold only external tables.
std::optional<std::string> CanonicalLocation(std::string_view location);

// The location a `region-REGION` qualifier of INFORMATION_SCHEMA names, or nothing for another
// qualifier or a region BigQuery does not have.
std::optional<std::string> RegionQualifierLocation(std::string_view qualifier);

// The location a request names for its job, as CanonicalLocation spells it, or nothing when it
// names none. Throws ApiError::Invalid for a location BigQuery does not have.
std::optional<std::string> RequestedLocation(const std::optional<std::string>& location);

// The location of a job: the one its request names, or else the first one it references, and the
// US when it references none. Every dataset a job reads or writes must be in the job's location.
class JobLocation {
 public:
  explicit JobLocation(std::optional<std::string> requested = std::nullopt)
      : location_(std::move(requested)) {}

  // Records that the job uses `name`, such as "Dataset project:dataset", which is in `location`.
  // Throws ApiError::NotFound, as BigQuery reports a dataset of another location, when the job's
  // location is already settled and is another.
  void Reference(const std::string& name, const std::string& location);
  // Takes `location` for the job when it has none yet, without checking it, as inferring the
  // location from the datasets a query names does.
  void Infer(const std::string& location);

  [[nodiscard]] const std::optional<std::string>& settled() const { return location_; }
  // The job's location, settling it in the US if nothing has yet.
  const std::string& Resolve();

 private:
  std::optional<std::string> location_;
};

}  // namespace bigquery_emulator_duckdb
