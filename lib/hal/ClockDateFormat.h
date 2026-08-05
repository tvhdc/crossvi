#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace ClockDateFormat {

enum Format : uint8_t {
  MonthDayYearLong = 0,
  DayMonthYearLong = 1,
  MonthDayYearNumeric = 2,
  DayMonthYearNumeric = 3,
  YearMonthDayNumeric = 4,
  MonthDayNumeric = 5,
  DayMonthNumeric = 6,
  MonthDayLong = 7,
  DayMonthLong = 8,
  FormatCount,
};

enum Separator : uint8_t { Period = 0, Hyphen = 1, Slash = 2, SeparatorCount };

inline char separatorChar(const uint8_t value) {
  switch (value) {
    case Period:
      return '.';
    case Hyphen:
      return '-';
    case Slash:
    default:
      return '/';
  }
}

inline bool format(const uint16_t year, const uint8_t month, const uint8_t day, const uint8_t formatValue,
                   const char numericSeparator, char* output, const size_t outputSize) {
  if (!output || outputSize == 0 || month < 1 || month > 12 || day < 1 || day > 31) return false;

  static constexpr const char* SHORT_MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  static constexpr const char* LONG_MONTHS[] = {"January", "February", "March",     "April",
                                                 "May",     "June",     "July",      "August",
                                                 "September", "October", "November", "December"};
  const char separator = numericSeparator == '.' || numericSeparator == '-' ? numericSeparator : '/';
  int written = -1;
  const uint8_t safeFormat = formatValue < FormatCount ? formatValue : static_cast<uint8_t>(MonthDayYearLong);
  switch (safeFormat) {
    case DayMonthYearLong:
      written = std::snprintf(output, outputSize, "%02u %s %u", static_cast<unsigned>(day), SHORT_MONTHS[month - 1],
                              static_cast<unsigned>(year));
      break;
    case MonthDayYearNumeric:
      written = std::snprintf(output, outputSize, "%02u%c%02u%c%u", static_cast<unsigned>(month), separator,
                              static_cast<unsigned>(day), separator, static_cast<unsigned>(year));
      break;
    case DayMonthYearNumeric:
      written = std::snprintf(output, outputSize, "%02u%c%02u%c%u", static_cast<unsigned>(day), separator,
                              static_cast<unsigned>(month), separator, static_cast<unsigned>(year));
      break;
    case YearMonthDayNumeric:
      written = std::snprintf(output, outputSize, "%u%c%02u%c%02u", static_cast<unsigned>(year), separator,
                              static_cast<unsigned>(month), separator, static_cast<unsigned>(day));
      break;
    case MonthDayNumeric:
      written = std::snprintf(output, outputSize, "%02u%c%02u", static_cast<unsigned>(month), separator,
                              static_cast<unsigned>(day));
      break;
    case DayMonthNumeric:
      written = std::snprintf(output, outputSize, "%02u%c%02u", static_cast<unsigned>(day), separator,
                              static_cast<unsigned>(month));
      break;
    case MonthDayLong:
      written = std::snprintf(output, outputSize, "%s %02u", LONG_MONTHS[month - 1], static_cast<unsigned>(day));
      break;
    case DayMonthLong:
      written = std::snprintf(output, outputSize, "%02u %s", static_cast<unsigned>(day), LONG_MONTHS[month - 1]);
      break;
    case MonthDayYearLong:
    default:
      written = std::snprintf(output, outputSize, "%s %02u, %u", SHORT_MONTHS[month - 1],
                              static_cast<unsigned>(day), static_cast<unsigned>(year));
      break;
  }
  return written >= 0 && static_cast<size_t>(written) < outputSize;
}

}  // namespace ClockDateFormat
