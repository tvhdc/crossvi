#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace SleepImageSelectionStore {

constexpr char NORMAL_BMP_PATH[] = "/sleep.bmp";
constexpr char OVERLAY_BMP_PATH[] = "/sleep-overlay.bmp";
constexpr char OVERLAY_PNG_PATH[] = "/sleep-overlay.png";
constexpr size_t MAX_IMAGES = 16;

enum class Target : uint8_t { NormalBmp = 1, OverlayBmp = 2, OverlayPng = 3 };

struct ImageTransform {
  uint8_t zoom = 100;
  int16_t offsetX = 0;
  int16_t offsetY = 0;
};

struct ImageEntry {
  uint16_t id = 0;
  std::string path;
  std::string name;
  ImageTransform transform;
};

struct Catalog {
  std::vector<ImageEntry> images;
  uint16_t nextId = 0x8000;
  bool loaded = false;
};

enum class CatalogStatus : uint8_t { Ok, Full, NotFound, Invalid, IoError };

// Load the selected-image catalog. When no catalog exists yet, the effective
// legacy root/folder selection is imported once with legacyTransform.
CatalogStatus loadCatalog(Catalog& catalog, ImageTransform legacyTransform = {});

// Publish a normalized overlay staging file into managed storage, then append
// it to the catalog. The passed catalog and filesystem are rolled back when
// the manifest cannot be published.
CatalogStatus addPreparedImage(Catalog& catalog, Target target, const char* stagingPath, const std::string& displayName,
                               ImageEntry* added = nullptr);

CatalogStatus updateTransform(Catalog& catalog, uint16_t id, ImageTransform transform);
CatalogStatus removeImage(Catalog& catalog, uint16_t id);
const ImageEntry* findImage(const Catalog& catalog, uint16_t id);
ImageEntry* findImage(Catalog& catalog, uint16_t id);

// Reconcile an interrupted picker publication before either Settings or Sleep
// decides which canonical image is active.
bool recover();

// stagingPath must already be fully written and synced. On success the target
// is validated, published, and any conflicting overlay format is removed.
bool publish(Target target, const char* stagingPath);

}  // namespace SleepImageSelectionStore
