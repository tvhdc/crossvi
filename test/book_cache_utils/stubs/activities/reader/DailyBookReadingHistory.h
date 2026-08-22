#pragma once

#include <string>

class DailyBookReadingHistory {
 public:
  static bool prepareRekey(const std::string&, const std::string&) { return true; }
  static bool finishPreparedRekey() { return true; }
  static bool cancelPreparedRekey(const std::string&, const std::string&) { return true; }
};
