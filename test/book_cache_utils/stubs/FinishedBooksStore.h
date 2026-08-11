#pragma once

#include <string>

class FinishedBooksStore {
 public:
  static FinishedBooksStore& getInstance() {
    static FinishedBooksStore store;
    return store;
  }

  bool removeByPath(const std::string&) { return true; }
  bool updatePath(const std::string&, const std::string&) { return true; }
};

#define FINISHED_BOOKS FinishedBooksStore::getInstance()
