#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace NextBookFinder {

enum class StepResult { Pending, Complete, Error };

class Scan {
 public:
  Scan() = default;
  ~Scan();
  Scan(const Scan&) = delete;
  Scan& operator=(const Scan&) = delete;

  bool begin(const std::string& currentBookPath, size_t maxCount);
  StepResult step(size_t maxEntries);
  const std::vector<std::string>& result() const { return result_; }
  const std::string& folder() const { return folder_; }

 private:
  HalFile directory_;
  std::unique_ptr<char[]> nameBuffer_;
  std::string folder_;
  std::string currentName_;
  std::vector<std::string> result_;
  size_t maxCount_ = 0;
  size_t entriesScanned_ = 0;
  bool active_ = false;
};

}  // namespace NextBookFinder
