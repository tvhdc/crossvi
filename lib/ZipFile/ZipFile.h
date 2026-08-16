#pragma once
#include <HalStorage.h>

#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

class ZipStreamReadJob;
class ZipSourceIdentityJob;

class ZipFile {
 public:
  struct FileStatSlim {
    uint16_t method;             // Compression method
    uint16_t flags;              // General-purpose bit flags
    uint32_t compressedSize;     // Compressed size
    uint32_t uncompressedSize;   // Uncompressed size
    uint32_t localHeaderOffset;  // Offset of local file header
  };

  enum class StoredEntryOpenStatus : uint8_t { Opened, Missing, NotStored, Invalid, IoError };

  struct ZipDetails {
    uint32_t centralDirOffset;
    uint32_t centralDirSize;
    uint16_t totalEntries;
    bool isSet;
  };

  // Cheap, content-sensitive identity for a ZIP archive. The central directory
  // includes every entry's path, sizes, local-header offset, and CRC32, so this
  // detects normal EPUB replacements without reading the compressed payload.
  struct SourceIdentity {
    // Raw (non-ZIP) sources reuse the durable source-identity envelope. The
    // impossible ZIP tuple {offset=UINT32_MAX, entries=0} distinguishes them
    // without changing the v1 on-disk payload used by EPUBs and replacement
    // barriers. For raw files centralDirSize carries CRC32 and centralDirHash
    // carries the streaming FNV-1a hash.
    static constexpr uint32_t RAW_FILE_OFFSET_SENTINEL = UINT32_MAX;

    uint64_t fileSize = 0;
    uint32_t centralDirOffset = 0;
    uint32_t centralDirSize = 0;
    uint16_t totalEntries = 0;
    uint64_t centralDirHash = 0;

    static SourceIdentity forRawFile(const uint64_t size, const uint32_t crc32, const uint64_t fnv64) {
      return {size, RAW_FILE_OFFSET_SENTINEL, crc32, 0, fnv64};
    }

    bool isRawFile() const { return centralDirOffset == RAW_FILE_OFFSET_SENTINEL && totalEntries == 0; }
    uint32_t rawFileCrc32() const { return centralDirSize; }
    uint64_t rawFileFnv64() const { return centralDirHash; }

    bool operator==(const SourceIdentity& other) const {
      return fileSize == other.fileSize && centralDirOffset == other.centralDirOffset &&
             centralDirSize == other.centralDirSize && totalEntries == other.totalEntries &&
             centralDirHash == other.centralDirHash;
    }
    bool operator!=(const SourceIdentity& other) const { return !(*this == other); }
  };

  // Target for batch uncompressed size lookup (sorted by hash, then len)
  struct SizeTarget {
    uint64_t hash;   // FNV-1a 64-bit hash of normalized path
    uint16_t len;    // Length of path for collision reduction
    uint16_t index;  // Caller's index (e.g. spine index)
  };

  // FNV-1a 64-bit hash computed from char buffer (no std::string allocation)
  static uint64_t fnvHash64(const char* s, size_t len) {
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < len; i++) {
      hash ^= static_cast<uint8_t>(s[i]);
      hash *= 1099511628211ull;
    }
    return hash;
  }

 private:
  friend class ZipStreamReadJob;
  friend class ZipSourceIdentityJob;

  const std::string& filePath;
  HalFile file;
  ZipDetails zipDetails = {0, 0, 0, false};
  std::unordered_map<std::string, FileStatSlim> fileStatSlimCache;

  // Cursor for sequential central-dir scanning optimization
  uint32_t lastCentralDirPos = 0;
  bool lastCentralDirPosValid = false;

  bool loadFileStatSlim(const char* filename, FileStatSlim* fileStat);
  long getDataOffset(const FileStatSlim& fileStat);
  bool loadZipDetails();
  StoredEntryOpenStatus openValidatedEntry(const char* filename, HalFile& archive, uint64_t& dataOffset,
                                           uint32_t& compressedSize, uint32_t& uncompressedSize, uint16_t& method);

 public:
  explicit ZipFile(const std::string& filePath) : filePath(filePath) {}
  ~ZipFile() = default;
  // Zip file can be opened and closed by hand in order to allow for quick calculation of inflated file size
  // It is NOT recommended to pre-open it for any kind of inflation due to memory constraints
  bool isOpen() const { return !!file; }
  bool open();
  bool close();
  bool loadAllFileStatSlims();
  bool getSourceIdentity(SourceIdentity& identity);
  // Read the central-directory metadata for one entry without inflating it.
  // Callers use this for small, intentionally stored assets.
  bool getFileStat(const char* filename, FileStatSlim* fileStat);
  // Open a bounded, uncompressed ZIP entry without materializing it. The
  // returned handle still refers to the complete archive; callers must treat
  // dataOffset/dataSize as the only readable range.
  StoredEntryOpenStatus openStoredEntry(const char* filename, HalFile& archive, uint64_t& dataOffset,
                                        uint32_t& dataSize);
  bool getInflatedFileSize(const char* filename, size_t* size);
  // Batch lookup: scan ZIP central dir once and fill sizes for matching targets.
  // targets must be sorted by (hash, len). sizes[target.index] receives uncompressedSize.
  // Returns number of targets matched.
  int fillUncompressedSizes(std::deque<SizeTarget>& targets, std::deque<uint32_t>& sizes);
  // Due to the memory required to run each of these, it is recommended to not preopen the zip file for multiple
  // These functions will open and close the zip as needed
  uint8_t* readFileToMemory(const char* filename, size_t* size = nullptr, bool trailingNullByte = false);
  bool readFileToStream(const char* filename, Print& out, size_t chunkSize, bool allowEarlyStop = false,
                        size_t maxOutputSize = SIZE_MAX, bool* outputLimitExceeded = nullptr);

  template <typename F>
  bool enumerateFilePaths(F&& callback) {
    if (!fileStatSlimCache.empty()) {
      for (const auto& entry : fileStatSlimCache) {
        callback(std::string_view{entry.first});
      }
      return true;
    }

    const bool wasOpen = isOpen();
    if (!wasOpen && !open()) {
      return false;
    }

    if (!loadZipDetails()) {
      if (!wasOpen) {
        close();
      }
      return false;
    }

    file.seek(zipDetails.centralDirOffset);

    uint32_t sig;
    char itemName[256];

    while (file.available()) {
      file.read(&sig, 4);
      if (sig != 0x02014b50) {
        break;
      }

      file.seekCur(24);
      uint16_t nameLen, m, k;
      file.read(&nameLen, 2);
      file.read(&m, 2);
      file.read(&k, 2);
      file.seekCur(12);

      if (nameLen < sizeof(itemName)) {
        file.read(itemName, nameLen);
        itemName[nameLen] = '\0';
        callback(std::string_view{itemName, nameLen});
      } else {
        file.seekCur(nameLen);
      }

      file.seekCur(m + k);
    }

    if (!wasOpen) {
      close();
    }
    return true;
  }
};

class ZipSourceIdentityJob {
 public:
  enum class StepStatus : uint8_t { InProgress, Done, Error };
  struct FileStamp {
    uint16_t modifyDate = 0;
    uint16_t modifyTime = 0;
    bool valid = false;
  };

  ZipSourceIdentityJob();
  ~ZipSourceIdentityJob();
  ZipSourceIdentityJob(const ZipSourceIdentityJob&) = delete;
  ZipSourceIdentityJob& operator=(const ZipSourceIdentityJob&) = delete;

  bool begin(const std::string& zipPath);
  StepStatus step(size_t maxBytes, ZipFile::SourceIdentity& identity, FileStamp* fileStamp = nullptr);
  void cancel();

 private:
  class Impl;
  std::unique_ptr<Impl> impl;
};

class ZipStreamReadJob {
 public:
  enum class BeginStatus : uint8_t { Started, NotApplicable, Error };
  enum class StepStatus : uint8_t { InProgress, Done, Error };

  ZipStreamReadJob();
  ~ZipStreamReadJob();
  ZipStreamReadJob(const ZipStreamReadJob&) = delete;
  ZipStreamReadJob& operator=(const ZipStreamReadJob&) = delete;

  BeginStatus begin(const std::string& zipPath, const char* entry, Print& out, size_t chunkSize, size_t maxOutputSize,
                    bool allowStored = false);
  StepStatus step();
  void cancel();

 private:
  class Impl;
  std::unique_ptr<Impl> impl;
};
