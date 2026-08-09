#pragma once

#include <cstdint>

class LazyStoreState {
 public:
  enum class Value : uint8_t { NotLoaded, Loading, Loaded, Failed };

  bool beginLoad() {
    if (value_ != Value::NotLoaded) return false;
    value_ = Value::Loading;
    return true;
  }
  bool usable() const { return value_ == Value::Loading || value_ == Value::Loaded; }
  bool failed() const { return value_ == Value::Failed; }
  bool loaded() const { return value_ == Value::Loaded; }
  void finish(const bool success) { value_ = success ? Value::Loaded : Value::Failed; }
  void markLoaded() { value_ = Value::Loaded; }

 private:
  Value value_ = Value::NotLoaded;
};
