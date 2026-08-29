#pragma once
#include <HalStorage.h>

#include <deque>
#include <memory>
#include <string>
#include <string_view>

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

  // Cursor for sequential central-dir scanning optimization
  uint32_t lastCentralDirPos = 0;
  bool lastCentralDirPosValid = false;

  bool loadFileStatSlim(const char* filename, FileStatSlim* fileStat);
  long getDataOffset(const FileStatSlim& fileStat);
  bool loadZipDetails();
  StoredEntryOpenStatus openValidatedFileStat(const char* filename, const FileStatSlim& fileStat, HalFile& archive,
                                              uint64_t& dataOffset, uint32_t& compressedSize,
                                              uint32_t& uncompressedSize, uint16_t& method);
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
  int fillUncompressedSizes(const SizeTarget* targets, size_t targetCount, uint32_t* sizes, size_t sizeCount);
  // Due to the memory required to run each of these, it is recommended to not preopen the zip file for multiple
  // These functions will open and close the zip as needed
  uint8_t* readFileToMemory(const char* filename, size_t* size = nullptr, bool trailingNullByte = false);
  bool readFileToStream(const char* filename, Print& out, size_t chunkSize, bool allowEarlyStop = false,
                        size_t maxOutputSize = SIZE_MAX, bool* outputLimitExceeded = nullptr);

  template <typename F>
  bool enumerateFilePaths(F&& callback) {
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

    bool complete = file.seek(zipDetails.centralDirOffset);
    const uint64_t centralDirEnd = static_cast<uint64_t>(zipDetails.centralDirOffset) + zipDetails.centralDirSize;

    uint32_t sig = 0;
    char itemName[256];

    for (uint16_t entry = 0; complete && entry < zipDetails.totalEntries; ++entry) {
      const uint64_t entryStart = file.position();
      if (entryStart > centralDirEnd || centralDirEnd - entryStart < 46U || file.read(&sig, 4) != 4 ||
          sig != 0x02014b50) {
        complete = false;
        break;
      }

      uint16_t nameLen = 0;
      uint16_t extraLen = 0;
      uint16_t commentLen = 0;
      if (!file.seekCur(24) || file.read(&nameLen, 2) != 2 || file.read(&extraLen, 2) != 2 ||
          file.read(&commentLen, 2) != 2 || !file.seekCur(12)) {
        complete = false;
        break;
      }

      const uint64_t tailLength = static_cast<uint64_t>(nameLen) + extraLen + commentLen;
      const uint64_t tailStart = file.position();
      if (tailStart > centralDirEnd || tailLength > centralDirEnd - tailStart) {
        complete = false;
        break;
      }

      if (nameLen < sizeof(itemName)) {
        if (file.read(itemName, nameLen) != nameLen) {
          complete = false;
          break;
        }
        itemName[nameLen] = '\0';
        callback(std::string_view{itemName, nameLen});
      } else if (!file.seekCur(nameLen)) {
        complete = false;
        break;
      }

      if (!file.seekCur(static_cast<int64_t>(extraLen) + commentLen)) {
        complete = false;
        break;
      }
    }

    if (!wasOpen && !close()) complete = false;
    return complete;
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
  enum class BeginStatus : uint8_t { Started, NotApplicable, Error, OutOfMemory };
  enum class StepStatus : uint8_t { InProgress, Done, Error };

  ZipStreamReadJob();
  ~ZipStreamReadJob();
  ZipStreamReadJob(const ZipStreamReadJob&) = delete;
  ZipStreamReadJob& operator=(const ZipStreamReadJob&) = delete;

  BeginStatus begin(const std::string& zipPath, const char* entry, Print& out, size_t chunkSize, size_t maxOutputSize,
                    bool allowStored = false);
  // Page-image preparation uses a bounded central-directory lookup before
  // streaming. Existing callers retain begin()'s immediate NotApplicable and
  // validation semantics.
  BeginStatus beginCooperativeLookup(const std::string& zipPath, const char* entry, Print& out, size_t chunkSize,
                                     size_t maxOutputSize);
  StepStatus step();
  void cancel();

 private:
  class Impl;
  std::unique_ptr<Impl> impl;
};
