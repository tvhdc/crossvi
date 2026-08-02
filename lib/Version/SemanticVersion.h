#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace ota_version {

struct Components {
  uint32_t major = 0;
  uint32_t minor = 0;
  uint32_t patch = 0;
  std::string_view preRelease;
};

inline bool isIdentifierCharacter(const char value) noexcept {
  return (value >= '0' && value <= '9') || (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
         value == '-';
}

inline bool isValidIdentifierList(const std::string_view identifiers, const bool rejectNumericLeadingZeros) noexcept {
  if (identifiers.empty()) return false;

  size_t start = 0;
  while (start < identifiers.size()) {
    const size_t end = identifiers.find('.', start);
    const size_t length = (end == std::string_view::npos ? identifiers.size() : end) - start;
    if (length == 0) return false;

    bool numeric = true;
    for (size_t i = start; i < start + length; ++i) {
      if (!isIdentifierCharacter(identifiers[i])) return false;
      numeric = numeric && identifiers[i] >= '0' && identifiers[i] <= '9';
    }
    if (rejectNumericLeadingZeros && numeric && length > 1 && identifiers[start] == '0') return false;
    if (end == std::string_view::npos) return true;
    start = end + 1;
  }
  return false;
}

inline bool parse(const std::string_view text, Components& result) noexcept {
  result = {};
  if (text.empty()) return false;

  size_t position = 0;
  if (text[position] == 'v' || text[position] == 'V') {
    if (++position == text.size()) return false;
  }

  const auto readComponent = [&text, &position](uint32_t& output) {
    if (position == text.size() || text[position] < '0' || text[position] > '9') return false;

    const size_t start = position;
    uint32_t value = 0;
    while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
      const uint32_t digit = static_cast<uint32_t>(text[position] - '0');
      if (value > (std::numeric_limits<uint32_t>::max() - digit) / 10U) return false;
      value = value * 10U + digit;
      ++position;
    }
    if (position - start > 1 && text[start] == '0') return false;
    output = value;
    return true;
  };

  if (!readComponent(result.major) || position == text.size() || text[position++] != '.' ||
      !readComponent(result.minor) || position == text.size() || text[position++] != '.' ||
      !readComponent(result.patch)) {
    return false;
  }

  if (position < text.size() && text[position] == '-') {
    const size_t start = ++position;
    const size_t end = text.find('+', position);
    position = end == std::string_view::npos ? text.size() : end;
    result.preRelease = text.substr(start, position - start);
    if (!isValidIdentifierList(result.preRelease, true)) return false;
  }

  if (position < text.size() && text[position] == '+') {
    const std::string_view build = text.substr(position + 1);
    if (!isValidIdentifierList(build, false)) return false;
    position = text.size();
  }
  return position == text.size();
}

inline bool isValid(const std::string_view text) noexcept {
  Components ignored;
  return parse(text, ignored);
}

inline bool isNewer(const std::string_view latest, const std::string_view current) noexcept {
  Components latestVersion;
  Components currentVersion;
  if (!parse(latest, latestVersion) || !parse(current, currentVersion)) return false;

  if (latestVersion.major != currentVersion.major) return latestVersion.major > currentVersion.major;
  if (latestVersion.minor != currentVersion.minor) return latestVersion.minor > currentVersion.minor;
  if (latestVersion.patch != currentVersion.patch) return latestVersion.patch > currentVersion.patch;

  if (latestVersion.preRelease.empty()) return !currentVersion.preRelease.empty();
  if (currentVersion.preRelease.empty()) return false;

  size_t latestStart = 0;
  size_t currentStart = 0;
  while (latestStart < latestVersion.preRelease.size() && currentStart < currentVersion.preRelease.size()) {
    const size_t latestEnd = latestVersion.preRelease.find('.', latestStart);
    const size_t currentEnd = currentVersion.preRelease.find('.', currentStart);
    const std::string_view latestIdentifier = latestVersion.preRelease.substr(
        latestStart, (latestEnd == std::string_view::npos ? latestVersion.preRelease.size() : latestEnd) - latestStart);
    const std::string_view currentIdentifier = currentVersion.preRelease.substr(
        currentStart,
        (currentEnd == std::string_view::npos ? currentVersion.preRelease.size() : currentEnd) - currentStart);

    const bool latestNumeric = latestIdentifier.find_first_not_of("0123456789") == std::string_view::npos;
    const bool currentNumeric = currentIdentifier.find_first_not_of("0123456789") == std::string_view::npos;
    if (latestNumeric != currentNumeric) return !latestNumeric;
    if (latestIdentifier != currentIdentifier) {
      if (latestNumeric && latestIdentifier.size() != currentIdentifier.size()) {
        return latestIdentifier.size() > currentIdentifier.size();
      }
      return latestIdentifier > currentIdentifier;
    }

    latestStart = latestEnd == std::string_view::npos ? latestVersion.preRelease.size() : latestEnd + 1;
    currentStart = currentEnd == std::string_view::npos ? currentVersion.preRelease.size() : currentEnd + 1;
  }
  return latestStart < latestVersion.preRelease.size();
}

}  // namespace ota_version
