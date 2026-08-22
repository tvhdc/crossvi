#pragma once

#include <HalStorage.h>
#include <StagedFileTransaction.h>

#include <climits>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

enum class LibraryBookFormat : uint8_t { Epub, Text, Markdown, Xtc, Xtch };

struct LibraryBookRecord {
  std::string path;
  std::string title;
  std::string author;
  std::string coverBmpPath;
  LibraryBookFormat format = LibraryBookFormat::Epub;
  uint64_t sourceSize = 0;
  // Packed FAT modification time used only to invalidate stale catalog
  // metadata when a book is replaced in place.
  uint32_t sourceTimestamp = 0;
  // Packed FAT date/time. Zero means the filesystem did not expose a usable
  // timestamp; such records sort after timestamped records.
  uint32_t addedTimestamp = 0;
  // Presentation-only state. This field is never serialized into library.idx.
  bool pinned = false;
};

class Epub;

class LibraryCatalogStore final {
 public:
  static constexpr uint32_t MAX_BOOKS = 2048;
  static constexpr uint8_t MAX_DEPTH = 16;
  static constexpr size_t MAX_PATH_BYTES = 511;
  static constexpr size_t MAX_TITLE_BYTES = 255;
  static constexpr size_t MAX_AUTHOR_BYTES = 255;

  enum class Phase : uint8_t { Idle, Discovering, Enriching, Sorting, Ready, Error, Updating };
  enum class FindPathResult : uint8_t { Found, NotFound, IoError };
  enum class OrderPhase : uint8_t { Idle, Initializing, Merging, Publishing };

  static LibraryCatalogStore& getInstance();

  bool open();
  bool startRefresh();
  // Re-check persisted source paths after the SD card has been unavailable or
  // externally modified. The check runs cooperatively through step().
  void invalidateSourceValidation() { sourcePathsValidated_ = false; }
  void step();
  void cancel();
  bool loadPage(size_t start, size_t count, std::vector<LibraryBookRecord>& records) const;
  bool loadRecords(std::span<const size_t> indices, std::vector<LibraryBookRecord>& records) const;
  bool loadRecord(size_t index, LibraryBookRecord& record) const;
  // Return sorted source indices whose format is not the excluded one.
  bool loadOrderedIndices(uint8_t sortMode, LibraryBookFormat excluded, std::vector<size_t>& indices,
                          std::span<const std::string> paths = {}, std::vector<size_t>* pathIndices = nullptr);
  bool findPathIndices(const std::vector<std::string>& paths, std::vector<size_t>& indices) const;
  FindPathResult findPath(const std::string& path, size_t preferredIndex, size_t& foundIndex) const;
  bool consumeLastBuildFailed() {
    const bool failed = lastBuildFailed_;
    lastBuildFailed_ = false;
    return failed;
  }
  bool isReady() const { return phase_ == Phase::Ready; }
  bool isBuilding() const {
    return phase_ == Phase::Discovering || phase_ == Phase::Enriching || phase_ == Phase::Sorting ||
           phase_ == Phase::Updating;
  }
  bool isOrderBuilding() const { return orderPhase_ != OrderPhase::Idle; }
  OrderPhase orderPhase() const { return orderPhase_; }
  bool isTruncated() const { return truncated_; }
  Phase phase() const { return phase_; }
  uint32_t count() const { return count_; }
  uint32_t generation() const { return generation_; }

  static void markDirty() {
    Storage.ensureDirectoryExists("/.crosspoint");
    HalFile marker;
    if (Storage.openFileForWrite("LIB", "/.crosspoint/library.dirty", marker)) {
      constexpr uint8_t dirty = 1;
      marker.write(&dirty, sizeof(dirty));
      marker.sync();
      marker.close();
    }
  }

  // A single newly published book can be folded into the ready catalog
  // without rediscovering the whole SD card. Multiple or unknown changes
  // continue to use markDirty() and the safe full rebuild path.
  static constexpr const char* dirtyPathPrefix() { return "CVLIBPATH1:"; }
  static constexpr const char* deletedPathPrefix() { return "CVLIBDEL1:"; }
  static void markDirtyPath(const std::string& path) { markPathMarker(dirtyPathPrefix(), path); }

  // A successful single-file deletion can be applied without rediscovering the
  // SD card. Multiple pending mutations deliberately fall back to markDirty().
  static void markDeletedPath(const std::string& path) { markPathMarker(deletedPathPrefix(), path); }

 private:
  static void markPathMarker(const char* prefix, const std::string& path) {
    if (path.empty() || path.size() > MAX_PATH_BYTES) {
      markDirty();
      return;
    }
    const std::string marker = std::string(prefix) + path;
    if (Storage.exists("/.crosspoint/library.dirty")) {
      HalFile existingFile;
      std::string existing;
      const bool opened = Storage.openFileForRead("LIB", "/.crosspoint/library.dirty", existingFile);
      if (opened) {
        const uint64_t size = existingFile.fileSize64();
        if (size > MAX_PATH_BYTES + marker.size() || size > static_cast<uint64_t>(INT_MAX)) {
          existingFile.close();
        } else {
          existing.resize(static_cast<size_t>(size));
          const bool read = existingFile.read(existing.data(), existing.size()) == static_cast<int>(size);
          const bool closed = existingFile.close();
          if (!read || !closed) existing.clear();
        }
      }
      if (!opened || existing.empty() || existing != marker) {
        markDirty();
        return;
      }
    }
    Storage.ensureDirectoryExists("/.crosspoint");
    HalFile file;
    if (!Storage.openFileForWrite("LIB", "/.crosspoint/library.dirty", file) ||
        file.write(marker.data(), marker.size()) != marker.size() || !file.sync() || !file.close()) {
      markDirty();
    }
  }

  struct DirectoryFrame {
    std::string path;
    HalFile directory;
  };

  enum class UpdateKind : uint8_t { None, Upsert, Delete };
  enum class UpdateStage : uint8_t { Idle, Metadata, Locate, Copy, Publish, Verify };

  LibraryCatalogStore() = default;
  ~LibraryCatalogStore();

  bool beginBuild();
  bool beginSourceValidation();
  void validateOneSource();
  void resetSourceValidation();
  bool applyDirtyPath(const std::string& path);
  bool applyDeletedPath(const std::string& path);
  bool beginUpdateLocate();
  void stepUpdate();
  void resetUpdate(bool removeTemporary);
  void discoverOne();
  bool beginEnrichment();
  void enrichOne();
  void resetEnrichment();
  bool finalizeBuild();
  bool appendRecord(const LibraryBookRecord& record);
  bool readRecord(const char* path, size_t index, LibraryBookRecord& record) const;
  bool loadHeader(const char* path, bool requireReady);
  bool restorePreviousCatalog();
  bool writeWorkHeader(Phase phase);
  bool startOrderBuild(uint8_t sortMode);
  bool stepOrderBuild();
  void resetOrderBuild(bool removeTemporary);
  const char* activePath() const;
  void resetFinalize(bool removeTemporary);

  std::vector<DirectoryFrame> directories_;
  HalFile workFile_;
  uint32_t enrichmentIndex_ = 0;
  LibraryBookRecord enrichmentRecord_;
  std::unique_ptr<Epub> enrichmentEpub_;
  HalFile finalizeInput_;
  HalFile finalizeOutput_;
  HalFile finalizeVerify_;
  StagedFileTransaction::Digest finalizeDigest_;
  StagedFileTransaction::Digest finalizeActualDigest_;
  uint32_t finalizeIndex_ = 0;
  uint32_t finalizeVerifyIndex_ = 0;
  bool finalizeStarted_ = false;
  bool finalizePublishPending_ = false;
  Phase phase_ = Phase::Idle;
  uint32_t count_ = 0;
  uint32_t generation_ = 0;
  bool truncated_ = false;
  bool lastBuildFailed_ = false;

  HalFile sourceValidationFile_;
  uint32_t sourceValidationIndex_ = 0;
  bool sourcePathsValidated_ = false;

  UpdateKind updateKind_ = UpdateKind::None;
  UpdateStage updateStage_ = UpdateStage::Idle;
  std::string updatePath_;
  LibraryBookRecord updateRecord_;
  std::unique_ptr<Epub> updateEpub_;
  HalFile updateInput_;
  HalFile updateOutput_;
  uint32_t updateScanIndex_ = 0;
  uint32_t updateCopyIndex_ = 0;
  uint32_t updateExistingIndex_ = UINT32_MAX;
  uint32_t updateTargetCount_ = 0;
  StagedFileTransaction::Digest updateDigest_;
  StagedFileTransaction::Digest updateActualDigest_;
  uint32_t updateVerifyIndex_ = 0;
  bool updatePublishPending_ = false;

  OrderPhase orderPhase_ = OrderPhase::Idle;
  HalFile orderInput_;
  HalFile orderOutput_;
  HalFile orderCatalog_;
  uint32_t orderRunWidth_ = 0;
  uint32_t orderBase_ = 0;
  uint32_t orderLeft_ = 0;
  uint32_t orderLeftEnd_ = 0;
  uint32_t orderRight_ = 0;
  uint32_t orderRightEnd_ = 0;
  uint32_t orderPublished_ = 0;
  uint32_t orderEntriesCrc_ = 0;
  uint32_t orderSeedIndex_ = 0;
  uint8_t orderSortMode_ = 0;
  bool orderSourceA_ = true;
};

#define LIBRARY_CATALOG LibraryCatalogStore::getInstance()
