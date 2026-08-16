#pragma once

#include <HalStorage.h>
#include <RawSourceIdentity.h>
#include <ZipFile.h>

#include <memory>
#include <string>

class Txt {
  std::string filepath;
  std::string cacheBasePath;
  std::string cachePath;
  bool loaded = false;
  size_t fileSize = 0;
  ZipFile::SourceIdentity sourceIdentity{};
  RawSourceIdentityHandoff sourceIdentityHandoff{};
  HalFile loadFile;
  uint64_t loadExpectedSize = 0;
  uint64_t loadBytesRead = 0;
  RawSourceIdentityAccumulator loadFingerprint;
  bool loadInProgress = false;
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  uint32_t loadStartedMs = 0;
  uint32_t loadStartFreeHeap = 0;
#endif

 public:
  enum class LoadStepResult : uint8_t { InProgress, Loaded, Error };

  explicit Txt(std::string path, std::string cacheBasePath);
  ~Txt() { cancelLoad(); }

  bool load();
  bool beginLoad(const RawSourceIdentityHandoff* preparedIdentity = nullptr);
  LoadStepResult stepLoad(size_t maxBytes);
  void cancelLoad();
  [[nodiscard]] bool isLoadInProgress() const { return loadInProgress; }
  [[nodiscard]] bool isLoaded() const { return loaded; }
  [[nodiscard]] const std::string& getPath() const { return filepath; }
  [[nodiscard]] const std::string& getCachePath() const { return cachePath; }
  [[nodiscard]] std::string getTitle() const;
  [[nodiscard]] size_t getFileSize() const { return fileSize; }
  [[nodiscard]] bool getSourceIdentity(ZipFile::SourceIdentity& identity) const {
    if (!loaded || !sourceIdentity.isRawFile()) return false;
    identity = sourceIdentity;
    return true;
  }
  [[nodiscard]] bool getSourceIdentityHandoff(RawSourceIdentityHandoff& handoff) const {
    if (!loaded || !sourceIdentityHandoff.valid || sourceIdentityHandoff.identity != sourceIdentity) return false;
    handoff = sourceIdentityHandoff;
    return true;
  }

  bool setupCacheDir() const;
  bool clearCache() const;

  // Cover image support - looks for cover.bmp/jpg/jpeg/png in same folder as txt file
  [[nodiscard]] std::string getCoverBmpPath() const;
  [[nodiscard]] bool generateCoverBmp() const;
  [[nodiscard]] std::string findCoverImage() const;

  // Read content from file
  [[nodiscard]] bool readContent(uint8_t* buffer, size_t offset, size_t length) const;
  [[nodiscard]] bool readContent(HalFile& file, uint8_t* buffer, size_t offset, size_t length) const;
};
