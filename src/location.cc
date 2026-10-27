#include "src/location.h"

#include <algorithm>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

#include "src/api_error.h"
#include "src/duckdb_sql.h"

namespace bigquery_emulator_duckdb {
namespace {

// The dataset locations BigQuery lists, third_party/bigquery_docs/locations.md.
constexpr std::string_view kLocations[] = {
    "US",
    "EU",
    // Americas
    "us-east5",
    "us-south1",
    "us-central1",
    "us-west2",
    "us-west4",
    "northamerica-south1",
    "northamerica-northeast1",
    "us-east4",
    "us-central2",
    "us-west1",
    "us-west3",
    "southamerica-east1",
    "southamerica-west1",
    "us-east1",
    "northamerica-northeast2",
    // Asia Pacific
    "asia-southeast3",
    "asia-south2",
    "asia-east2",
    "asia-southeast2",
    "australia-southeast2",
    "asia-south1",
    "asia-northeast2",
    "asia-northeast3",
    "asia-southeast1",
    "australia-southeast1",
    "asia-east1",
    "asia-northeast1",
    // Europe
    "europe-west1",
    "europe-west10",
    "europe-north1",
    "europe-west3",
    "europe-west2",
    "europe-southwest1",
    "europe-west8",
    "europe-west4",
    "europe-west9",
    "europe-north2",
    "europe-west12",
    "europe-central2",
    "europe-west6",
    // Middle East
    "me-central2",
    "me-central1",
    "me-west1",
    // Africa
    "africa-south1",
};

constexpr std::string_view kOmniLocations[] = {
    "aws-us-east-1", "aws-us-west-2",    "aws-ap-northeast-2", "aws-ap-southeast-2",
    "aws-eu-west-1", "aws-eu-central-1", "azure-eastus2",
};

// The location of kLocations that `folded`, in lower case, names.
std::optional<std::string> KnownLocation(const std::string& folded) {
  const auto* const it = std::ranges::find_if(
      kLocations, [&](std::string_view known) { return ToLowerAscii(known) == folded; });
  return it == std::end(kLocations) ? std::nullopt : std::optional<std::string>(*it);
}

}  // namespace

std::optional<std::string> CanonicalLocation(std::string_view location) {
  const std::string folded = ToLowerAscii(location);
  if (std::optional<std::string> known = KnownLocation(folded)) {
    return known;
  }
  if (std::ranges::find(kOmniLocations, folded) != std::end(kOmniLocations)) {
    throw ApiError::Unsupported("BigQuery Omni location " + folded);
  }
  return std::nullopt;
}

std::optional<std::string> RequestedLocation(const std::optional<std::string>& location) {
  if (!location.has_value()) {
    return std::nullopt;
  }
  std::optional<std::string> canonical = CanonicalLocation(*location);
  if (!canonical.has_value()) {
    throw ApiError::Invalid("Invalid location: " + *location);
  }
  return canonical;
}

std::optional<std::string> RegionQualifierLocation(std::string_view qualifier) {
  if (!qualifier.starts_with(kRegionPrefix)) {
    return std::nullopt;
  }
  return KnownLocation(ToLowerAscii(qualifier.substr(kRegionPrefix.size())));
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): what is used, then where it is.
void JobLocation::Reference(const std::string& name, const std::string& location) {
  if (!location_.has_value()) {
    location_ = location;
  } else if (*location_ != location) {
    throw ApiError::NotFound("Not found: " + name + " was not found in location " + *location_);
  }
}

void JobLocation::Infer(const std::string& location) {
  if (!location_.has_value()) {
    location_ = location;
  }
}

const std::string& JobLocation::Resolve() {
  if (!location_.has_value()) {
    location_ = kDefaultLocation;
  }
  return *location_;
}

}  // namespace bigquery_emulator_duckdb
