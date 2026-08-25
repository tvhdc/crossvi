#include "ZipFile.h"

#include <HalStorage.h>
#include <InflateStream.h>
#include <Logging.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

struct ZipInflateCtx {
  HalFile* file = nullptr;
  size_t fileRemaining = 0;
  uint8_t* readBuf = nullptr;
  size_t readBufSize = 0;
};

namespace {
constexpr uint16_t ZIP_METHOD_STORED = 0;
constexpr uint16_t ZIP_METHOD_DEFLATED = 8;
constexpr uint64_t FNV64_OFFSET_BASIS = 14695981039346656037ull;
constexpr uint64_t FNV64_PRIME = 1099511628211ull;

uint16_t readLe16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8U);
}

uint32_t readLe32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8U) |
         (static_cast<uint32_t>(data[2]) << 16U) | (static_cast<uint32_t>(data[3]) << 24U);
}

// RAII zip: opens the zip if not already open, closes on destruction only if
// it performed the open.  Removes the wasOpen/close boilerplate from every method.
class ScopedOpenClose final {
 public:
  [[nodiscard]] explicit ScopedOpenClose(ZipFile& zf) : zf(zf), needsClose(!zf.isOpen()) {
    if (needsClose) ok = zf.open();
  }
  ~ScopedOpenClose() {
    if (needsClose && ok) zf.close();
  }
  ScopedOpenClose(const ScopedOpenClose&) = delete;
  ScopedOpenClose& operator=(const ScopedOpenClose&) = delete;
  ScopedOpenClose(ScopedOpenClose&&) = delete;
  ScopedOpenClose& operator=(ScopedOpenClose&&) = delete;
  explicit operator bool() const { return ok || !needsClose; }

 private:
  ZipFile& zf;
  bool needsClose = false;
  bool ok = true;  // true when zip was already open (no open() call needed)
};

size_t zipFillCallback(void* vctx, const uint8_t** data) {
  auto* ctx = static_cast<ZipInflateCtx*>(vctx);
  if (ctx->fileRemaining == 0) return 0;

  const size_t toRead = ctx->fileRemaining < ctx->readBufSize ? ctx->fileRemaining : ctx->readBufSize;
  const size_t bytesRead = ctx->file->read(ctx->readBuf, toRead);
  ctx->fileRemaining -= bytesRead;

  *data = ctx->readBuf;
  return bytesRead;
}
}  // namespace

class ZipStreamReadJob::Impl {
 public:
  Impl(Print& output, const size_t requestedChunkSize) : out(&output), chunkSize(requestedChunkSize) {}

  ~Impl() { archive.close(); }

  ZipStreamReadJob::BeginStatus configureStream(const uint64_t fileOffset, const uint32_t compressedSize,
                                                const uint32_t uncompressedSize, const uint16_t method,
                                                const size_t maxOutputSize, const bool allowStored) {
    if (method == ZIP_METHOD_STORED && !allowStored) return ZipStreamReadJob::BeginStatus::NotApplicable;
    if (method == ZIP_METHOD_STORED && compressedSize != uncompressedSize) {
      return ZipStreamReadJob::BeginStatus::Error;
    }
    if ((method != ZIP_METHOD_STORED && method != ZIP_METHOD_DEFLATED) || uncompressedSize > maxOutputSize) {
      LOG_ERR("ZIP", "Cooperative entry is unsupported or too large (%u bytes, limit %zu)", uncompressedSize,
              maxOutputSize);
      return ZipStreamReadJob::BeginStatus::Error;
    }
    if (!archive.seek64(fileOffset)) return ZipStreamReadJob::BeginStatus::Error;

    outputBuffer.reset(new (std::nothrow) uint8_t[chunkSize]);
    if (!outputBuffer) {
      LOG_ERR("ZIP", "Failed to allocate cooperative output buffer (%zu bytes)", chunkSize);
      return ZipStreamReadJob::BeginStatus::Error;
    }

    expectedSize = uncompressedSize;
    if (method == ZIP_METHOD_STORED) {
      stored = true;
      return ZipStreamReadJob::BeginStatus::Started;
    }

    inputBuffer.reset(new (std::nothrow) uint8_t[chunkSize]);
    if (!inputBuffer) {
      LOG_ERR("ZIP", "Failed to allocate cooperative input buffer (%zu bytes)", chunkSize);
      return ZipStreamReadJob::BeginStatus::Error;
    }

    ctx.file = &archive;
    ctx.fileRemaining = compressedSize;
    ctx.readBuf = inputBuffer.get();
    ctx.readBufSize = chunkSize;
    if (!inflate.init(true)) {
      LOG_ERR("ZIP", "Failed to init cooperative inflate stream");
      return ZipStreamReadJob::BeginStatus::Error;
    }
    inflate.setFill(zipFillCallback, &ctx);
    return ZipStreamReadJob::BeginStatus::Started;
  }

  HalFile archive;
  Print* out = nullptr;
  size_t chunkSize = 0;
  size_t expectedSize = 0;
  size_t totalProduced = 0;
  bool stored = false;
  std::unique_ptr<uint8_t[]> inputBuffer;
  std::unique_ptr<uint8_t[]> outputBuffer;
  ZipInflateCtx ctx;
  InflateStream inflate;
  std::string lookupPath;
  std::string lookupEntry;
  std::unique_ptr<ZipFile> lookupZip;
  uint32_t lookupCentralDirEnd = 0;
  uint16_t lookupEntriesRemaining = 0;
  size_t lookupMaxOutputSize = 0;
  bool lookupPending = false;
};

class ZipSourceIdentityJob::Impl {
 public:
  explicit Impl(std::string sourcePath) : path(std::move(sourcePath)), zip(path) {}
  ~Impl() { zip.close(); }

  std::string path;
  ZipFile zip;
  std::array<uint8_t, 512> buffer;
  uint64_t expectedFileSize = 0;
  uint64_t hash = FNV64_OFFSET_BASIS;
  uint32_t remaining = 0;
};

ZipSourceIdentityJob::ZipSourceIdentityJob() = default;
ZipSourceIdentityJob::~ZipSourceIdentityJob() = default;

bool ZipSourceIdentityJob::begin(const std::string& zipPath) {
  cancel();
  if (zipPath.empty()) return false;

  auto next = std::unique_ptr<Impl>(new (std::nothrow) Impl(zipPath));
  if (!next || !next->zip.open() || !next->zip.loadZipDetails()) return false;

  next->expectedFileSize = next->zip.file.fileSize64();
  const uint64_t centralDirEnd =
      static_cast<uint64_t>(next->zip.zipDetails.centralDirOffset) + next->zip.zipDetails.centralDirSize;
  if (next->expectedFileSize == 0 || centralDirEnd > next->expectedFileSize ||
      !next->zip.file.seek(next->zip.zipDetails.centralDirOffset)) {
    return false;
  }
  next->remaining = next->zip.zipDetails.centralDirSize;
  impl = std::move(next);
  return true;
}

ZipSourceIdentityJob::StepStatus ZipSourceIdentityJob::step(const size_t maxBytes, ZipFile::SourceIdentity& identity,
                                                            FileStamp* const fileStamp) {
  identity = {};
  if (fileStamp) *fileStamp = {};
  if (!impl || maxBytes == 0) return StepStatus::Error;

  size_t budget = std::min<size_t>(maxBytes, impl->remaining);
  while (budget > 0) {
    const size_t chunk = std::min({impl->buffer.size(), budget, static_cast<size_t>(impl->remaining)});
    if (impl->zip.file.read(impl->buffer.data(), chunk) != static_cast<int>(chunk)) {
      cancel();
      return StepStatus::Error;
    }
    for (size_t i = 0; i < chunk; ++i) {
      impl->hash ^= impl->buffer[i];
      impl->hash *= FNV64_PRIME;
    }
    impl->remaining -= static_cast<uint32_t>(chunk);
    budget -= chunk;
  }

  if (impl->remaining > 0) return StepStatus::InProgress;
  if (impl->zip.file.fileSize64() != impl->expectedFileSize) {
    cancel();
    return StepStatus::Error;
  }

  identity.fileSize = impl->expectedFileSize;
  identity.centralDirOffset = impl->zip.zipDetails.centralDirOffset;
  identity.centralDirSize = impl->zip.zipDetails.centralDirSize;
  identity.totalEntries = impl->zip.zipDetails.totalEntries;
  identity.centralDirHash = impl->hash;
  if (fileStamp) {
    fileStamp->valid = impl->zip.file.getModifyDateTime(&fileStamp->modifyDate, &fileStamp->modifyTime);
  }
  cancel();
  return StepStatus::Done;
}

void ZipSourceIdentityJob::cancel() { impl.reset(); }

ZipStreamReadJob::ZipStreamReadJob() = default;
ZipStreamReadJob::~ZipStreamReadJob() = default;

ZipStreamReadJob::BeginStatus ZipStreamReadJob::begin(const std::string& zipPath, const char* entry, Print& out,
                                                      const size_t chunkSize, const size_t maxOutputSize,
                                                      const bool allowStored) {
  cancel();
  if (zipPath.empty() || !entry || entry[0] == '\0' || chunkSize == 0 || maxOutputSize == 0) {
    return BeginStatus::Error;
  }

  auto next = std::unique_ptr<Impl>(new (std::nothrow) Impl(out, chunkSize));
  if (!next) return BeginStatus::Error;

  uint64_t fileOffset = 0;
  uint32_t compressedSize = 0;
  uint32_t uncompressedSize = 0;
  uint16_t method = 0;
  ZipFile zip(zipPath);
  const ZipFile::StoredEntryOpenStatus opened =
      zip.openValidatedEntry(entry, next->archive, fileOffset, compressedSize, uncompressedSize, method);
  if (opened != ZipFile::StoredEntryOpenStatus::Opened) return BeginStatus::Error;
  const BeginStatus configured =
      next->configureStream(fileOffset, compressedSize, uncompressedSize, method, maxOutputSize, allowStored);
  if (configured != BeginStatus::Started) return configured;
  impl = std::move(next);
  return BeginStatus::Started;
}

ZipStreamReadJob::BeginStatus ZipStreamReadJob::beginCooperativeLookup(const std::string& zipPath, const char* entry,
                                                                       Print& out, const size_t chunkSize,
                                                                       const size_t maxOutputSize) {
  cancel();
  if (zipPath.empty() || !entry || entry[0] == '\0' || strlen(entry) >= 256 || chunkSize == 0 || maxOutputSize == 0) {
    return BeginStatus::Error;
  }

  auto next = std::unique_ptr<Impl>(new (std::nothrow) Impl(out, chunkSize));
  if (!next) return BeginStatus::Error;
  next->lookupPath = zipPath;
  next->lookupEntry = entry;
  next->lookupZip = std::unique_ptr<ZipFile>(new (std::nothrow) ZipFile(next->lookupPath));
  if (!next->lookupZip || !next->lookupZip->open() || !next->lookupZip->loadZipDetails()) {
    return BeginStatus::Error;
  }
  const uint64_t centralDirEnd =
      static_cast<uint64_t>(next->lookupZip->zipDetails.centralDirOffset) + next->lookupZip->zipDetails.centralDirSize;
  if (centralDirEnd > UINT32_MAX || !next->lookupZip->file.seek(next->lookupZip->zipDetails.centralDirOffset)) {
    return BeginStatus::Error;
  }
  next->lookupCentralDirEnd = static_cast<uint32_t>(centralDirEnd);
  next->lookupEntriesRemaining = next->lookupZip->zipDetails.totalEntries;
  next->lookupMaxOutputSize = maxOutputSize;
  next->lookupPending = true;
  impl = std::move(next);
  return BeginStatus::Started;
}

ZipStreamReadJob::StepStatus ZipStreamReadJob::step() {
  if (!impl) return StepStatus::Error;
  if (impl->lookupPending) {
    constexpr size_t LOOKUP_ENTRIES_PER_STEP = 8;
    constexpr size_t CENTRAL_HEADER_SIZE = 46;
    std::array<uint8_t, CENTRAL_HEADER_SIZE> centralHeader{};
    std::array<char, 256> entryName{};

    for (size_t scanned = 0; scanned < LOOKUP_ENTRIES_PER_STEP; ++scanned) {
      if (!impl->lookupZip || impl->lookupEntriesRemaining == 0) {
        LOG_ERR("ZIP", "Cooperative ZIP entry was not found: %s", impl->lookupEntry.c_str());
        cancel();
        return StepStatus::Error;
      }
      HalFile& centralDirectory = impl->lookupZip->file;
      const uint64_t entryStart = centralDirectory.position();
      if (entryStart > impl->lookupCentralDirEnd || CENTRAL_HEADER_SIZE > impl->lookupCentralDirEnd - entryStart ||
          centralDirectory.read(centralHeader.data(), centralHeader.size()) != static_cast<int>(centralHeader.size()) ||
          readLe32(centralHeader.data()) != 0x02014b50) {
        LOG_ERR("ZIP", "Invalid cooperative central-directory record");
        cancel();
        return StepStatus::Error;
      }

      ZipFile::FileStatSlim fileStat{};
      fileStat.flags = readLe16(centralHeader.data() + 8);
      fileStat.method = readLe16(centralHeader.data() + 10);
      fileStat.compressedSize = readLe32(centralHeader.data() + 20);
      fileStat.uncompressedSize = readLe32(centralHeader.data() + 24);
      fileStat.localHeaderOffset = readLe32(centralHeader.data() + 42);
      const uint16_t nameLength = readLe16(centralHeader.data() + 28);
      const uint16_t extraLength = readLe16(centralHeader.data() + 30);
      const uint16_t commentLength = readLe16(centralHeader.data() + 32);
      const uint64_t recordEnd = entryStart + CENTRAL_HEADER_SIZE + nameLength + extraLength + commentLength;
      if (recordEnd < entryStart || recordEnd > impl->lookupCentralDirEnd) {
        LOG_ERR("ZIP", "Cooperative central-directory record exceeds declared bounds");
        cancel();
        return StepStatus::Error;
      }

      bool found = false;
      if (nameLength == impl->lookupEntry.size()) {
        if (centralDirectory.read(entryName.data(), nameLength) != nameLength) {
          cancel();
          return StepStatus::Error;
        }
        found = memcmp(entryName.data(), impl->lookupEntry.data(), nameLength) == 0;
      } else if (!centralDirectory.seekCur(nameLength)) {
        cancel();
        return StepStatus::Error;
      }
      if (!centralDirectory.seekCur(static_cast<uint32_t>(extraLength) + commentLength)) {
        cancel();
        return StepStatus::Error;
      }
      --impl->lookupEntriesRemaining;
      if (!found) continue;

      uint64_t fileOffset = 0;
      uint32_t compressedSize = 0;
      uint32_t uncompressedSize = 0;
      uint16_t method = 0;
      const ZipFile::StoredEntryOpenStatus opened = impl->lookupZip->openValidatedFileStat(
          impl->lookupEntry.c_str(), fileStat, impl->archive, fileOffset, compressedSize, uncompressedSize, method);
      if (opened != ZipFile::StoredEntryOpenStatus::Opened) {
        cancel();
        return StepStatus::Error;
      }
      const BeginStatus configured =
          impl->configureStream(fileOffset, compressedSize, uncompressedSize, method, impl->lookupMaxOutputSize, true);
      impl->lookupZip.reset();
      impl->lookupPending = false;
      if (configured != BeginStatus::Started) {
        cancel();
        return StepStatus::Error;
      }
      // Keep lookup and payload production on separate main-loop ticks.
      return StepStatus::InProgress;
    }
    return StepStatus::InProgress;
  }
  if (impl->stored && impl->expectedSize == 0) {
    cancel();
    return StepStatus::Done;
  }

  if (impl->stored) {
    const size_t remaining = impl->expectedSize - impl->totalProduced;
    const size_t toRead = std::min(impl->chunkSize, remaining);
    if (toRead == 0 || impl->archive.read(impl->outputBuffer.get(), toRead) != static_cast<int>(toRead) ||
        impl->out->write(impl->outputBuffer.get(), toRead) != toRead) {
      LOG_ERR("ZIP", "Cooperative stored-entry read failed");
      cancel();
      return StepStatus::Error;
    }
    impl->totalProduced += toRead;
    if (impl->totalProduced == impl->expectedSize) {
      cancel();
      return StepStatus::Done;
    }
    return StepStatus::InProgress;
  }

  size_t produced = 0;
  const InflateStream::Status status = impl->inflate.readAtMost(impl->outputBuffer.get(), impl->chunkSize, &produced);
  if (produced > impl->expectedSize - impl->totalProduced) {
    LOG_ERR("ZIP", "Cooperative decompressed size exceeds expected (%zu + %zu > %zu)", impl->totalProduced, produced,
            impl->expectedSize);
    cancel();
    return StepStatus::Error;
  }

  if (produced > 0 && impl->out->write(impl->outputBuffer.get(), produced) != produced) {
    LOG_ERR("ZIP", "Failed to write all cooperative output bytes to stream");
    cancel();
    return StepStatus::Error;
  }
  impl->totalProduced += produced;

  if (status == InflateStream::Status::Done) {
    if (impl->totalProduced != impl->expectedSize) {
      LOG_ERR("ZIP", "Cooperative decompressed size mismatch (expected %zu, got %zu)", impl->expectedSize,
              impl->totalProduced);
      cancel();
      return StepStatus::Error;
    }
    cancel();
    return StepStatus::Done;
  }

  if (status == InflateStream::Status::Error || produced == 0) {
    LOG_ERR("ZIP", "Cooperative decompression failed");
    cancel();
    return StepStatus::Error;
  }
  return StepStatus::InProgress;
}

void ZipStreamReadJob::cancel() { impl.reset(); }

bool ZipFile::loadAllFileStatSlims() {
  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  if (!loadZipDetails()) return false;

  file.seek(zipDetails.centralDirOffset);

  uint32_t sig;
  char itemName[256];
  fileStatSlimCache.clear();
  fileStatSlimCache.reserve(zipDetails.totalEntries);

  while (file.available()) {
    file.read(&sig, 4);
    if (sig != 0x02014b50) break;  // End of list

    FileStatSlim fileStat = {};

    file.seekCur(4);
    file.read(&fileStat.flags, 2);
    file.read(&fileStat.method, 2);
    file.seekCur(8);
    file.read(&fileStat.compressedSize, 4);
    file.read(&fileStat.uncompressedSize, 4);
    uint16_t nameLen, m, k;
    file.read(&nameLen, 2);
    file.read(&m, 2);
    file.read(&k, 2);
    file.seekCur(8);
    file.read(&fileStat.localHeaderOffset, 4);

    if (nameLen < sizeof(itemName)) {
      file.read(itemName, nameLen);
      itemName[nameLen] = '\0';
      fileStatSlimCache.emplace(itemName, fileStat);
    } else {
      // Skip over oversized entry names to avoid writing past fixed buffer.
      file.seekCur(nameLen);
    }

    // Skip the rest of this entry (extra field + comment)
    file.seekCur(m + k);
  }

  // Set cursor to start of central directory for sequential access
  lastCentralDirPos = zipDetails.centralDirOffset;
  lastCentralDirPosValid = true;

  return true;
}

bool ZipFile::loadFileStatSlim(const char* filename, FileStatSlim* fileStat) {
  if (!fileStatSlimCache.empty()) {
    const auto it = fileStatSlimCache.find(filename);
    if (it != fileStatSlimCache.end()) {
      *fileStat = it->second;
      return true;
    }
    return false;
  }

  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  if (!loadZipDetails()) return false;

  // Phase 1: Try scanning from cursor position first
  uint32_t startPos = lastCentralDirPosValid ? lastCentralDirPos : zipDetails.centralDirOffset;
  bool wrapped = false;
  bool found = false;

  file.seek(startPos);

  uint32_t sig;
  char itemName[256];

  while (true) {
    uint32_t entryStart = file.position();

    if (file.read(&sig, 4) != 4 || sig != 0x02014b50) {
      // End of central directory
      if (!wrapped && lastCentralDirPosValid && startPos != zipDetails.centralDirOffset) {
        // Wrap around to beginning
        file.seek(zipDetails.centralDirOffset);
        wrapped = true;
        continue;
      }
      break;
    }

    // If we've wrapped and reached our start position, stop
    if (wrapped && entryStart >= startPos) {
      break;
    }

    file.seekCur(4);
    file.read(&fileStat->flags, 2);
    file.read(&fileStat->method, 2);
    file.seekCur(8);
    file.read(&fileStat->compressedSize, 4);
    file.read(&fileStat->uncompressedSize, 4);
    uint16_t nameLen, m, k;
    file.read(&nameLen, 2);
    file.read(&m, 2);
    file.read(&k, 2);
    file.seekCur(8);
    file.read(&fileStat->localHeaderOffset, 4);

    if (nameLen < 256) {
      file.read(itemName, nameLen);
      itemName[nameLen] = '\0';

      if (strcmp(itemName, filename) == 0) {
        // Found it! Update cursor to next entry
        file.seekCur(m + k);
        lastCentralDirPos = file.position();
        lastCentralDirPosValid = true;
        found = true;
        break;
      }
    } else {
      // Name too long, skip it
      file.seekCur(nameLen);
    }

    // Skip extra field + comment
    file.seekCur(m + k);
  }

  return found;
}

long ZipFile::getDataOffset(const FileStatSlim& fileStat) {
  const ScopedOpenClose zip{*this};
  if (!zip) return -1;

  constexpr auto localHeaderSize = 30;

  uint8_t pLocalHeader[localHeaderSize];
  const uint64_t fileOffset = fileStat.localHeaderOffset;

  file.seek(fileOffset);
  const size_t read = file.read(pLocalHeader, localHeaderSize);

  if (read != localHeaderSize) {
    LOG_ERR("ZIP", "Something went wrong reading the local header");
    return -1;
  }

  if (pLocalHeader[0] + (pLocalHeader[1] << 8) + (pLocalHeader[2] << 16) + (pLocalHeader[3] << 24) !=
      0x04034b50 /* ZIP local file header signature */) {
    LOG_ERR("ZIP", "Not a valid zip file header");
    return -1;
  }

  const uint16_t filenameLength = pLocalHeader[26] + (pLocalHeader[27] << 8);
  const uint16_t extraOffset = pLocalHeader[28] + (pLocalHeader[29] << 8);
  return fileOffset + localHeaderSize + filenameLength + extraOffset;
}

bool ZipFile::loadZipDetails() {
  if (zipDetails.isSet) {
    return true;
  }

  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  const size_t fileSize = file.size();
  if (fileSize < 22) {
    LOG_ERR("ZIP", "File too small to be a valid zip");
    return false;  // Minimum EOCD size is 22 bytes
  }

  // We scan the last 1KB (or the whole file if smaller) for the EOCD signature
  // 0x06054b50 is stored as 0x50, 0x4b, 0x05, 0x06 in little-endian
  const int scanRange = fileSize > 1024 ? 1024 : fileSize;
  const auto buffer = static_cast<uint8_t*>(malloc(scanRange));
  if (!buffer) {
    LOG_ERR("ZIP", "Failed to allocate memory for EOCD scan buffer");
    return false;
  }

  file.seek(fileSize - scanRange);
  if (file.read(buffer, scanRange) != scanRange) {
    free(buffer);
    return false;
  }

  // Scan backwards for the signature
  int foundOffset = -1;
  for (int i = scanRange - 22; i >= 0; i--) {
    constexpr uint32_t signature = 0x06054b50;
    if (readLe32(&buffer[i]) == signature) {
      foundOffset = i;
      break;
    }
  }

  if (foundOffset == -1) {
    LOG_ERR("ZIP", "EOCD signature not found in zip file");
    free(buffer);
    return false;
  }

  // Now extract the values we need from the EOCD record
  // Relative positions within EOCD:
  // Offset 10: Total number of entries (2 bytes)
  // Offset 16: Offset of start of central directory with respect to the starting disk number (4 bytes)
  zipDetails.totalEntries = readLe16(&buffer[foundOffset + 10]);
  zipDetails.centralDirSize = readLe32(&buffer[foundOffset + 12]);
  zipDetails.centralDirOffset = readLe32(&buffer[foundOffset + 16]);
  const uint64_t centralDirEnd = static_cast<uint64_t>(zipDetails.centralDirOffset) + zipDetails.centralDirSize;
  const uint64_t eocdOffset = static_cast<uint64_t>(fileSize - scanRange + foundOffset);
  if (zipDetails.totalEntries == 0 || zipDetails.centralDirSize == 0 || centralDirEnd > eocdOffset) {
    LOG_ERR("ZIP", "Invalid central directory bounds");
    free(buffer);
    zipDetails = {0, 0, 0, false};
    return false;
  }
  zipDetails.isSet = true;

  free(buffer);
  return true;
}

bool ZipFile::getSourceIdentity(SourceIdentity& identity) {
  ZipSourceIdentityJob job;
  if (!job.begin(filePath)) return false;
  ZipSourceIdentityJob::StepStatus status = ZipSourceIdentityJob::StepStatus::InProgress;
  while (status == ZipSourceIdentityJob::StepStatus::InProgress) status = job.step(SIZE_MAX, identity);
  return status == ZipSourceIdentityJob::StepStatus::Done;
}

bool ZipFile::open() {
  if (!Storage.openFileForRead("ZIP", filePath, file)) {
    return false;
  }
  return true;
}

bool ZipFile::close() {
  if (file) {
    // Explicit close() required: member variable persists beyond function scope
    file.close();
  }
  lastCentralDirPos = 0;
  lastCentralDirPosValid = false;
  return true;
}

bool ZipFile::getInflatedFileSize(const char* filename, size_t* size) {
  FileStatSlim fileStat = {};
  if (!loadFileStatSlim(filename, &fileStat)) {
    return false;
  }

  *size = static_cast<size_t>(fileStat.uncompressedSize);
  return true;
}

bool ZipFile::getFileStat(const char* filename, FileStatSlim* fileStat) {
  if (!filename || !fileStat) return false;
  return loadFileStatSlim(filename, fileStat);
}

ZipFile::StoredEntryOpenStatus ZipFile::openValidatedEntry(const char* filename, HalFile& archive, uint64_t& dataOffset,
                                                           uint32_t& compressedSize, uint32_t& uncompressedSize,
                                                           uint16_t& method) {
  archive = HalFile{};
  dataOffset = 0;
  compressedSize = 0;
  uncompressedSize = 0;
  method = 0;
  if (!filename || filename[0] == '\0') return StoredEntryOpenStatus::Invalid;

  const size_t requestedNameLength = strlen(filename);
  if (requestedNameLength == 0 || requestedNameLength >= 256) return StoredEntryOpenStatus::Invalid;

  const ScopedOpenClose zip{*this};
  if (!zip) return StoredEntryOpenStatus::IoError;
  if (!loadZipDetails()) return StoredEntryOpenStatus::Invalid;

  FileStatSlim fileStat{};
  if (!loadFileStatSlim(filename, &fileStat)) {
    return file.getError() == 0 ? StoredEntryOpenStatus::Missing : StoredEntryOpenStatus::IoError;
  }
  return openValidatedFileStat(filename, fileStat, archive, dataOffset, compressedSize, uncompressedSize, method);
}

ZipFile::StoredEntryOpenStatus ZipFile::openValidatedFileStat(const char* filename, const FileStatSlim& fileStat,
                                                              HalFile& archive, uint64_t& dataOffset,
                                                              uint32_t& compressedSize, uint32_t& uncompressedSize,
                                                              uint16_t& method) {
  archive = HalFile{};
  dataOffset = 0;
  compressedSize = 0;
  uncompressedSize = 0;
  method = 0;
  if (!filename || filename[0] == '\0') return StoredEntryOpenStatus::Invalid;

  const size_t requestedNameLength = strlen(filename);
  if (requestedNameLength == 0 || requestedNameLength >= 256) return StoredEntryOpenStatus::Invalid;

  const ScopedOpenClose zip{*this};
  if (!zip) return StoredEntryOpenStatus::IoError;
  if (!loadZipDetails()) return StoredEntryOpenStatus::Invalid;

  const bool emptyStored =
      fileStat.method == ZIP_METHOD_STORED && fileStat.compressedSize == 0 && fileStat.uncompressedSize == 0;
  if ((fileStat.method != ZIP_METHOD_STORED && fileStat.method != ZIP_METHOD_DEFLATED) ||
      (fileStat.compressedSize == 0 && !emptyStored) ||
      fileStat.compressedSize > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
      fileStat.localHeaderOffset == std::numeric_limits<uint32_t>::max()) {
    return StoredEntryOpenStatus::Invalid;
  }

  constexpr uint64_t localHeaderSize = 30;
  const uint64_t archiveSize = file.fileSize64();
  const uint64_t localHeaderOffset = fileStat.localHeaderOffset;
  if (localHeaderOffset > archiveSize || localHeaderSize > archiveSize - localHeaderOffset ||
      localHeaderOffset >= zipDetails.centralDirOffset) {
    return StoredEntryOpenStatus::Invalid;
  }

  std::array<uint8_t, localHeaderSize> localHeader;
  if (!file.seek64(localHeaderOffset) ||
      file.read(localHeader.data(), localHeader.size()) != static_cast<int>(localHeader.size())) {
    return file.getError() == 0 ? StoredEntryOpenStatus::Invalid : StoredEntryOpenStatus::IoError;
  }
  if (readLe32(localHeader.data()) != 0x04034b50) return StoredEntryOpenStatus::Invalid;

  const uint16_t localFlags = readLe16(localHeader.data() + 6);
  const uint16_t localMethod = readLe16(localHeader.data() + 8);
  const uint32_t localCompressedSize = readLe32(localHeader.data() + 18);
  const uint32_t localUncompressedSize = readLe32(localHeader.data() + 22);
  const uint16_t localNameLength = readLe16(localHeader.data() + 26);
  const uint16_t localExtraLength = readLe16(localHeader.data() + 28);
  constexpr uint16_t encryptedFlag = 1U << 0U;
  constexpr uint16_t dataDescriptorFlag = 1U << 3U;
  if ((localFlags & encryptedFlag) != 0 || localFlags != fileStat.flags || localMethod != fileStat.method ||
      localNameLength != requestedNameLength) {
    return StoredEntryOpenStatus::Invalid;
  }
  if ((localFlags & dataDescriptorFlag) == 0 &&
      (localCompressedSize != fileStat.compressedSize || localUncompressedSize != fileStat.uncompressedSize)) {
    return StoredEntryOpenStatus::Invalid;
  }

  std::array<char, 256> localName;
  if (file.read(localName.data(), localNameLength) != localNameLength ||
      memcmp(localName.data(), filename, requestedNameLength) != 0) {
    return file.getError() == 0 ? StoredEntryOpenStatus::Invalid : StoredEntryOpenStatus::IoError;
  }

  const uint64_t nameEnd = localHeaderOffset + localHeaderSize + localNameLength;
  if (nameEnd < localHeaderOffset || localExtraLength > std::numeric_limits<uint64_t>::max() - nameEnd) {
    return StoredEntryOpenStatus::Invalid;
  }
  const uint64_t entryDataOffset = nameEnd + localExtraLength;
  if (fileStat.compressedSize > std::numeric_limits<uint64_t>::max() - entryDataOffset) {
    return StoredEntryOpenStatus::Invalid;
  }
  const uint64_t entryDataEnd = entryDataOffset + fileStat.compressedSize;
  if (entryDataOffset > archiveSize || entryDataEnd > archiveSize || entryDataEnd > zipDetails.centralDirOffset) {
    return StoredEntryOpenStatus::Invalid;
  }

  HalFile openedArchive;
  if (!Storage.openFileForRead("ZIP", filePath, openedArchive)) return StoredEntryOpenStatus::IoError;
  if (openedArchive.fileSize64() != archiveSize || !openedArchive.seek64(entryDataOffset)) {
    openedArchive.close();
    return StoredEntryOpenStatus::IoError;
  }

  archive = std::move(openedArchive);
  dataOffset = entryDataOffset;
  compressedSize = fileStat.compressedSize;
  uncompressedSize = fileStat.uncompressedSize;
  method = fileStat.method;
  return StoredEntryOpenStatus::Opened;
}

ZipFile::StoredEntryOpenStatus ZipFile::openStoredEntry(const char* filename, HalFile& archive, uint64_t& dataOffset,
                                                        uint32_t& dataSize) {
  uint32_t compressedSize = 0;
  uint32_t uncompressedSize = 0;
  uint16_t method = 0;
  const StoredEntryOpenStatus opened =
      openValidatedEntry(filename, archive, dataOffset, compressedSize, uncompressedSize, method);
  if (opened != StoredEntryOpenStatus::Opened) return opened;
  if (method != ZIP_METHOD_STORED || compressedSize != uncompressedSize) {
    archive.close();
    dataOffset = 0;
    dataSize = 0;
    return StoredEntryOpenStatus::NotStored;
  }
  dataSize = uncompressedSize;
  return StoredEntryOpenStatus::Opened;
}

int ZipFile::fillUncompressedSizes(std::deque<SizeTarget>& targets, std::deque<uint32_t>& sizes) {
  if (targets.empty() || sizes.empty()) return 0;

  // deque storage is not contiguous, so retain the compatibility overload for
  // existing callers and use their original implementation below.

  const ScopedOpenClose zip{*this};
  if (!zip) return 0;

  if (!loadZipDetails()) return 0;

  file.seek(zipDetails.centralDirOffset);

  int matched = 0;
  const int targetCount = static_cast<int>(targets.size());
  uint32_t sig;
  char itemName[256];

  while (file.available()) {
    file.read(&sig, 4);
    if (sig != 0x02014b50) break;

    file.seekCur(6);
    uint16_t method;
    file.read(&method, 2);
    file.seekCur(8);
    uint32_t compressedSize, uncompressedSize;
    file.read(&compressedSize, 4);
    file.read(&uncompressedSize, 4);
    uint16_t nameLen, m, k;
    file.read(&nameLen, 2);
    file.read(&m, 2);
    file.read(&k, 2);
    file.seekCur(8);
    uint32_t localHeaderOffset;
    file.read(&localHeaderOffset, 4);

    if (nameLen < 256) {
      file.read(itemName, nameLen);
      itemName[nameLen] = '\0';

      uint64_t hash = fnvHash64(itemName, nameLen);
      SizeTarget key = {hash, nameLen, 0};

      auto it = std::lower_bound(targets.begin(), targets.end(), key, [](const SizeTarget& a, const SizeTarget& b) {
        return a.hash < b.hash || (a.hash == b.hash && a.len < b.len);
      });

      while (it != targets.end() && it->hash == hash && it->len == nameLen) {
        if (it->index < sizes.size()) {
          sizes[it->index] = uncompressedSize;
          matched++;
        }
        ++it;
      }

      if (matched >= targetCount) {
        break;
      }
    } else {
      file.seekCur(nameLen);
    }

    file.seekCur(m + k);
  }

  return matched;
}

int ZipFile::fillUncompressedSizes(const SizeTarget* const targets, const size_t targetCount, uint32_t* const sizes,
                                   const size_t sizeCount) {
  if (!targets || targetCount == 0 || !sizes || sizeCount == 0) return 0;

  const ScopedOpenClose zip{*this};
  if (!zip || !loadZipDetails() || !file.seek(zipDetails.centralDirOffset)) return 0;

  int matched = 0;
  uint32_t sig = 0;
  char itemName[256];
  while (file.available()) {
    if (file.read(&sig, sizeof(sig)) != sizeof(sig) || sig != 0x02014b50) break;

    file.seekCur(20);
    uint32_t uncompressedSize = 0;
    file.read(&uncompressedSize, sizeof(uncompressedSize));
    uint16_t nameLen = 0;
    uint16_t extraLen = 0;
    uint16_t commentLen = 0;
    file.read(&nameLen, sizeof(nameLen));
    file.read(&extraLen, sizeof(extraLen));
    file.read(&commentLen, sizeof(commentLen));
    file.seekCur(12);

    if (nameLen < sizeof(itemName)) {
      if (file.read(itemName, nameLen) != nameLen) break;
      const uint64_t hash = fnvHash64(itemName, nameLen);
      const SizeTarget key = {hash, nameLen, 0};
      const SizeTarget* it =
          std::lower_bound(targets, targets + targetCount, key, [](const SizeTarget& a, const SizeTarget& b) {
            return a.hash < b.hash || (a.hash == b.hash && a.len < b.len);
          });
      while (it != targets + targetCount && it->hash == hash && it->len == nameLen) {
        if (it->index < sizeCount) {
          sizes[it->index] = uncompressedSize;
          ++matched;
        }
        ++it;
      }
    } else if (!file.seekCur(nameLen)) {
      break;
    }

    if (!file.seekCur(static_cast<int32_t>(extraLen) + commentLen)) break;
    if (matched >= static_cast<int>(targetCount)) break;
  }
  return matched;
}

uint8_t* ZipFile::readFileToMemory(const char* filename, size_t* size, const bool trailingNullByte) {
  const ScopedOpenClose zip{*this};
  if (!zip) return nullptr;

  FileStatSlim fileStat = {};
  if (!loadFileStatSlim(filename, &fileStat)) return nullptr;

  const long fileOffset = getDataOffset(fileStat);
  if (fileOffset < 0) return nullptr;

  file.seek(fileOffset);

  const auto deflatedDataSize = fileStat.compressedSize;
  const auto inflatedDataSize = fileStat.uncompressedSize;
  if (trailingNullByte && inflatedDataSize == std::numeric_limits<size_t>::max()) {
    LOG_ERR("ZIP", "File is too large to null-terminate safely");
    return nullptr;
  }
  const size_t dataSize = static_cast<size_t>(inflatedDataSize) + (trailingNullByte ? 1U : 0U);
  const auto data = static_cast<uint8_t*>(malloc(dataSize));
  if (data == nullptr) {
    LOG_ERR("ZIP", "Failed to allocate memory for output buffer (%zu bytes)", dataSize);
    return nullptr;
  }

  if (fileStat.method == ZIP_METHOD_STORED) {
    // no deflation, just read content
    const size_t dataRead = file.read(data, inflatedDataSize);

    if (dataRead != inflatedDataSize) {
      LOG_ERR("ZIP", "Failed to read data");
      free(data);
      return nullptr;
    }

    // Continue out of block with data set
  } else if (fileStat.method == ZIP_METHOD_DEFLATED) {
    auto* fileReadBuffer = static_cast<uint8_t*>(malloc(1024));
    if (!fileReadBuffer) {
      LOG_ERR("ZIP", "Failed to allocate memory for zip file read buffer");
      free(data);
      return nullptr;
    }

    ZipInflateCtx ctx;
    ctx.file = &file;
    ctx.fileRemaining = deflatedDataSize;
    ctx.readBuf = fileReadBuffer;
    ctx.readBufSize = 1024;

    // One-shot mode: `data` holds the entire output, so back-references
    // resolve inside it and no 32KB window is allocated.
    InflateStream inflate;
    if (!inflate.init(false)) {
      LOG_ERR("ZIP", "Failed to init inflate stream");
      free(fileReadBuffer);
      free(data);
      return nullptr;
    }
    inflate.setFill(zipFillCallback, &ctx);

    if (!inflate.read(data, inflatedDataSize)) {
      LOG_ERR("ZIP", "Failed to inflate file");
      free(fileReadBuffer);
      free(data);
      return nullptr;
    }
    free(fileReadBuffer);

    // Continue out of block with data set
  } else {
    LOG_ERR("ZIP", "Unsupported compression method");
    free(data);
    return nullptr;
  }

  if (trailingNullByte) data[inflatedDataSize] = '\0';
  if (size) *size = inflatedDataSize;
  return data;
}

bool ZipFile::readFileToStream(const char* filename, Print& out, const size_t chunkSize, const bool allowEarlyStop,
                               const size_t maxOutputSize, bool* const outputLimitExceeded) {
  if (outputLimitExceeded) *outputLimitExceeded = false;
  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  FileStatSlim fileStat = {};
  if (!loadFileStatSlim(filename, &fileStat)) return false;

  const long fileOffset = getDataOffset(fileStat);
  if (fileOffset < 0) return false;

  file.seek(fileOffset);
  const auto deflatedDataSize = fileStat.compressedSize;
  const auto inflatedDataSize = fileStat.uncompressedSize;
  if (static_cast<size_t>(inflatedDataSize) > maxOutputSize) {
    if (outputLimitExceeded) *outputLimitExceeded = true;
    LOG_ERR("ZIP", "Entry output is too large (%u bytes, limit %zu)", inflatedDataSize, maxOutputSize);
    return false;
  }

  if (fileStat.method == ZIP_METHOD_STORED) {
    // no deflation, just read content
    const auto buffer = static_cast<uint8_t*>(malloc(chunkSize));
    if (!buffer) {
      LOG_ERR("ZIP", "Failed to allocate memory for buffer");
      return false;
    }

    size_t remaining = inflatedDataSize;
    while (remaining > 0) {
      const size_t dataRead = file.read(buffer, remaining < chunkSize ? remaining : chunkSize);
      if (dataRead == 0) {
        LOG_ERR("ZIP", "Could not read more bytes");
        free(buffer);
        return false;
      }

      if (out.write(buffer, dataRead) != dataRead) {
        if (!allowEarlyStop) LOG_ERR("ZIP", "Failed to write all output bytes to stream");
        free(buffer);
        return allowEarlyStop;
      }
      remaining -= dataRead;
    }

    free(buffer);
    return true;
  }

  if (fileStat.method == ZIP_METHOD_DEFLATED) {
    auto* fileReadBuffer = static_cast<uint8_t*>(malloc(chunkSize));
    if (!fileReadBuffer) {
      LOG_ERR("ZIP", "Failed to allocate memory for zip file read buffer");
      return false;
    }

    auto* outputBuffer = static_cast<uint8_t*>(malloc(chunkSize));
    if (!outputBuffer) {
      LOG_ERR("ZIP", "Failed to allocate memory for output buffer");
      free(fileReadBuffer);
      return false;
    }

    ZipInflateCtx ctx;
    ctx.file = &file;
    ctx.fileRemaining = deflatedDataSize;
    ctx.readBuf = fileReadBuffer;
    ctx.readBufSize = chunkSize;

    InflateStream inflate;
    if (!inflate.init(true)) {
      LOG_ERR("ZIP", "Failed to init inflate stream");
      free(outputBuffer);
      free(fileReadBuffer);
      return false;
    }
    inflate.setFill(zipFillCallback, &ctx);

    bool success = false;
    bool stoppedEarly = false;
    size_t totalProduced = 0;

    while (true) {
      size_t produced;
      const InflateStream::Status status = inflate.readAtMost(outputBuffer, chunkSize, &produced);

      totalProduced += produced;
      if (totalProduced > static_cast<size_t>(inflatedDataSize)) {
        LOG_ERR("ZIP", "Decompressed size exceeds expected (%zu > %zu)", totalProduced,
                static_cast<size_t>(inflatedDataSize));
        break;
      }

      if (produced > 0) {
        if (out.write(outputBuffer, produced) != produced) {
          if (!allowEarlyStop) LOG_ERR("ZIP", "Failed to write all output bytes to stream");
          stoppedEarly = allowEarlyStop;
          break;
        }
      }

      if (status == InflateStream::Status::Done) {
        if (totalProduced != static_cast<size_t>(inflatedDataSize)) {
          LOG_ERR("ZIP", "Decompressed size mismatch (expected %zu, got %zu)", static_cast<size_t>(inflatedDataSize),
                  totalProduced);
          break;
        }
        LOG_DBG("ZIP", "Decompressed %d bytes into %d bytes", deflatedDataSize, inflatedDataSize);
        success = true;
        break;
      }

      if (status == InflateStream::Status::Error) {
        LOG_ERR("ZIP", "Decompression failed");
        break;
      }
      // InflateStream::Status::Ok: output buffer full, continue
    }

    free(outputBuffer);
    free(fileReadBuffer);
    return success || stoppedEarly;  // inflate destructor frees the decompressor state + window
  }

  LOG_ERR("ZIP", "Unsupported compression method");
  return false;
}
