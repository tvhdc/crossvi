#pragma once

#include <cstdint>
#include <string>

#include "SleepImageSelectionStore.h"

namespace SleepImageNormalizer {

constexpr uint64_t MAX_SOURCE_BYTES = 16ULL * 1024ULL * 1024ULL;
constexpr uint64_t MAX_PASSTHROUGH_OVERLAY_BYTES = 2ULL * 1024ULL * 1024ULL;

enum class Status : uint8_t { Ready, TooLarge, Invalid, IoError };

struct Result {
  Status status = Status::Invalid;
  SleepImageSelectionStore::Target target = SleepImageSelectionStore::Target::NormalBmp;
  uint64_t sourceBytes = 0;
  uint64_t outputBytes = 0;
  bool optimized = false;
};

const char* stagingPath(SleepImageSelectionStore::Target target);

// Prepares, syncs and validates one canonical staging file. The source is
// never modified. Publication and settings changes remain the caller's job.
Result prepare(const std::string& sourcePath, bool transparent, int screenWidth, int screenHeight);

}  // namespace SleepImageNormalizer
