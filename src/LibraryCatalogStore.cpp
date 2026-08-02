#include "LibraryCatalogStore.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <Logging.h>
#include <StagedFileTransaction.h>
#include <Utf8.h>
#include <Xtc.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "RecentBooksStore.h"
#include "util/BookCacheUtils.h"
#include "util/BookPathMoveUtils.h"
#include "util/BookSearchUtils.h"

namespace {
constexpr char CATALOG_PATH[] = "/.crosspoint/library.idx";
constexpr char TEMP_PATH[] = "/.crosspoint/library.idx.tmp";
constexpr char BACKUP_PATH[] = "/.crosspoint/library.idx.bak";
constexpr char WORK_PATH[] = "/.crosspoint/library.work";
constexpr char DIRTY_PATH[] = "/.crosspoint/library.dirty";
constexpr char ORDER_PATH[] = "/.crosspoint/library.order";
constexpr char ORDER_TEMP_PATH[] = "/.crosspoint/library.order.tmp";
constexpr char ORDER_BACKUP_PATH[] = "/.crosspoint/library.order.bak";
constexpr char ORDER_WORK_A_PATH[] = "/.crosspoint/library.order.a";
constexpr char ORDER_WORK_B_PATH[] = "/.crosspoint/library.order.b";
constexpr std::array<char, 8> MAGIC = {'C', 'V', 'L', 'I', 'B', '0', '1', '\0'};
constexpr std::array<char, 8> ORDER_MAGIC = {'C', 'V', 'L', 'O', 'R', '0', '1', '\0'};
constexpr uint16_t VERSION = 2;
constexpr uint16_t ORDER_VERSION = 1;

#pragma pack(push, 1)
struct DiskHeader {
  char magic[8];
  uint16_t version;
  uint16_t recordSize;
  uint32_t count;
  uint32_t generation;
  uint8_t phase;
  uint8_t truncated;
  uint16_t reserved;
  uint32_t crc;
};

struct DiskRecord {
  char path[LibraryCatalogStore::MAX_PATH_BYTES + 1];
  char title[LibraryCatalogStore::MAX_TITLE_BYTES + 1];
  char author[LibraryCatalogStore::MAX_AUTHOR_BYTES + 1];
  char cover[LibraryCatalogStore::MAX_PATH_BYTES + 1];
  uint64_t sourceSize;
  uint32_t addedTimestamp;
  uint8_t format;
  uint8_t reserved[3];
  uint32_t crc;
};

struct OrderHeader {
  char magic[8];
  uint16_t version;
  uint8_t sortMode;
  uint8_t reserved;
  uint32_t generation;
  uint32_t count;
  uint32_t entriesCrc;
  uint32_t crc;
};

// Internal merge-sort record. The final library.order format remains a compact
// uint16_t index list. Keeping only the selected sort key and a short path
// prefix here avoids reopening the much larger catalog record for almost every
// comparison while bounding temporary SD usage.
struct OrderWorkEntry {
  uint16_t index;
  uint32_t addedTimestamp;
  uint16_t pathLength;
  char pathPrefix[64];
  char value[LibraryCatalogStore::MAX_TITLE_BYTES + 1];
};

#pragma pack(pop)

uint32_t crc32(const void* data, const size_t size) {
  uint32_t crc = 0xFFFFFFFFU;
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) {
    crc ^= bytes[i];
    for (uint8_t bit = 0; bit < 8; ++bit) crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
  }
  return ~crc;
}

uint32_t crc32Update(uint32_t crc, const void* data, const size_t size) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) {
    crc ^= bytes[i];
    for (uint8_t bit = 0; bit < 8; ++bit) crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
  }
  return crc;
}

uint32_t readTimestamp(HalFile& file) {
  uint16_t date = 0;
  uint16_t time = 0;
  if (!file.getCreateDateTime(&date, &time)) file.getModifyDateTime(&date, &time);
  return date == 0 ? 0U : (static_cast<uint32_t>(date) << 16U) | time;
}

template <typename T>
uint32_t structureCrc(const T& value) {
  return crc32(&value, sizeof(T) - sizeof(value.crc));
}

bool exactRead(HalFile& file, void* data, const size_t size) {
  return size <= static_cast<size_t>(INT_MAX) && file.read(data, size) == static_cast<int>(size);
}

bool exactWrite(HalFile& file, const void* data, const size_t size) { return file.write(data, size) == size; }

bool validOrderHeader(const OrderHeader& header) {
  return std::memcmp(header.magic, ORDER_MAGIC.data(), ORDER_MAGIC.size()) == 0 && header.version == ORDER_VERSION &&
         header.sortMode < CrossPointSettings::LIBRARY_SORT_COUNT && header.count <= LibraryCatalogStore::MAX_BOOKS &&
         header.crc == structureCrc(header);
}

bool validateOrder(const char* path, void*) {
  HalFile file;
  if (!Storage.openFileForRead("LIB", path, file)) return false;
  OrderHeader header{};
  if (!exactRead(file, &header, sizeof(header)) || !validOrderHeader(header) ||
      file.fileSize64() != sizeof(header) + static_cast<uint64_t>(header.count) * sizeof(uint16_t)) {
    file.close();
    return false;
  }
  std::array<uint8_t, (LibraryCatalogStore::MAX_BOOKS + 7) / 8> seen{};
  uint32_t crc = 0xFFFFFFFFU;
  bool valid = true;
  for (uint32_t position = 0; position < header.count; ++position) {
    uint16_t index = 0;
    valid = exactRead(file, &index, sizeof(index)) && index < header.count;
    if (!valid) break;
    const uint8_t mask = static_cast<uint8_t>(1U << (index & 7U));
    uint8_t& byte = seen[index >> 3U];
    if ((byte & mask) != 0) {
      valid = false;
      break;
    }
    byte |= mask;
    crc = crc32Update(crc, &index, sizeof(index));
  }
  const bool closed = file.close();
  return valid && closed && header.entriesCrc == ~crc;
}

enum class DirtyMarkerKind : uint8_t { None, Generic, Path, DeletedPath, Invalid };

DirtyMarkerKind readDirtyMarker(std::string& path) {
  path.clear();
  if (!Storage.exists("/.crosspoint/library.dirty")) return DirtyMarkerKind::None;
  const String contents = Storage.readFile("/.crosspoint/library.dirty");
  if (contents.equals("1")) return DirtyMarkerKind::Generic;
  const std::string raw = contents.c_str();
  const std::string pathPrefix = LibraryCatalogStore::dirtyPathPrefix();
  const std::string deletedPrefix = LibraryCatalogStore::deletedPathPrefix();
  const bool deleted = raw.rfind(deletedPrefix, 0) == 0;
  const std::string& prefix = deleted ? deletedPrefix : pathPrefix;
  if (raw.rfind(prefix, 0) != 0 || raw.size() == prefix.size() ||
      raw.size() - prefix.size() > LibraryCatalogStore::MAX_PATH_BYTES) {
    return DirtyMarkerKind::Invalid;
  }
  path = raw.substr(prefix.size());
  return deleted ? DirtyMarkerKind::DeletedPath : DirtyMarkerKind::Path;
}

bool isSupportedBook(const std::string& path, LibraryBookFormat& format) {
  if (FsHelpers::hasEpubExtension(path)) {
    format = LibraryBookFormat::Epub;
  } else if (FsHelpers::hasTxtExtension(path)) {
    format = LibraryBookFormat::Text;
  } else if (FsHelpers::hasMarkdownExtension(path)) {
    format = LibraryBookFormat::Markdown;
  } else if (FsHelpers::checkFileExtension(path, ".xtch")) {
    format = LibraryBookFormat::Xtch;
  } else if (FsHelpers::checkFileExtension(path, ".xtc")) {
    format = LibraryBookFormat::Xtc;
  } else {
    return false;
  }
  return true;
}

std::string fallbackTitle(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  std::string title = slash == std::string::npos ? path : path.substr(slash + 1);
  const size_t dot = title.find_last_of('.');
  if (dot != std::string::npos) title.resize(dot);
  std::replace(title.begin(), title.end(), '_', ' ');
  if (title.size() > LibraryCatalogStore::MAX_TITLE_BYTES) {
    title.resize(utf8SafeTruncateBuffer(title.c_str(), LibraryCatalogStore::MAX_TITLE_BYTES));
  }
  return title;
}

bool copyString(const std::string& source, char* destination, const size_t capacity) {
  if (source.size() >= capacity) return false;
  std::memcpy(destination, source.c_str(), source.size() + 1);
  return true;
}

void copyTruncatedUtf8(const std::string& source, char* destination, const size_t capacity) {
  size_t length = std::min(source.size(), capacity - 1);
  if (length < source.size()) length = static_cast<size_t>(utf8SafeTruncateBuffer(source.c_str(), length));
  std::memcpy(destination, source.data(), length);
  destination[length] = '\0';
}

bool toDisk(const LibraryBookRecord& record, DiskRecord& disk) {
  disk = {};
  if (!copyString(record.path, disk.path, sizeof(disk.path))) return false;
  copyTruncatedUtf8(record.title, disk.title, sizeof(disk.title));
  copyTruncatedUtf8(record.author, disk.author, sizeof(disk.author));
  // A cover path is derived cache state. Dropping an oversized value is safe;
  // the thumbnail will be regenerated when the book is next visible.
  if (record.coverBmpPath.size() < sizeof(disk.cover)) copyString(record.coverBmpPath, disk.cover, sizeof(disk.cover));
  disk.sourceSize = record.sourceSize;
  disk.addedTimestamp = record.addedTimestamp;
  disk.format = static_cast<uint8_t>(record.format);
  disk.crc = structureCrc(disk);
  return true;
}

bool validDiskRecord(const DiskRecord& disk) {
  return disk.crc == structureCrc(disk) && disk.path[sizeof(disk.path) - 1] == '\0' &&
         disk.title[sizeof(disk.title) - 1] == '\0' && disk.author[sizeof(disk.author) - 1] == '\0' &&
         disk.cover[sizeof(disk.cover) - 1] == '\0' && disk.format <= static_cast<uint8_t>(LibraryBookFormat::Xtch);
}

bool fromDisk(const DiskRecord& disk, LibraryBookRecord& record) {
  if (!validDiskRecord(disk)) return false;
  record.path = disk.path;
  record.title = disk.title;
  record.author = disk.author;
  record.coverBmpPath = disk.cover;
  record.format = static_cast<LibraryBookFormat>(disk.format);
  record.sourceSize = disk.sourceSize;
  record.addedTimestamp = disk.addedTimestamp;
  record.pinned = false;
  return true;
}

bool readRecordAt(HalFile& file, const size_t index, LibraryBookRecord& record) {
  const uint64_t offset = sizeof(DiskHeader) + static_cast<uint64_t>(index) * sizeof(DiskRecord);
  if (!file.seek64(offset)) return false;
  DiskRecord disk{};
  return exactRead(file, &disk, sizeof(disk)) && fromDisk(disk, record);
}

size_t orderWorkEntrySize(const uint8_t sortMode) {
  return sortMode == CrossPointSettings::LIBRARY_SORT_DATE_ADDED_DESC ? offsetof(OrderWorkEntry, value)
                                                                      : sizeof(OrderWorkEntry);
}

bool makeOrderWorkEntry(const LibraryBookRecord& record, const size_t index, const uint8_t sortMode,
                        OrderWorkEntry& entry) {
  if (index > UINT16_MAX || record.path.size() > UINT16_MAX) return false;
  entry = {};
  entry.index = static_cast<uint16_t>(index);
  entry.addedTimestamp = record.addedTimestamp;
  entry.pathLength = static_cast<uint16_t>(record.path.size());
  std::memcpy(entry.pathPrefix, record.path.data(), std::min(record.path.size(), sizeof(entry.pathPrefix)));
  if (sortMode == CrossPointSettings::LIBRARY_SORT_TITLE_ASC) {
    return copyString(record.title, entry.value, sizeof(entry.value));
  }
  if (sortMode == CrossPointSettings::LIBRARY_SORT_AUTHOR_ASC) {
    return copyString(record.author, entry.value, sizeof(entry.value));
  }
  return sortMode == CrossPointSettings::LIBRARY_SORT_DATE_ADDED_DESC;
}

bool readOrderWorkEntryAt(HalFile& file, const size_t position, const uint8_t sortMode, OrderWorkEntry& entry) {
  entry = {};
  const size_t entrySize = orderWorkEntrySize(sortMode);
  return file.seek64(static_cast<uint64_t>(position) * entrySize) && exactRead(file, &entry, entrySize);
}

bool orderPathLess(const OrderWorkEntry& first, const OrderWorkEntry& second, HalFile& catalog, bool& less) {
  const size_t shared = std::min<size_t>({first.pathLength, second.pathLength, sizeof(first.pathPrefix)});
  const int prefixOrder = std::memcmp(first.pathPrefix, second.pathPrefix, shared);
  if (prefixOrder != 0) {
    less = prefixOrder < 0;
    return true;
  }
  if (std::min(first.pathLength, second.pathLength) <= sizeof(first.pathPrefix)) {
    less = first.pathLength < second.pathLength;
    return true;
  }

  // Only paths sharing the full 64-byte prefix need a catalog lookup. This is
  // rare for normal root/Books layouts and preserves the previous exact tie
  // break without retaining every full path in RAM or the work files.
  LibraryBookRecord firstRecord;
  LibraryBookRecord secondRecord;
  if (!readRecordAt(catalog, first.index, firstRecord) || !readRecordAt(catalog, second.index, secondRecord)) {
    return false;
  }
  less = firstRecord.path < secondRecord.path;
  return true;
}

bool orderWorkEntryLess(const OrderWorkEntry& first, const OrderWorkEntry& second, const uint8_t sortMode,
                        HalFile& catalog, bool& less) {
  if (sortMode == CrossPointSettings::LIBRARY_SORT_TITLE_ASC ||
      sortMode == CrossPointSettings::LIBRARY_SORT_AUTHOR_ASC) {
    const bool firstEmpty = first.value[0] == '\0';
    const bool secondEmpty = second.value[0] == '\0';
    if (sortMode == CrossPointSettings::LIBRARY_SORT_AUTHOR_ASC && firstEmpty != secondEmpty) {
      less = !firstEmpty;
      return true;
    }
    const BookSearchQuery firstKey = makeBookSearchQuery(first.value);
    const BookSearchQuery secondKey = makeBookSearchQuery(second.value);
    if (firstKey.folded != secondKey.folded) {
      less = firstKey.folded < secondKey.folded;
      return true;
    }
    const int originalOrder = std::strcmp(first.value, second.value);
    if (originalOrder != 0) {
      less = originalOrder < 0;
      return true;
    }
  } else {
    const bool firstMissing = first.addedTimestamp == 0;
    const bool secondMissing = second.addedTimestamp == 0;
    if (firstMissing != secondMissing) {
      less = !firstMissing;
      return true;
    }
    if (first.addedTimestamp != second.addedTimestamp) {
      less = first.addedTimestamp > second.addedTimestamp;
      return true;
    }
  }
  return orderPathLess(first, second, catalog, less);
}

bool validateCatalog(const char* path, void*) {
  HalFile file;
  if (!Storage.openFileForRead("LIB", path, file)) return false;
  DiskHeader header{};
  bool valid = exactRead(file, &header, sizeof(header)) && std::memcmp(header.magic, MAGIC.data(), MAGIC.size()) == 0 &&
               header.version == VERSION && header.recordSize == sizeof(DiskRecord) &&
               header.count <= LibraryCatalogStore::MAX_BOOKS &&
               header.phase == static_cast<uint8_t>(LibraryCatalogStore::Phase::Ready) &&
               header.crc == structureCrc(header) &&
               file.fileSize64() == sizeof(DiskHeader) + static_cast<uint64_t>(header.count) * sizeof(DiskRecord);
  for (uint32_t index = 0; valid && index < header.count; ++index) {
    DiskRecord disk{};
    LibraryBookRecord record;
    valid = exactRead(file, &disk, sizeof(disk)) && fromDisk(disk, record);
  }
  return file.close() && valid;
}

bool isSkippedDirectory(const char* name) {
  return name[0] == '.' || std::strcmp(name, "System Volume Information") == 0;
}

void enrichRecord(LibraryBookRecord& record) {
  if (record.format == LibraryBookFormat::Epub) {
    Epub epub(record.path, "/.crosspoint");
    BookMetadataCache::BookMetadata metadata;
    if (!epub.readCoreMetadata(metadata)) return;
    if (!metadata.title.empty()) record.title = metadata.title;
    record.author = metadata.author;
    record.coverBmpPath = metadata.coverItemHref.empty() ? std::string{} : epub.getThumbBmpPath();
    return;
  }
  if (record.format != LibraryBookFormat::Xtc && record.format != LibraryBookFormat::Xtch) return;
  Xtc xtc(record.path, "/.crosspoint");
  if (!xtc.load()) return;
  if (!xtc.getTitle().empty()) record.title = xtc.getTitle();
  record.author = xtc.getAuthor();
  record.coverBmpPath = xtc.getThumbBmpPath();
}
}  // namespace

LibraryCatalogStore& LibraryCatalogStore::getInstance() {
  static LibraryCatalogStore instance;
  return instance;
}

const char* LibraryCatalogStore::activePath() const { return isBuilding() ? WORK_PATH : CATALOG_PATH; }

bool LibraryCatalogStore::loadHeader(const char* path, const bool requireReady) {
  HalFile file;
  if (!Storage.openFileForRead("LIB", path, file)) return false;
  DiskHeader header{};
  const bool valid =
      exactRead(file, &header, sizeof(header)) && std::memcmp(header.magic, MAGIC.data(), MAGIC.size()) == 0 &&
      header.version == VERSION && header.recordSize == sizeof(DiskRecord) && header.count <= MAX_BOOKS &&
      header.phase <= static_cast<uint8_t>(Phase::Error) &&
      (!requireReady || header.phase == static_cast<uint8_t>(Phase::Ready)) && header.crc == structureCrc(header) &&
      file.fileSize64() >= sizeof(DiskHeader) + static_cast<uint64_t>(header.count) * sizeof(DiskRecord);
  file.close();
  if (!valid) return false;
  count_ = header.count;
  generation_ = header.generation;
  truncated_ = header.truncated != 0;
  phase_ = requireReady ? Phase::Ready : static_cast<Phase>(header.phase);
  return true;
}

bool LibraryCatalogStore::open() {
  resetOrderBuild(true);
  StagedFileTransaction::recover(CATALOG_PATH, BACKUP_PATH, validateCatalog);
  StagedFileTransaction::recover(ORDER_PATH, ORDER_BACKUP_PATH, validateOrder);
  Storage.remove(TEMP_PATH);
  Storage.remove(ORDER_TEMP_PATH);
  if (loadHeader(CATALOG_PATH, true)) {
    std::string changedPath;
    const DirtyMarkerKind marker = readDirtyMarker(changedPath);
    // An empty catalog cannot prove that the card is still empty: books copied
    // directly to the SD card do not create our dirty marker. Re-run the
    // cooperative discovery so an externally added first book is found.
    if (marker == DirtyMarkerKind::None) return count_ == 0 ? beginBuild() : true;
    if (marker == DirtyMarkerKind::Path && applyDirtyPath(changedPath)) return true;
    if (marker == DirtyMarkerKind::DeletedPath && applyDeletedPath(changedPath)) return true;
  }
  return beginBuild();
}

bool LibraryCatalogStore::startRefresh() { return beginBuild(); }

bool LibraryCatalogStore::restorePreviousCatalog() {
  resetFinalize(true);
  for (auto& frame : directories_) frame.directory.close();
  directories_.clear();
  StagedFileTransaction::recover(CATALOG_PATH, BACKUP_PATH, validateCatalog);
  if (loadHeader(CATALOG_PATH, true)) return true;
  count_ = 0;
  truncated_ = false;
  phase_ = Phase::Error;
  return false;
}

bool LibraryCatalogStore::beginBuild() {
  cancel();
  lastBuildFailed_ = false;
  Storage.ensureDirectoryExists("/.crosspoint");
  Storage.remove(TEMP_PATH);
  HalFile work;
  if (!Storage.openFileForWrite("LIB", WORK_PATH, work)) {
    lastBuildFailed_ = true;
    restorePreviousCatalog();
    return false;
  }
  work.close();
  count_ = 0;
  truncated_ = false;
  phase_ = Phase::Discovering;
  ++generation_;
  if (!writeWorkHeader(phase_)) {
    lastBuildFailed_ = true;
    restorePreviousCatalog();
    return false;
  }
  HalFile root = Storage.open("/");
  if (!root || !root.isDirectory()) {
    lastBuildFailed_ = true;
    restorePreviousCatalog();
    return false;
  }
  root.rewindDirectory();
  directories_.push_back({"/", std::move(root)});
  return true;
}

bool LibraryCatalogStore::applyDirtyPath(const std::string& path) {
  LibraryBookFormat format;
  if (!isSupportedBook(path, format)) {
    Storage.remove(DIRTY_PATH);
    return true;
  }

  HalFile source;
  if (!Storage.openFileForRead("LIB", path, source)) return false;
  const uint64_t sourceSize = source.fileSize64();
  const uint32_t addedTimestamp = readTimestamp(source);
  const bool sourceClosed = source.close();
  if (!sourceClosed) return false;
  size_t existingIndex = 0;
  const FindPathResult existing = findPath(path, count_, existingIndex);
  if (existing == FindPathResult::IoError) return false;
  const bool replacing = existing == FindPathResult::Found;
  if (!replacing && count_ >= MAX_BOOKS) return false;

  LibraryBookRecord updated{path, fallbackTitle(path), "", "", format, sourceSize, addedTimestamp};
  const auto& recents = RECENT_BOOKS.getBooks();
  const auto recent =
      std::find_if(recents.begin(), recents.end(), [&path](const RecentBook& book) { return book.path == path; });
  if (recent != recents.end()) {
    if (!recent->title.empty()) updated.title = recent->title;
    updated.author = recent->author;
    updated.coverBmpPath = recent->coverBmpPath;
  }
  enrichRecord(updated);

  Storage.remove(TEMP_PATH);
  HalFile input;
  HalFile output;
  if (!Storage.openFileForRead("LIB", CATALOG_PATH, input) || !Storage.openFileForWrite("LIB", TEMP_PATH, output)) {
    input.close();
    output.close();
    Storage.remove(TEMP_PATH);
    return false;
  }

  DiskHeader header{};
  header = {};
  std::memcpy(header.magic, MAGIC.data(), MAGIC.size());
  header.version = VERSION;
  header.recordSize = sizeof(DiskRecord);
  header.count = replacing ? count_ : count_ + 1;
  header.generation = generation_ + 1;
  header.phase = static_cast<uint8_t>(Phase::Ready);
  header.truncated = truncated_;
  header.crc = structureCrc(header);
  bool success = exactWrite(output, &header, sizeof(header));
  bool replaced = false;
  for (size_t index = 0; success && index < count_; ++index) {
    LibraryBookRecord record;
    if (!readRecordAt(input, index, record)) {
      success = false;
      break;
    }
    if (replacing && record.path == path) {
      record = updated;
      replaced = true;
    }
    DiskRecord disk{};
    success = toDisk(record, disk) && exactWrite(output, &disk, sizeof(disk));
  }
  if (success && !replacing) {
    DiskRecord disk{};
    success = toDisk(updated, disk) && exactWrite(output, &disk, sizeof(disk));
  }
  success = success && (!replacing || replaced);
  const bool synced = success && output.sync();
  const bool outputClosed = output.close();
  const bool inputClosed = input.close();
  success = success && synced && outputClosed && inputClosed && validateCatalog(TEMP_PATH, nullptr) &&
            StagedFileTransaction::publish(CATALOG_PATH, TEMP_PATH, BACKUP_PATH, validateCatalog) ==
                StagedFileTransaction::Status::Published;
  if (!success) {
    Storage.remove(TEMP_PATH);
    return false;
  }

  count_ = header.count;
  generation_ = header.generation;
  phase_ = Phase::Ready;
  Storage.remove(ORDER_PATH);
  Storage.remove(DIRTY_PATH);
  return true;
}

bool LibraryCatalogStore::applyDeletedPath(const std::string& path) {
  size_t existingIndex = 0;
  const FindPathResult existing = findPath(path, count_, existingIndex);
  if (existing == FindPathResult::IoError) return false;
  if (existing == FindPathResult::NotFound) {
    Storage.remove(DIRTY_PATH);
    return true;
  }
  if (count_ == 0) return false;

  Storage.remove(TEMP_PATH);
  HalFile input;
  HalFile output;
  if (!Storage.openFileForRead("LIB", CATALOG_PATH, input) || !Storage.openFileForWrite("LIB", TEMP_PATH, output)) {
    input.close();
    output.close();
    Storage.remove(TEMP_PATH);
    return false;
  }

  DiskHeader header{};
  std::memcpy(header.magic, MAGIC.data(), MAGIC.size());
  header.version = VERSION;
  header.recordSize = sizeof(DiskRecord);
  header.count = count_ - 1;
  header.generation = generation_ + 1;
  header.phase = static_cast<uint8_t>(Phase::Ready);
  header.truncated = truncated_;
  header.crc = structureCrc(header);
  bool success = exactWrite(output, &header, sizeof(header));
  for (size_t index = 0; success && index < count_; ++index) {
    if (index == existingIndex) continue;
    LibraryBookRecord record;
    success = readRecordAt(input, index, record);
    if (success) {
      DiskRecord disk{};
      success = toDisk(record, disk) && exactWrite(output, &disk, sizeof(disk));
    }
  }
  const bool synced = success && output.sync();
  const bool outputClosed = output.close();
  const bool inputClosed = input.close();
  success = success && synced && outputClosed && inputClosed && validateCatalog(TEMP_PATH, nullptr) &&
            StagedFileTransaction::publish(CATALOG_PATH, TEMP_PATH, BACKUP_PATH, validateCatalog) ==
                StagedFileTransaction::Status::Published;
  if (!success) {
    Storage.remove(TEMP_PATH);
    return false;
  }

  count_ = header.count;
  generation_ = header.generation;
  phase_ = Phase::Ready;
  Storage.remove(ORDER_PATH);
  Storage.remove(DIRTY_PATH);
  return true;
}

void LibraryCatalogStore::cancel() {
  const bool wasBuilding = isBuilding();
  resetOrderBuild(true);
  resetFinalize(true);
  for (auto& frame : directories_) frame.directory.close();
  directories_.clear();
  if (!wasBuilding) return;
  if (!loadHeader(CATALOG_PATH, true)) {
    count_ = 0;
    truncated_ = false;
    phase_ = Phase::Idle;
  }
}

void LibraryCatalogStore::step() {
  if (phase_ == Phase::Discovering) {
    discoverOne();
  } else if (phase_ == Phase::Sorting) {
    if (!finalizeBuild()) phase_ = Phase::Error;
  }
  if (phase_ == Phase::Error) {
    lastBuildFailed_ = true;
    restorePreviousCatalog();
  }
  if (phase_ == Phase::Ready && isOrderBuilding() && !stepOrderBuild()) {
    // A failed sidecar must never poison the committed catalog. Drop only the
    // derived order and let a later open retry it.
    resetOrderBuild(true);
  }
}

void LibraryCatalogStore::discoverOne() {
  if (directories_.empty()) {
    phase_ = Phase::Sorting;
    if (!writeWorkHeader(phase_)) phase_ = Phase::Error;
    return;
  }
  auto& frame = directories_.back();
  HalFile entry = frame.directory.openNextFile();
  if (!entry) {
    const bool error = frame.directory.getError() != 0;
    frame.directory.close();
    directories_.pop_back();
    if (error) phase_ = Phase::Error;
    return;
  }

  char name[MAX_PATH_BYTES + 1]{};
  if (entry.getName(name, sizeof(name)) == 0 || name[sizeof(name) - 1] != '\0') {
    entry.close();
    return;
  }
  const bool directory = entry.isDirectory();
  const uint64_t size = directory ? 0 : entry.fileSize64();
  const uint32_t addedTimestamp = directory ? 0 : readTimestamp(entry);
  entry.close();
  if (name[0] == '\0' || isBookFileTransactionArtifact(name)) return;
  if (directory && isSkippedDirectory(name)) return;

  std::string path = frame.path;
  if (path.size() > 1) path += '/';
  path += name;
  if (path.size() > MAX_PATH_BYTES) {
    LibraryBookFormat oversizedFormat;
    if (!directory && isSupportedBook(path, oversizedFormat)) {
      // A source path is authoritative catalog data and cannot be truncated.
      // Keep the previously committed catalog and the dirty marker so a later
      // firmware with a wider format can retry instead of silently publishing
      // a catalog that has dropped this book.
      markDirty();
      phase_ = Phase::Error;
      return;
    }
    truncated_ = true;
    return;
  }
  if (directory) {
    if (directories_.size() >= MAX_DEPTH) {
      truncated_ = true;
      return;
    }
    HalFile child = Storage.open(path.c_str());
    if (child && child.isDirectory()) {
      child.rewindDirectory();
      directories_.push_back({std::move(path), std::move(child)});
    }
    return;
  }
  if (count_ >= MAX_BOOKS) {
    truncated_ = true;
    return;
  }
  LibraryBookFormat format;
  if (!isSupportedBook(path, format)) return;
  LibraryBookRecord record{path, fallbackTitle(path), "", "", format, size, addedTimestamp};
  const auto& recents = RECENT_BOOKS.getBooks();
  const auto recent =
      std::find_if(recents.begin(), recents.end(), [&path](const RecentBook& book) { return book.path == path; });
  if (recent != recents.end()) {
    if (!recent->title.empty()) record.title = recent->title;
    record.author = recent->author;
    record.coverBmpPath = recent->coverBmpPath;
  }
  enrichRecord(record);
  if (!appendRecord(record)) phase_ = Phase::Error;
}

bool LibraryCatalogStore::finalizeBuild() {
  if (!finalizeStarted_) {
    if (!Storage.openFileForRead("LIB", WORK_PATH, finalizeInput_) ||
        !Storage.openFileForWrite("LIB", TEMP_PATH, finalizeOutput_)) {
      resetFinalize(true);
      return false;
    }
    DiskHeader header{};
    std::memcpy(header.magic, MAGIC.data(), MAGIC.size());
    header.version = VERSION;
    header.recordSize = sizeof(DiskRecord);
    header.count = count_;
    header.generation = generation_;
    header.phase = static_cast<uint8_t>(Phase::Ready);
    header.truncated = truncated_;
    header.crc = structureCrc(header);
    if (!exactWrite(finalizeOutput_, &header, sizeof(header))) {
      resetFinalize(true);
      return false;
    }
    finalizeIndex_ = 0;
    finalizeStarted_ = true;
  }

  if (finalizeIndex_ < count_) {
    LibraryBookRecord record;
    if (!readRecordAt(finalizeInput_, finalizeIndex_, record)) {
      resetFinalize(true);
      return false;
    }
    DiskRecord disk{};
    if (!toDisk(record, disk) || !exactWrite(finalizeOutput_, &disk, sizeof(disk))) {
      resetFinalize(true);
      return false;
    }
    ++finalizeIndex_;
    return true;
  }

  const bool synced = finalizeOutput_.sync();
  const bool outputClosed = finalizeOutput_.close();
  const bool inputClosed = finalizeInput_.close();
  const bool durable = synced && outputClosed && inputClosed;
  finalizeStarted_ = false;
  finalizeIndex_ = 0;
  if (!durable || !validateCatalog(TEMP_PATH, nullptr) ||
      StagedFileTransaction::publish(CATALOG_PATH, TEMP_PATH, BACKUP_PATH, validateCatalog) !=
          StagedFileTransaction::Status::Published) {
    Storage.remove(TEMP_PATH);
    return false;
  }
  Storage.remove(WORK_PATH);
  Storage.remove(ORDER_PATH);
  Storage.remove(DIRTY_PATH);
  return loadHeader(CATALOG_PATH, true);
}

void LibraryCatalogStore::resetFinalize(const bool removeTemporary) {
  if (finalizeInput_) finalizeInput_.close();
  if (finalizeOutput_) finalizeOutput_.close();
  finalizeIndex_ = 0;
  finalizeStarted_ = false;
  if (removeTemporary) Storage.remove(TEMP_PATH);
}

bool LibraryCatalogStore::writeWorkHeader(const Phase phase) {
  HalFile file = Storage.open(WORK_PATH, O_RDWR);
  if (!file || !file.seekSet(0)) return false;
  DiskHeader header{};
  std::memcpy(header.magic, MAGIC.data(), MAGIC.size());
  header.version = VERSION;
  header.recordSize = sizeof(DiskRecord);
  header.count = count_;
  header.generation = generation_;
  header.phase = static_cast<uint8_t>(phase);
  header.truncated = truncated_;
  header.crc = structureCrc(header);
  const bool success = exactWrite(file, &header, sizeof(header));
  file.flush();
  const bool closed = file.close();
  return success && closed;
}

bool LibraryCatalogStore::appendRecord(const LibraryBookRecord& record) {
  HalFile file = Storage.open(WORK_PATH, O_RDWR);
  const uint64_t offset = sizeof(DiskHeader) + static_cast<uint64_t>(count_) * sizeof(DiskRecord);
  if (!file || !file.seek64(offset)) return false;
  DiskRecord disk{};
  const bool written = toDisk(record, disk) && exactWrite(file, &disk, sizeof(disk));
  const bool closed = file.close();
  if (!written || !closed) return false;
  ++count_;
  return writeWorkHeader(phase_);
}

bool LibraryCatalogStore::readRecord(const char* path, const size_t index, LibraryBookRecord& record) const {
  HalFile file;
  if (index >= count_ || !Storage.openFileForRead("LIB", path, file)) return false;
  const bool success = readRecordAt(file, index, record);
  file.close();
  return success;
}

bool LibraryCatalogStore::loadRecord(const size_t index, LibraryBookRecord& record) const {
  return readRecord(activePath(), index, record);
}

bool LibraryCatalogStore::loadIndicesExcludingFormat(const LibraryBookFormat excluded,
                                                     std::vector<size_t>& indices) const {
  indices.clear();
  HalFile file;
  if (!Storage.openFileForRead("LIB", activePath(), file)) return false;
  indices.reserve(count_);
  for (size_t index = 0; index < count_; ++index) {
    LibraryBookRecord record;
    if (!readRecordAt(file, index, record)) {
      file.close();
      indices.clear();
      return false;
    }
    if (record.format != excluded) indices.push_back(index);
  }
  if (!file.close()) {
    indices.clear();
    return false;
  }
  return true;
}

bool LibraryCatalogStore::ensureOrder(const uint8_t sortMode) {
  if (!isReady() || sortMode >= CrossPointSettings::LIBRARY_SORT_COUNT) return false;

  HalFile existing;
  OrderHeader existingHeader{};
  if (Storage.openFileForRead("LIB", ORDER_PATH, existing)) {
    const bool readable = exactRead(existing, &existingHeader, sizeof(existingHeader));
    const bool closed = existing.close();
    if (readable && closed && validOrderHeader(existingHeader) && existingHeader.generation == generation_ &&
        existingHeader.count == count_ && existingHeader.sortMode == sortMode && validateOrder(ORDER_PATH, nullptr)) {
      return true;
    }
  }

  if (isOrderBuilding() && orderSortMode_ != sortMode) resetOrderBuild(true);
  if (!isOrderBuilding()) startOrderBuild(sortMode);
  return false;
}

bool LibraryCatalogStore::startOrderBuild(const uint8_t sortMode) {
  if (!isReady() || sortMode >= CrossPointSettings::LIBRARY_SORT_COUNT || isOrderBuilding()) return false;
  resetOrderBuild(true);
  Storage.remove(ORDER_WORK_A_PATH);
  Storage.remove(ORDER_WORK_B_PATH);
  if (!Storage.openFileForWrite("LIB", ORDER_WORK_A_PATH, orderOutput_) ||
      !Storage.openFileForRead("LIB", activePath(), orderCatalog_)) {
    resetOrderBuild(true);
    return false;
  }
  orderSortMode_ = sortMode;
  orderSourceA_ = true;
  orderRunWidth_ = 1;
  orderSeedIndex_ = 0;
  orderPhase_ = OrderPhase::Initializing;
  return true;
}

void LibraryCatalogStore::resetOrderBuild(const bool removeTemporary) {
  if (orderInput_) orderInput_.close();
  if (orderOutput_) orderOutput_.close();
  if (orderCatalog_) orderCatalog_.close();
  orderPhase_ = OrderPhase::Idle;
  orderRunWidth_ = 0;
  orderBase_ = orderLeft_ = orderLeftEnd_ = orderRight_ = orderRightEnd_ = 0;
  orderPublished_ = 0;
  orderEntriesCrc_ = 0;
  orderSeedIndex_ = 0;
  if (removeTemporary) {
    Storage.remove(ORDER_TEMP_PATH);
    Storage.remove(ORDER_WORK_A_PATH);
    Storage.remove(ORDER_WORK_B_PATH);
  }
}

bool LibraryCatalogStore::stepOrderBuild() {
  if (orderPhase_ == OrderPhase::Idle) return true;
  const char* inputPath = orderSourceA_ ? ORDER_WORK_A_PATH : ORDER_WORK_B_PATH;
  const char* outputPath = orderSourceA_ ? ORDER_WORK_B_PATH : ORDER_WORK_A_PATH;
  if (orderPhase_ == OrderPhase::Initializing) {
    if (orderSeedIndex_ < count_) {
      LibraryBookRecord record;
      OrderWorkEntry entry{};
      const size_t index = orderSeedIndex_++;
      if (!readRecordAt(orderCatalog_, index, record) || !makeOrderWorkEntry(record, index, orderSortMode_, entry) ||
          !exactWrite(orderOutput_, &entry, orderWorkEntrySize(orderSortMode_))) {
        resetOrderBuild(true);
        return false;
      }
      return true;
    }
    if (!orderOutput_.sync() || !orderOutput_.close() || !orderCatalog_.close()) {
      resetOrderBuild(true);
      return false;
    }
    orderPhase_ = OrderPhase::Merging;
    return true;
  }
  if (orderPhase_ == OrderPhase::Merging) {
    if (!orderInput_) {
      Storage.remove(outputPath);
      if (!Storage.openFileForRead("LIB", inputPath, orderInput_) ||
          !Storage.openFileForWrite("LIB", outputPath, orderOutput_) ||
          !Storage.openFileForRead("LIB", activePath(), orderCatalog_)) {
        resetOrderBuild(true);
        return false;
      }
      orderBase_ = 0;
      orderLeft_ = 0;
      orderLeftEnd_ = std::min(orderRunWidth_, count_);
      orderRight_ = orderLeftEnd_;
      orderRightEnd_ = std::min(orderRight_ + orderRunWidth_, count_);
    }
    if (orderBase_ < count_) {
      OrderWorkEntry selected{};
      if (orderLeft_ >= orderLeftEnd_) {
        if (!readOrderWorkEntryAt(orderInput_, orderRight_++, orderSortMode_, selected)) {
          resetOrderBuild(true);
          return false;
        }
      } else if (orderRight_ >= orderRightEnd_) {
        if (!readOrderWorkEntryAt(orderInput_, orderLeft_++, orderSortMode_, selected)) {
          resetOrderBuild(true);
          return false;
        }
      } else {
        OrderWorkEntry left{};
        OrderWorkEntry right{};
        if (!readOrderWorkEntryAt(orderInput_, orderLeft_, orderSortMode_, left) ||
            !readOrderWorkEntryAt(orderInput_, orderRight_, orderSortMode_, right)) {
          resetOrderBuild(true);
          return false;
        }
        bool rightBeforeLeft = false;
        if (!orderWorkEntryLess(right, left, orderSortMode_, orderCatalog_, rightBeforeLeft)) {
          resetOrderBuild(true);
          return false;
        }
        selected = rightBeforeLeft ? right : left;
        if (rightBeforeLeft) {
          ++orderRight_;
        } else {
          ++orderLeft_;
        }
      }
      if (!exactWrite(orderOutput_, &selected, orderWorkEntrySize(orderSortMode_))) {
        resetOrderBuild(true);
        return false;
      }
      if (orderLeft_ == orderLeftEnd_ && orderRight_ == orderRightEnd_) {
        orderBase_ = orderRightEnd_;
        orderLeft_ = orderBase_;
        orderLeftEnd_ = std::min(orderLeft_ + orderRunWidth_, count_);
        orderRight_ = orderLeftEnd_;
        orderRightEnd_ = std::min(orderRight_ + orderRunWidth_, count_);
      }
      return true;
    }
    if (!orderOutput_.sync() || !orderOutput_.close() || !orderInput_.close() || !orderCatalog_.close()) {
      resetOrderBuild(true);
      return false;
    }
    orderSourceA_ = !orderSourceA_;
    if (orderRunWidth_ >= count_ || orderRunWidth_ > count_ / 2) {
      orderPhase_ = OrderPhase::Publishing;
      return true;
    }
    orderRunWidth_ *= 2;
    return true;
  }

  if (!orderInput_) {
    if (!Storage.openFileForRead("LIB", inputPath, orderInput_) ||
        !Storage.openFileForWrite("LIB", ORDER_TEMP_PATH, orderOutput_)) {
      resetOrderBuild(true);
      return false;
    }
    OrderHeader header{};
    std::memcpy(header.magic, ORDER_MAGIC.data(), ORDER_MAGIC.size());
    header.version = ORDER_VERSION;
    header.sortMode = orderSortMode_;
    header.generation = generation_;
    header.count = count_;
    if (!exactWrite(orderOutput_, &header, sizeof(header))) {
      resetOrderBuild(true);
      return false;
    }
    orderPublished_ = 0;
    orderEntriesCrc_ = 0xFFFFFFFFU;
  }
  if (orderPublished_ < count_) {
    OrderWorkEntry entry{};
    if (!exactRead(orderInput_, &entry, orderWorkEntrySize(orderSortMode_)) ||
        !exactWrite(orderOutput_, &entry.index, sizeof(entry.index))) {
      resetOrderBuild(true);
      return false;
    }
    orderEntriesCrc_ = crc32Update(orderEntriesCrc_, &entry.index, sizeof(entry.index));
    ++orderPublished_;
    return true;
  }
  OrderHeader header{};
  header.entriesCrc = ~orderEntriesCrc_;
  std::memcpy(header.magic, ORDER_MAGIC.data(), ORDER_MAGIC.size());
  header.version = ORDER_VERSION;
  header.sortMode = orderSortMode_;
  header.generation = generation_;
  header.count = count_;
  header.crc = structureCrc(header);
  const bool written = orderOutput_.seekSet(0) && exactWrite(orderOutput_, &header, sizeof(header)) &&
                       orderOutput_.sync() && orderOutput_.close() && orderInput_.close() &&
                       validateOrder(ORDER_TEMP_PATH, nullptr) &&
                       StagedFileTransaction::publish(ORDER_PATH, ORDER_TEMP_PATH, ORDER_BACKUP_PATH, validateOrder) ==
                           StagedFileTransaction::Status::Published;
  resetOrderBuild(true);
  return written;
}

bool LibraryCatalogStore::loadOrderedIndices(const uint8_t sortMode, const LibraryBookFormat excluded,
                                             std::vector<size_t>& indices) {
  indices.clear();
  if (!ensureOrder(sortMode)) return false;
  HalFile order;
  HalFile file;
  OrderHeader header{};
  if (!Storage.openFileForRead("LIB", ORDER_PATH, order) || !exactRead(order, &header, sizeof(header)) ||
      !validOrderHeader(header) || header.generation != generation_ || header.count != count_ ||
      header.sortMode != sortMode || !Storage.openFileForRead("LIB", activePath(), file)) {
    order.close();
    file.close();
    return false;
  }
  indices.reserve(header.count);
  for (uint32_t position = 0; position < header.count; ++position) {
    uint16_t index = 0;
    if (!exactRead(order, &index, sizeof(index))) {
      order.close();
      file.close();
      indices.clear();
      return false;
    }
    LibraryBookRecord record;
    if (!readRecordAt(file, index, record)) {
      order.close();
      file.close();
      indices.clear();
      return false;
    }
    if (record.format != excluded) indices.push_back(index);
  }

  const bool orderClosed = order.close();
  const bool fileClosed = file.close();
  if (!orderClosed || !fileClosed) {
    indices.clear();
    return false;
  }
  return true;
}

LibraryCatalogStore::FindPathResult LibraryCatalogStore::findPath(const std::string& path, const size_t preferredIndex,
                                                                  size_t& foundIndex) const {
  if (path.empty() || count_ == 0) return FindPathResult::NotFound;
  HalFile file;
  if (!Storage.openFileForRead("LIB", activePath(), file)) return FindPathResult::IoError;

  const auto matches = [&file, &path](const size_t index, bool& match) {
    LibraryBookRecord record;
    if (!readRecordAt(file, index, record)) return false;
    match = record.path == path;
    return true;
  };

  if (preferredIndex < count_) {
    bool match = false;
    if (!matches(preferredIndex, match)) {
      file.close();
      return FindPathResult::IoError;
    }
    if (match) {
      if (!file.close()) return FindPathResult::IoError;
      foundIndex = preferredIndex;
      return FindPathResult::Found;
    }
  }

  for (size_t index = 0; index < count_; ++index) {
    if (index == preferredIndex) continue;
    bool match = false;
    if (!matches(index, match)) {
      file.close();
      return FindPathResult::IoError;
    }
    if (match) {
      if (!file.close()) return FindPathResult::IoError;
      foundIndex = index;
      return FindPathResult::Found;
    }
  }
  return file.close() ? FindPathResult::NotFound : FindPathResult::IoError;
}

bool LibraryCatalogStore::loadPage(const size_t start, const size_t requested,
                                   std::vector<LibraryBookRecord>& records) const {
  records.clear();
  if (start >= count_ || requested == 0) return true;
  const size_t amount = std::min(requested, static_cast<size_t>(count_) - start);
  HalFile file;
  if (!Storage.openFileForRead("LIB", activePath(), file)) return false;
  records.reserve(amount);
  for (size_t i = 0; i < amount; ++i) {
    LibraryBookRecord record;
    if (!readRecordAt(file, start + i, record)) {
      file.close();
      records.clear();
      return false;
    }
    records.push_back(std::move(record));
  }
  if (!file.close()) {
    records.clear();
    return false;
  }
  return true;
}

bool LibraryCatalogStore::loadRecords(const std::vector<size_t>& indices,
                                      std::vector<LibraryBookRecord>& records) const {
  records.clear();
  if (indices.empty()) return true;

  HalFile file;
  if (!Storage.openFileForRead("LIB", activePath(), file)) return false;
  records.reserve(indices.size());
  for (const size_t index : indices) {
    LibraryBookRecord record;
    if (index >= count_ || !readRecordAt(file, index, record)) {
      file.close();
      records.clear();
      return false;
    }
    records.push_back(std::move(record));
  }
  if (!file.close()) {
    records.clear();
    return false;
  }
  return true;
}

bool LibraryCatalogStore::findPathIndices(const std::vector<std::string>& paths, std::vector<size_t>& indices) const {
  indices.assign(paths.size(), static_cast<size_t>(-1));
  if (paths.empty()) return true;

  HalFile file;
  if (!Storage.openFileForRead("LIB", activePath(), file)) return false;
  size_t remaining = paths.size();
  for (size_t index = 0; index < count_ && remaining > 0; ++index) {
    DiskRecord disk{};
    if (!file.seek64(sizeof(DiskHeader) + static_cast<uint64_t>(index) * sizeof(DiskRecord)) ||
        !exactRead(file, &disk, sizeof(disk)) || !validDiskRecord(disk)) {
      file.close();
      indices.clear();
      return false;
    }
    for (size_t pathIndex = 0; pathIndex < paths.size(); ++pathIndex) {
      if (indices[pathIndex] == static_cast<size_t>(-1) && paths[pathIndex] == disk.path) {
        indices[pathIndex] = index;
        --remaining;
        break;
      }
    }
  }
  const bool closed = file.close();
  if (!closed) {
    indices.clear();
    return false;
  }
  return true;
}
