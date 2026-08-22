#include "SleepImageSelectionStore.h"

#include <HalStorage.h>
#include <StagedFileTransaction.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>

#include "SleepImageValidation.h"

namespace SleepImageSelectionStore {
namespace {

constexpr char MARKER_PATH[] = "/.crosspoint/sleep_image_selection_v1.pending";
constexpr char MARKER_TEMP_PATH[] = "/.crosspoint/sleep_image_selection_v1.pending.tmp";
constexpr std::array<uint8_t, 4> MARKER_MAGIC = {'C', 'V', 'S', 'I'};
constexpr uint8_t MARKER_VERSION = 1;
constexpr size_t MARKER_SIZE = 24;

struct Marker {
  Target target = Target::NormalBmp;
  StagedFileTransaction::Digest digest;
};

using Validator = bool (*)(const char* path);

struct TargetSpec {
  const char* finalPath;
  const char* backupPath;
  Validator validator;
};

uint32_t readU32(const uint8_t* bytes) {
  return static_cast<uint32_t>(bytes[0]) | static_cast<uint32_t>(bytes[1]) << 8U |
         static_cast<uint32_t>(bytes[2]) << 16U | static_cast<uint32_t>(bytes[3]) << 24U;
}

uint64_t readU64(const uint8_t* bytes) {
  return static_cast<uint64_t>(readU32(bytes)) | static_cast<uint64_t>(readU32(bytes + 4)) << 32U;
}

void writeU32(uint8_t* bytes, const uint32_t value) {
  bytes[0] = static_cast<uint8_t>(value);
  bytes[1] = static_cast<uint8_t>(value >> 8U);
  bytes[2] = static_cast<uint8_t>(value >> 16U);
  bytes[3] = static_cast<uint8_t>(value >> 24U);
}

void writeU64(uint8_t* bytes, const uint64_t value) {
  writeU32(bytes, static_cast<uint32_t>(value));
  writeU32(bytes + 4, static_cast<uint32_t>(value >> 32U));
}

uint32_t markerHash(const uint8_t* bytes, const size_t size) {
  uint32_t hash = 2166136261U;
  for (size_t index = 0; index < size; ++index) {
    hash ^= bytes[index];
    hash *= 16777619U;
  }
  return hash;
}

TargetSpec specFor(const Target target) {
  switch (target) {
    case Target::NormalBmp:
      return {NORMAL_BMP_PATH, "/sleep.bmp.bak", SleepImageValidation::normalBmp};
    case Target::OverlayBmp:
      return {OVERLAY_BMP_PATH, "/sleep-overlay.bmp.bak", SleepImageValidation::overlayBmp};
    case Target::OverlayPng:
      return {OVERLAY_PNG_PATH, "/sleep-overlay.png.bak", SleepImageValidation::overlayPng};
  }
  return {};
}

bool transactionValidator(const char* path, void* context) {
  const auto validator = static_cast<Validator*>(context);
  return validator && *validator && (*validator)(path);
}

bool removeIfPresent(const char* path) { return !Storage.exists(path) || Storage.remove(path); }

bool digestMatches(const char* path, const TargetSpec& spec, const StagedFileTransaction::Digest& expected) {
  StagedFileTransaction::Digest actual;
  return spec.validator && spec.validator(path) && StagedFileTransaction::digestFile(path, actual) &&
         actual == expected;
}

bool readMarker(Marker& marker) {
  HalFile file;
  if (!Storage.openFileForRead("SLP", MARKER_PATH, file) || file.fileSize64() != MARKER_SIZE) {
    if (file) file.close();
    return false;
  }
  std::array<uint8_t, MARKER_SIZE> bytes{};
  const bool read = file.read(bytes.data(), bytes.size()) == static_cast<int>(bytes.size());
  const bool closed = file.close();
  const uint8_t rawTarget = bytes[5];
  if (!read || !closed || !std::equal(MARKER_MAGIC.begin(), MARKER_MAGIC.end(), bytes.begin()) ||
      bytes[4] != MARKER_VERSION || rawTarget < static_cast<uint8_t>(Target::NormalBmp) ||
      rawTarget > static_cast<uint8_t>(Target::OverlayPng) || bytes[6] != 0 || bytes[7] != 0 ||
      readU32(bytes.data() + 20) != markerHash(bytes.data(), 20)) {
    return false;
  }
  marker.target = static_cast<Target>(rawTarget);
  marker.digest.size = readU64(bytes.data() + 8);
  marker.digest.hash = readU32(bytes.data() + 16);
  return marker.digest.size > 0;
}

bool writeMarker(const Marker& marker) {
  if (!Storage.exists("/.crosspoint") && !Storage.mkdir("/.crosspoint")) return false;
  std::array<uint8_t, MARKER_SIZE> bytes{};
  std::copy(MARKER_MAGIC.begin(), MARKER_MAGIC.end(), bytes.begin());
  bytes[4] = MARKER_VERSION;
  bytes[5] = static_cast<uint8_t>(marker.target);
  writeU64(bytes.data() + 8, marker.digest.size);
  writeU32(bytes.data() + 16, marker.digest.hash);
  writeU32(bytes.data() + 20, markerHash(bytes.data(), 20));

  removeIfPresent(MARKER_TEMP_PATH);
  HalFile file;
  if (!Storage.openFileForWrite("SLP", MARKER_TEMP_PATH, file)) return false;
  bool ok = file.write(bytes.data(), bytes.size()) == bytes.size();
  ok = file.sync() && ok;
  ok = file.close() && ok;
  if (!ok) {
    removeIfPresent(MARKER_TEMP_PATH);
    return false;
  }
  return !Storage.exists(MARKER_PATH) && Storage.rename(MARKER_TEMP_PATH, MARKER_PATH);
}

bool removeConflictingOverlay(const Target target) {
  if (target == Target::NormalBmp) return true;
  const Target conflict = target == Target::OverlayBmp ? Target::OverlayPng : Target::OverlayBmp;
  const TargetSpec spec = specFor(conflict);
  const std::string staging = std::string(spec.finalPath) + ".tmp";
  return removeIfPresent(staging.c_str()) && removeIfPresent(spec.backupPath) && removeIfPresent(spec.finalPath);
}

bool finish(const Marker& marker, const char* stagingPath) {
  const TargetSpec spec = specFor(marker.target);
  if (!spec.finalPath || !spec.validator) return false;

  bool published = digestMatches(spec.finalPath, spec, marker.digest);
  if (!published && stagingPath && digestMatches(stagingPath, spec, marker.digest)) {
    Validator validator = spec.validator;
    published = StagedFileTransaction::publishAndVerify(spec.finalPath, stagingPath, spec.backupPath, marker.digest,
                                                        transactionValidator, &validator) ==
                StagedFileTransaction::Status::Published;
  }
  if (!published) return false;
  if (!removeConflictingOverlay(marker.target)) return false;
  return removeIfPresent(MARKER_PATH) && removeIfPresent(MARKER_TEMP_PATH);
}

bool recoverCanonical(const Target target) {
  TargetSpec spec = specFor(target);
  Validator validator = spec.validator;
  return StagedFileTransaction::recover(spec.finalPath, spec.backupPath, transactionValidator, &validator) !=
         StagedFileTransaction::Status::IoError;
}

}  // namespace

bool recover() {
  if (!Storage.exists(MARKER_PATH)) {
    if (!removeIfPresent(MARKER_TEMP_PATH)) return false;
    for (const Target target : {Target::NormalBmp, Target::OverlayBmp, Target::OverlayPng}) {
      if (!recoverCanonical(target)) return false;
    }
    return true;
  }

  Marker marker;
  if (!readMarker(marker)) return false;
  const std::string staging = std::string(specFor(marker.target).finalPath) + ".tmp";
  if (finish(marker, staging.c_str())) return true;

  // The intended bytes are unavailable or invalid. Restore the prior file of
  // the same format, retain any other-format canonical, and abandon the pick.
  if (!recoverCanonical(marker.target)) return false;
  return removeIfPresent(staging.c_str()) && removeIfPresent(MARKER_PATH) && removeIfPresent(MARKER_TEMP_PATH);
}

bool publish(const Target target, const char* stagingPath) {
  if (!stagingPath || !recover()) return false;
  const TargetSpec spec = specFor(target);
  StagedFileTransaction::Digest digest;
  if (!spec.validator || !spec.validator(stagingPath) || !StagedFileTransaction::digestFile(stagingPath, digest) ||
      digest.size == 0) {
    return false;
  }
  const Marker marker{target, digest};
  if (!writeMarker(marker)) return false;
  return finish(marker, stagingPath);
}

}  // namespace SleepImageSelectionStore
