#include "SleepImageSelectionStore.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <AtomicFile.h>
#include <HalStorage.h>
#include <StagedFileTransaction.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#include "SleepImageValidation.h"

namespace SleepImageSelectionStore {
namespace {

constexpr char MARKER_PATH[] = "/.crosspoint/sleep_image_selection_v1.pending";
constexpr char MARKER_TEMP_PATH[] = "/.crosspoint/sleep_image_selection_v1.pending.tmp";
constexpr std::array<uint8_t, 4> MARKER_MAGIC = {'C', 'V', 'S', 'I'};
constexpr uint8_t MARKER_VERSION = 1;
constexpr size_t MARKER_SIZE = 24;
constexpr char CATALOG_PATH[] = "/.crosspoint/sleep_images.json";
constexpr char MANAGED_DIRECTORY[] = "/.sleep-overlay";
constexpr uint8_t CATALOG_VERSION = 1;
constexpr uint16_t FIRST_MANAGED_ID = 0x8000;
constexpr size_t MAX_MANIFEST_BYTES = 16U * 1024U;
constexpr size_t MAX_PATH_BYTES = 512;
constexpr size_t MAX_NAME_BYTES = 256;
constexpr uint8_t MIN_ZOOM = 50;
constexpr uint8_t MAX_ZOOM = 200;
constexpr int16_t MIN_OFFSET = -1024;
constexpr int16_t MAX_OFFSET = 1024;
constexpr uint16_t MAX_LEGACY_DIRECTORY_ENTRIES = 4096;
constexpr uint8_t DIRECTORY_YIELD_INTERVAL = 16;

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

  const bool canonicalAlreadyMatches = digestMatches(spec.finalPath, spec, marker.digest);
  bool published = canonicalAlreadyMatches;
  if (!published && stagingPath && digestMatches(stagingPath, spec, marker.digest)) {
    Validator validator = spec.validator;
    published = StagedFileTransaction::publishAndVerify(spec.finalPath, stagingPath, spec.backupPath, marker.digest,
                                                        transactionValidator,
                                                        &validator) == StagedFileTransaction::Status::Published;
  }
  if (!published) return false;
  if (canonicalAlreadyMatches && stagingPath && strcmp(stagingPath, spec.finalPath) != 0 &&
      !removeIfPresent(stagingPath)) {
    return false;
  }
  if (!removeConflictingOverlay(marker.target)) return false;
  return removeIfPresent(MARKER_PATH) && removeIfPresent(MARKER_TEMP_PATH);
}

bool recoverCanonical(const Target target) {
  TargetSpec spec = specFor(target);
  Validator validator = spec.validator;
  return StagedFileTransaction::recover(spec.finalPath, spec.backupPath, transactionValidator, &validator) !=
         StagedFileTransaction::Status::IoError;
}

bool hasExtension(const std::string_view path, const std::string_view extension) {
  if (path.size() < extension.size()) return false;
  const size_t offset = path.size() - extension.size();
  for (size_t i = 0; i < extension.size(); ++i) {
    const unsigned char left = static_cast<unsigned char>(path[offset + i]);
    const unsigned char right = static_cast<unsigned char>(extension[i]);
    const char lowerLeft = left >= 'A' && left <= 'Z' ? static_cast<char>(left + ('a' - 'A')) : static_cast<char>(left);
    const char lowerRight =
        right >= 'A' && right <= 'Z' ? static_cast<char>(right + ('a' - 'A')) : static_cast<char>(right);
    if (lowerLeft != lowerRight) return false;
  }
  return true;
}

bool validPath(const std::string& path) {
  if (path.empty() || path.size() > MAX_PATH_BYTES || path[0] != '/') return false;
  return std::none_of(path.begin(), path.end(), [](const unsigned char value) { return value < 0x20U; });
}

bool validName(const std::string& name) {
  if (name.empty() || name.size() > MAX_NAME_BYTES) return false;
  return std::none_of(name.begin(), name.end(), [](const unsigned char value) { return value < 0x20U; });
}

bool validTransform(const ImageTransform& transform) {
  return transform.zoom >= MIN_ZOOM && transform.zoom <= MAX_ZOOM && transform.offsetX >= MIN_OFFSET &&
         transform.offsetX <= MAX_OFFSET && transform.offsetY >= MIN_OFFSET && transform.offsetY <= MAX_OFFSET;
}

bool validCatalog(const Catalog& catalog) {
  if (catalog.nextId < FIRST_MANAGED_ID || catalog.images.size() > MAX_IMAGES) return false;
  for (size_t i = 0; i < catalog.images.size(); ++i) {
    const ImageEntry& entry = catalog.images[i];
    if (entry.id < FIRST_MANAGED_ID || !validPath(entry.path) || !validName(entry.name) ||
        !validTransform(entry.transform)) {
      return false;
    }
    for (size_t other = 0; other < i; ++other) {
      if (catalog.images[other].id == entry.id || catalog.images[other].path == entry.path) return false;
    }
  }
  return true;
}

bool parseCatalog(const uint8_t* data, const size_t size, Catalog& catalog) {
  JsonDocument document;
  if (!data || size == 0 || deserializeJson(document, data, size) || !document.is<JsonObjectConst>()) return false;
  const JsonObjectConst root = document.as<JsonObjectConst>();
  if (!root["version"].is<uint32_t>() || root["version"].as<uint32_t>() != CATALOG_VERSION ||
      !root["nextId"].is<uint32_t>() || !root["images"].is<JsonArrayConst>()) {
    return false;
  }
  const uint32_t rawNextId = root["nextId"].as<uint32_t>();
  const JsonArrayConst images = root["images"].as<JsonArrayConst>();
  if (rawNextId < FIRST_MANAGED_ID || rawNextId > UINT16_MAX || images.size() > MAX_IMAGES) return false;

  Catalog parsed;
  parsed.nextId = static_cast<uint16_t>(rawNextId);
  parsed.images.reserve(images.size());
  for (const JsonVariantConst value : images) {
    if (!value.is<JsonObjectConst>()) return false;
    const JsonObjectConst object = value.as<JsonObjectConst>();
    if (!object["id"].is<uint32_t>() || !object["path"].is<const char*>() || !object["name"].is<const char*>() ||
        !object["zoom"].is<uint32_t>() || !object["x"].is<int32_t>() || !object["y"].is<int32_t>()) {
      return false;
    }
    const uint32_t rawId = object["id"].as<uint32_t>();
    const uint32_t rawZoom = object["zoom"].as<uint32_t>();
    const int32_t rawX = object["x"].as<int32_t>();
    const int32_t rawY = object["y"].as<int32_t>();
    if (rawId < FIRST_MANAGED_ID || rawId > UINT16_MAX || rawZoom > UINT8_MAX || rawX < INT16_MIN || rawX > INT16_MAX ||
        rawY < INT16_MIN || rawY > INT16_MAX) {
      return false;
    }
    parsed.images.push_back({static_cast<uint16_t>(rawId),
                             object["path"].as<const char*>(),
                             object["name"].as<const char*>(),
                             {static_cast<uint8_t>(rawZoom), static_cast<int16_t>(rawX), static_cast<int16_t>(rawY)}});
  }
  parsed.loaded = true;
  if (!validCatalog(parsed)) return false;
  catalog = std::move(parsed);
  return true;
}

bool catalogValidator(const uint8_t* data, const size_t size, void*) {
  Catalog parsed;
  return parseCatalog(data, size, parsed);
}

bool serializeCatalog(const Catalog& catalog, std::string& output) {
  if (!catalog.loaded || !validCatalog(catalog)) return false;
  JsonDocument document;
  document["version"] = CATALOG_VERSION;
  document["nextId"] = catalog.nextId;
  JsonArray images = document["images"].to<JsonArray>();
  for (const ImageEntry& entry : catalog.images) {
    JsonObject object = images.add<JsonObject>();
    object["id"] = entry.id;
    object["path"] = entry.path;
    object["name"] = entry.name;
    object["zoom"] = entry.transform.zoom;
    object["x"] = entry.transform.offsetX;
    object["y"] = entry.transform.offsetY;
  }
  const size_t measured = measureJson(document);
  if (measured == 0 || measured > MAX_MANIFEST_BYTES) return false;
  output.assign(measured + 1, '\0');
  const size_t written = serializeJson(document, output.data(), output.size());
  output.resize(written);
  return written == measured;
}

CatalogStatus mapSaveStatus(const AtomicFile::SaveStatus status) {
  switch (status) {
    case AtomicFile::SaveStatus::Saved:
    case AtomicFile::SaveStatus::Unchanged:
      return CatalogStatus::Ok;
    case AtomicFile::SaveStatus::Oversize:
    case AtomicFile::SaveStatus::InvalidExistingState:
      return CatalogStatus::Invalid;
    case AtomicFile::SaveStatus::IoError:
      return CatalogStatus::IoError;
  }
  return CatalogStatus::IoError;
}

CatalogStatus saveCatalog(const Catalog& catalog, const bool rotateIfUnchanged = false) {
  if (!Storage.ensureDirectoryExists("/.crosspoint")) return CatalogStatus::IoError;
  std::string json;
  if (!serializeCatalog(catalog, json)) return CatalogStatus::Invalid;
  return mapSaveStatus(AtomicFile::save(CATALOG_PATH, reinterpret_cast<const uint8_t*>(json.data()), json.size(),
                                        MAX_MANIFEST_BYTES, catalogValidator, nullptr, rotateIfUnchanged));
}

uint16_t nextId(const uint16_t id) { return id == UINT16_MAX ? FIRST_MANAGED_ID : static_cast<uint16_t>(id + 1U); }

bool catalogContainsId(const Catalog& catalog, const uint16_t id) {
  return std::any_of(catalog.images.begin(), catalog.images.end(),
                     [id](const ImageEntry& entry) { return entry.id == id; });
}

std::string managedPath(const uint16_t id, const char* extension) {
  char path[64];
  snprintf(path, sizeof(path), "%s/crossvi-%u%s", MANAGED_DIRECTORY, static_cast<unsigned>(id), extension);
  return path;
}

bool managedIdAvailable(const Catalog& catalog, const uint16_t id) {
  if (catalogContainsId(catalog, id)) return false;
  for (const char* extension : {".bmp", ".png"}) {
    const std::string path = managedPath(id, extension);
    if (Storage.exists(path.c_str()) || Storage.exists((path + ".bak").c_str()) ||
        Storage.exists((path + ".tmp").c_str())) {
      return false;
    }
  }
  return true;
}

bool allocateManagedId(Catalog& catalog, uint16_t& id) {
  uint16_t candidate = std::max(catalog.nextId, FIRST_MANAGED_ID);
  // A valid catalog contains at most 16 IDs and a failed transaction can
  // leave only a small bounded number of sibling files. Avoid an unbounded SD
  // scan if a manually populated managed directory occupies the ID range.
  constexpr size_t MAX_ATTEMPTS = MAX_IMAGES * 4U;
  for (size_t attempt = 0; attempt < MAX_ATTEMPTS; ++attempt) {
    if (managedIdAvailable(catalog, candidate)) {
      id = candidate;
      catalog.nextId = nextId(candidate);
      return true;
    }
    candidate = nextId(candidate);
  }
  return false;
}

std::string nameFromPath(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool appendLegacy(Catalog& catalog, const std::string& path, const ImageTransform transform) {
  if (catalog.images.size() >= MAX_IMAGES || !validPath(path)) return false;
  if (std::any_of(catalog.images.begin(), catalog.images.end(),
                  [&path](const ImageEntry& entry) { return entry.path == path; })) {
    return true;
  }
  const std::string name = nameFromPath(path);
  if (!validName(name)) return false;
  uint16_t id = 0;
  if (!allocateManagedId(catalog, id)) return false;
  catalog.images.push_back({id, path, name, transform});
  return true;
}

enum class DirectoryScanStatus : uint8_t { Empty, Found, IoError };

bool validateLegacyOverlay(const char* path) {
  if (!path) return false;
  return hasExtension(path, ".png") ? SleepImageValidation::overlayPng(path) : SleepImageValidation::overlayBmp(path);
}

DirectoryScanStatus appendLegacyDirectory(Catalog& catalog, const char* directoryPath, const Validator validator,
                                          const ImageTransform transform) {
  if (!Storage.exists(directoryPath)) return DirectoryScanStatus::Empty;
  HalFile directory = Storage.open(directoryPath);
  if (!directory) return DirectoryScanStatus::IoError;
  if (!directory.isDirectory()) {
    if (!directory.close()) return DirectoryScanStatus::IoError;
    return DirectoryScanStatus::Empty;
  }

  const size_t initialSize = catalog.images.size();
  uint16_t entries = 0;
  char name[MAX_NAME_BYTES + 1];
  while (catalog.images.size() < MAX_IMAGES && entries < MAX_LEGACY_DIRECTORY_ENTRIES) {
    HalFile file = directory.openNextFile();
    if (!file) break;
    ++entries;
    if (entries % DIRECTORY_YIELD_INTERVAL == 0) yield();
    if (file.isDirectory()) {
      if (!file.close()) {
        directory.close();
        return DirectoryScanStatus::IoError;
      }
      continue;
    }
    const size_t length = file.getName(name, sizeof(name));
    const bool closed = file.close();
    if (!closed) {
      directory.close();
      return DirectoryScanStatus::IoError;
    }
    if (length == 0 || length >= sizeof(name) || name[0] == '.') continue;
    const std::string filename(name, length);
    if ((!hasExtension(filename, ".bmp") && !hasExtension(filename, ".png")) || !validName(filename)) continue;
    const std::string path = std::string(directoryPath) + "/" + filename;
    if (!validPath(path) || !validator(path.c_str())) continue;
    if (!appendLegacy(catalog, path, transform)) {
      directory.close();
      return DirectoryScanStatus::IoError;
    }
  }
  const bool scanError = directory.getError() != 0;
  const bool closed = directory.close();
  if (scanError || !closed) return DirectoryScanStatus::IoError;
  return catalog.images.size() == initialSize ? DirectoryScanStatus::Empty : DirectoryScanStatus::Found;
}

CatalogStatus migrateLegacyCatalog(Catalog& catalog, const ImageTransform transform) {
  if (!validTransform(transform)) return CatalogStatus::Invalid;
  catalog = {};
  catalog.loaded = true;

  if (SleepImageValidation::overlayBmp(OVERLAY_BMP_PATH)) {
    if (!appendLegacy(catalog, OVERLAY_BMP_PATH, transform)) return CatalogStatus::IoError;
  } else if (SleepImageValidation::overlayPng(OVERLAY_PNG_PATH)) {
    if (!appendLegacy(catalog, OVERLAY_PNG_PATH, transform)) return CatalogStatus::IoError;
  } else {
    const DirectoryScanStatus hidden =
        appendLegacyDirectory(catalog, "/.sleep-overlay", validateLegacyOverlay, transform);
    if (hidden == DirectoryScanStatus::IoError) return CatalogStatus::IoError;
    if (hidden == DirectoryScanStatus::Empty) {
      const DirectoryScanStatus visible =
          appendLegacyDirectory(catalog, "/sleep-overlay", validateLegacyOverlay, transform);
      if (visible == DirectoryScanStatus::IoError) return CatalogStatus::IoError;
    }
  }

  if (catalog.images.size() < MAX_IMAGES) {
    if (SleepImageValidation::normalBmp(NORMAL_BMP_PATH)) {
      if (!appendLegacy(catalog, NORMAL_BMP_PATH, transform)) return CatalogStatus::IoError;
    } else {
      const DirectoryScanStatus hidden =
          appendLegacyDirectory(catalog, "/.sleep", SleepImageValidation::overlayBmp, transform);
      if (hidden == DirectoryScanStatus::IoError) return CatalogStatus::IoError;
      if (hidden == DirectoryScanStatus::Empty) {
        const DirectoryScanStatus visible =
            appendLegacyDirectory(catalog, "/sleep", SleepImageValidation::overlayBmp, transform);
        if (visible == DirectoryScanStatus::IoError) return CatalogStatus::IoError;
      }
    }
  }
  return CatalogStatus::Ok;
}

bool transactionValidatorForTarget(const char* path, void* context) {
  const auto validator = static_cast<Validator*>(context);
  return validator && *validator && (*validator)(path);
}

CatalogStatus rollbackCatalog(Catalog& catalog, const Catalog& previous, const CatalogStatus failedStatus) {
  catalog = previous;
  if (saveCatalog(previous) != CatalogStatus::Ok) {
    catalog.loaded = false;
    return CatalogStatus::IoError;
  }
  return failedStatus;
}

bool isManagedEntry(const ImageEntry& entry) {
  if (entry.id < FIRST_MANAGED_ID) return false;
  if (entry.path == managedPath(entry.id, ".png")) return true;
  return entry.path == managedPath(entry.id, ".bmp");
}

}  // namespace

bool recoverInternal(const char* const preservedStagingPath) {
  if (!Storage.exists(MARKER_PATH)) {
    if (!removeIfPresent(MARKER_TEMP_PATH)) return false;
    constexpr std::array targets = {Target::NormalBmp, Target::OverlayBmp, Target::OverlayPng};
    for (const Target target : targets) {
      const std::string staging = std::string(specFor(target).finalPath) + ".tmp";
      if ((!preservedStagingPath || staging != preservedStagingPath) && !removeIfPresent(staging.c_str())) return false;
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

bool recover() { return recoverInternal(nullptr); }

bool publish(const Target target, const char* stagingPath) {
  if (!stagingPath || !recoverInternal(stagingPath)) return false;
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

CatalogStatus loadCatalog(Catalog& catalog, const ImageTransform legacyTransform) {
  catalog = {};
  if (!recover()) return CatalogStatus::IoError;
  std::string json;
  const AtomicFile::LoadStatus status = AtomicFile::load(CATALOG_PATH, json, MAX_MANIFEST_BYTES, catalogValidator);
  switch (status) {
    case AtomicFile::LoadStatus::Primary:
    case AtomicFile::LoadStatus::Backup:
    case AtomicFile::LoadStatus::Temp:
      if (!parseCatalog(reinterpret_cast<const uint8_t*>(json.data()), json.size(), catalog)) {
        return CatalogStatus::Invalid;
      }
      return CatalogStatus::Ok;
    case AtomicFile::LoadStatus::Missing: {
      CatalogStatus migrated = migrateLegacyCatalog(catalog, legacyTransform);
      if (migrated != CatalogStatus::Ok) {
        catalog.loaded = false;
        return migrated;
      }
      migrated = saveCatalog(catalog);
      if (migrated != CatalogStatus::Ok) catalog.loaded = false;
      return migrated;
    }
    case AtomicFile::LoadStatus::Invalid:
    case AtomicFile::LoadStatus::Oversize:
      return CatalogStatus::Invalid;
    case AtomicFile::LoadStatus::IoError:
      return CatalogStatus::IoError;
  }
  return CatalogStatus::IoError;
}

CatalogStatus addPreparedImage(Catalog& catalog, const Target target, const char* stagingPath,
                               const std::string& displayName, ImageEntry* added) {
  if (!catalog.loaded || !validCatalog(catalog) || !stagingPath || target == Target::NormalBmp) {
    return CatalogStatus::Invalid;
  }
  if (catalog.images.size() >= MAX_IMAGES) return CatalogStatus::Full;
  const TargetSpec spec = specFor(target);
  if (!spec.validator || !spec.validator(stagingPath)) return CatalogStatus::Invalid;
  const std::string name = displayName.empty() ? nameFromPath(stagingPath) : displayName;
  if (!validName(name)) return CatalogStatus::Invalid;
  if (!Storage.ensureDirectoryExists(MANAGED_DIRECTORY)) return CatalogStatus::IoError;

  const Catalog previous = catalog;
  uint16_t id = 0;
  if (!allocateManagedId(catalog, id)) {
    catalog = previous;
    return CatalogStatus::Full;
  }
  const char* extension = target == Target::OverlayPng ? ".png" : ".bmp";
  const std::string path = managedPath(id, extension);
  const std::string backupPath = path + ".bak";
  StagedFileTransaction::Digest digest;
  if (!StagedFileTransaction::digestFile(stagingPath, digest) || digest.size == 0) {
    catalog = previous;
    return CatalogStatus::IoError;
  }
  Validator validator = spec.validator;
  const StagedFileTransaction::Status published = StagedFileTransaction::publishAndVerify(
      path.c_str(), stagingPath, backupPath.c_str(), digest, transactionValidatorForTarget, &validator);
  if (published != StagedFileTransaction::Status::Published) {
    catalog = previous;
    return published == StagedFileTransaction::Status::InvalidStaging ? CatalogStatus::Invalid : CatalogStatus::IoError;
  }

  const ImageEntry entry{id, path, name, {}};
  catalog.images.push_back(entry);
  const CatalogStatus saved = saveCatalog(catalog);
  if (saved != CatalogStatus::Ok) {
    const CatalogStatus rolledBack = rollbackCatalog(catalog, previous, saved);
    if (catalog.loaded) {
      removeIfPresent(path.c_str());
      removeIfPresent(backupPath.c_str());
    }
    return rolledBack;
  }
  if (added) *added = entry;
  return CatalogStatus::Ok;
}

CatalogStatus updateTransform(Catalog& catalog, const uint16_t id, ImageTransform transform) {
  if (!catalog.loaded || !validCatalog(catalog)) return CatalogStatus::Invalid;
  ImageEntry* entry = findImage(catalog, id);
  if (!entry) return CatalogStatus::NotFound;
  transform.zoom = std::clamp(transform.zoom, MIN_ZOOM, MAX_ZOOM);
  transform.offsetX = std::clamp(transform.offsetX, MIN_OFFSET, MAX_OFFSET);
  transform.offsetY = std::clamp(transform.offsetY, MIN_OFFSET, MAX_OFFSET);
  if (entry->transform.zoom == transform.zoom && entry->transform.offsetX == transform.offsetX &&
      entry->transform.offsetY == transform.offsetY) {
    return CatalogStatus::Ok;
  }
  const Catalog previous = catalog;
  entry->transform = transform;
  const CatalogStatus saved = saveCatalog(catalog);
  return saved == CatalogStatus::Ok ? CatalogStatus::Ok : rollbackCatalog(catalog, previous, saved);
}

CatalogStatus removeImage(Catalog& catalog, const uint16_t id) {
  if (!catalog.loaded || !validCatalog(catalog)) return CatalogStatus::Invalid;
  const auto found = std::find_if(catalog.images.begin(), catalog.images.end(),
                                  [id](const ImageEntry& entry) { return entry.id == id; });
  if (found == catalog.images.end()) return CatalogStatus::NotFound;
  const Catalog previous = catalog;
  const ImageEntry removed = *found;
  catalog.images.erase(found);
  const CatalogStatus saved = saveCatalog(catalog);
  if (saved != CatalogStatus::Ok) return rollbackCatalog(catalog, previous, saved);
  if (isManagedEntry(removed)) {
    // Before deleting the managed bytes, rotate the already-committed catalog
    // once more so both recovery candidates omit this image. If the checkpoint
    // fails, keep the unreferenced file: the old backup can still recover it.
    if (saveCatalog(catalog, true) != CatalogStatus::Ok) return CatalogStatus::Ok;
    removeIfPresent(removed.path.c_str());
    removeIfPresent((removed.path + ".bak").c_str());
    removeIfPresent((removed.path + ".tmp").c_str());
  }
  return CatalogStatus::Ok;
}

const ImageEntry* findImage(const Catalog& catalog, const uint16_t id) {
  const auto found = std::find_if(catalog.images.begin(), catalog.images.end(),
                                  [id](const ImageEntry& entry) { return entry.id == id; });
  return found == catalog.images.end() ? nullptr : &*found;
}

ImageEntry* findImage(Catalog& catalog, const uint16_t id) {
  const auto found = std::find_if(catalog.images.begin(), catalog.images.end(),
                                  [id](const ImageEntry& entry) { return entry.id == id; });
  return found == catalog.images.end() ? nullptr : &*found;
}

}  // namespace SleepImageSelectionStore
