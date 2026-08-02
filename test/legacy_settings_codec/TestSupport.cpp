#include "TestSupport.h"

#include <AtomicJsonFile.h>
#include <HalStorage.h>
#include <I18nKeys.h>

#include <string>
#include <vector>

namespace {
bool failSave = false;
int saveCalls = 0;
std::string savedJson;
}  // namespace

const char* const LANGUAGE_CODES[] = {"EN",  "ES", "FR", "DE", "CS", "PT", "RU", "SV", "RO", "CA", "UK",
                                      "BE",  "IT", "PL", "FI", "DA", "NL", "TR", "KK", "HU", "LT", "SI",
                                      "CAV", "HE", "SK", "VI", "NB", "P2", "AR", "BS", "ID"};

namespace AtomicJsonFile {

AtomicFile::LoadStatus load(const char* path, std::string& json, size_t) {
  if (!Storage.exists(path)) return AtomicFile::LoadStatus::Missing;
  const auto& bytes = Storage.file(path);
  json.assign(bytes.begin(), bytes.end());
  return AtomicFile::LoadStatus::Primary;
}

AtomicFile::SaveStatus save(const char* path, const char* json, const size_t size, size_t) {
  ++saveCalls;
  if (failSave) {
    failSave = false;
    return AtomicFile::SaveStatus::IoError;
  }
  savedJson.assign(json, size);
  Storage.setFile(path, std::vector<uint8_t>(savedJson.begin(), savedJson.end()));
  return AtomicFile::SaveStatus::Saved;
}

}  // namespace AtomicJsonFile

namespace LegacySettingsTestSupport {

void resetAtomicJson() {
  failSave = false;
  saveCalls = 0;
  savedJson.clear();
}

void failNextJsonSave() { failSave = true; }

int jsonSaveCalls() { return saveCalls; }

const std::string& lastSavedJson() { return savedJson; }

}  // namespace LegacySettingsTestSupport
