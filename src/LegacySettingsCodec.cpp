#include "LegacySettingsCodec.h"

#include <cstring>

namespace LegacySettingsV2 {
namespace {

constexpr bool isStringField(const uint8_t field) {
  return field == OpdsUrl || field == OpdsUsername || field == OpdsPassword;
}

constexpr uint32_t maximumStringBytes(const uint8_t field) { return field == OpdsUrl ? 127U : 63U; }

}  // namespace

DecodeStatus decode(const uint8_t* data, const size_t size, Decoded& output) {
  if (!data || size < 2 || size > MAX_ENCODED_BYTES) return DecodeStatus::Invalid;
  if (data[0] > VERSION) return DecodeStatus::FutureVersion;
  if (data[0] != VERSION || data[1] > FIELD_COUNT) return DecodeStatus::Invalid;

  Decoded decoded{};
  decoded.count = data[1];
  size_t cursor = 2;
  for (uint8_t field = 0; field < decoded.count; ++field) {
    if (!isStringField(field)) {
      if (cursor >= size) return DecodeStatus::Invalid;
      decoded.values[field] = data[cursor++];
      continue;
    }

    if (size - cursor < sizeof(uint32_t)) return DecodeStatus::Invalid;
    uint32_t length = 0;
    std::memcpy(&length, data + cursor, sizeof(length));
    cursor += sizeof(length);
    if (length > maximumStringBytes(field) || length > size - cursor) return DecodeStatus::Invalid;
    cursor += length;
  }

  if (cursor != size) return DecodeStatus::Invalid;
  output = decoded;
  return DecodeStatus::Ok;
}

StatusBarValues statusBarValues(const uint8_t legacyMode) {
  // Values match CrossPointSettings' persisted status-bar enums without
  // depending on the singleton in this small migration codec.
  switch (legacyMode) {
    case 0:  // NONE
      return {0, 0, 2, 2, 0};
    case 1:  // NO_PROGRESS
      return {0, 0, 2, 1, 1};
    case 3:  // BOOK_PROGRESS_BAR
      return {1, 0, 0, 1, 1};
    case 4:  // ONLY_BOOK_PROGRESS_BAR
      return {1, 0, 0, 2, 0};
    case 5:  // CHAPTER_PROGRESS_BAR
      return {0, 1, 1, 1, 1};
    case 2:  // FULL
    default:
      return {1, 1, 2, 1, 1};
  }
}

bool selectLegacyStatusBarMode(const bool canonicalPresent, const bool legacyPresent, const int legacyMode,
                               uint8_t& selectedMode) {
  if (canonicalPresent) return false;
  selectedMode = legacyPresent && legacyMode >= 0 && legacyMode < 6 ? static_cast<uint8_t>(legacyMode) : 2;
  return true;
}

}  // namespace LegacySettingsV2
