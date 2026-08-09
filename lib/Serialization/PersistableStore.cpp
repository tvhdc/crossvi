#include "PersistableStore.h"

#include <AtomicJsonFile.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include <cstring>
#include <limits>

bool PersistableStoreBase::writeDocToFile(const char* path, const JsonDocument& doc) {
  Storage.mkdir("/.crosspoint");
  String json;
  serializeJson(doc, json);
  const AtomicFile::SaveStatus saved = AtomicJsonFile::save(path, json);
  if (saved != AtomicFile::SaveStatus::Saved && saved != AtomicFile::SaveStatus::Unchanged) {
    LOG_ERR("PERSIST", "Failed to write %s", path);
    return false;
  }
  return true;
}

AtomicFile::LoadStatus PersistableStoreBase::readDocFromFile(const char* path, JsonDocument& doc) {
  std::string json;
  const AtomicFile::LoadStatus loaded = AtomicJsonFile::load(path, json);
  if (loaded == AtomicFile::LoadStatus::Missing) return loaded;
  if (loaded != AtomicFile::LoadStatus::Primary && loaded != AtomicFile::LoadStatus::Backup &&
      loaded != AtomicFile::LoadStatus::Temp) {
    LOG_ERR("PERSIST", "Failed to recover valid JSON from %s", path);
    return loaded;
  }
  auto error = deserializeJson(doc, json.data(), json.size());
  if (error) {
    LOG_ERR("PERSIST", "JSON parse error in %s: %s", path, error.c_str());
    return AtomicFile::LoadStatus::Invalid;
  }
  return loaded;
}

std::string PersistableStoreBase::extractPassword(JsonVariantConst doc, bool& needsResave) {
  bool valid = false;
  return extractPassword(doc, needsResave, std::numeric_limits<size_t>::max(), valid);
}

std::string PersistableStoreBase::extractPassword(JsonVariantConst doc, bool& needsResave, const size_t maxLength,
                                                  bool& valid) {
  valid = true;
  bool ok = false;
  bool tooLong = false;
  std::string pass = obfuscation::deobfuscateFromBase64(doc["password_obf"] | "", maxLength, &ok, &tooLong);
  if (tooLong) {
    valid = false;
    return "";
  }
  if (!ok) {
    // Deobfuscation failed — fall back to legacy plaintext password.
    const char* legacyPassword = doc["password"] | "";
    const size_t legacyLength = std::strlen(legacyPassword);
    if (legacyLength > maxLength) {
      valid = false;
      return "";
    }
    pass.assign(legacyPassword, legacyLength);
    if (!pass.empty()) needsResave = true;
  }
  // A successfully decoded empty string is a legitimate value; preserve as-is.
  return pass;
}
