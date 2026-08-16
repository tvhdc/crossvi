#include <ArduinoJson.h>
#include <AtomicFile.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <string>
#include <string_view>

#include "BookmarkEntry.h"
#include "JsonSettingsIO.h"
#include "clippings/ClippingCodec.h"

namespace {

constexpr size_t MAX_BOOKMARK_COUNT = 1024;

bool validBookmarkBookType(const std::string_view value) {
  return !value.empty() && value.size() <= ClippingCodec::MAX_BOOK_TYPE_BYTES &&
         std::all_of(value.begin(), value.end(), [](const unsigned char value) {
           return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9');
         });
}

bool validBookmarkMetadata(const BookmarkBookMetadata& metadata) {
  return !metadata.path.empty() && metadata.path.size() <= ClippingCodec::MAX_BOOK_PATH_BYTES &&
         metadata.title.size() <= ClippingCodec::MAX_BOOK_TITLE_BYTES &&
         metadata.author.size() <= ClippingCodec::MAX_BOOK_AUTHOR_BYTES && validBookmarkBookType(metadata.bookType) &&
         ClippingCodec::isValidUtf8(metadata.path) && ClippingCodec::isValidUtf8(metadata.title) &&
         ClippingCodec::isValidUtf8(metadata.author);
}

bool parseBookmarks(const uint8_t* data, const size_t size, std::vector<BookmarkEntry>* destination,
                    BookmarkBookMetadata* metadataDestination = nullptr) {
  JsonDocument doc;
  const auto error = deserializeJson(doc, data, size);
  if (error || !doc["bookmarks"].is<JsonArray>()) return false;

  BookmarkBookMetadata parsedMetadata;
  if (!doc["book"].isNull()) {
    if (!doc["book"].is<JsonObject>()) return false;
    const JsonObjectConst book = doc["book"].as<JsonObjectConst>();
    if (!book["path"].is<const char*>() || !book["title"].is<const char*>() || !book["author"].is<const char*>() ||
        !book["type"].is<const char*>()) {
      return false;
    }
    parsedMetadata.path = book["path"].as<std::string>();
    parsedMetadata.title = book["title"].as<std::string>();
    parsedMetadata.author = book["author"].as<std::string>();
    parsedMetadata.bookType = book["type"].as<std::string>();
    if (!validBookmarkMetadata(parsedMetadata)) return false;
  }

  JsonArray arr = doc["bookmarks"].as<JsonArray>();
  if (arr.size() > MAX_BOOKMARK_COUNT) return false;
  std::vector<BookmarkEntry> parsed;
  if (destination) parsed.reserve(arr.size());
  for (JsonVariant entry : arr) {
    if (!entry.is<JsonObject>()) return false;
    JsonObject obj = entry.as<JsonObject>();
    if (!obj["xpath"].is<const char*>() || !obj["summary"].is<const char*>()) return false;
    const char* positionKind = obj["positionKind"] | "epub";
    const bool textPosition = std::strcmp(positionKind, "text") == 0;
    const bool fixedPosition = std::strcmp(positionKind, "fixed") == 0;
    if (!textPosition && !fixedPosition && std::strcmp(positionKind, "epub") != 0) return false;
    if (textPosition && !obj["byteOffset"].is<uint32_t>()) return false;
    if (fixedPosition && !obj["pageIndex"].is<uint32_t>()) return false;
    const bool hasContentSourceOffset = !obj["sourceOffset"].isNull();
    if (hasContentSourceOffset && (textPosition || fixedPosition || !obj["sourceOffset"].is<uint32_t>())) return false;
    const float percentage = obj["percentage"] | static_cast<float>(-1);
    if (!std::isfinite(percentage) || percentage < 0.0f || percentage > 1.0f) return false;
    if (!destination) continue;
    parsed.emplace_back();
    auto& bookmark = parsed.back();
    bookmark.xpath = obj["xpath"] | std::string("");
    bookmark.percentage = percentage;
    bookmark.summary = obj["summary"] | std::string("");
    bookmark.computedSpineIndex = obj["si"] | static_cast<uint16_t>(0);
    bookmark.computedChapterPageCount = obj["pc"] | static_cast<uint16_t>(0);
    bookmark.computedChapterProgress = obj["pp"] | static_cast<uint16_t>(0);
    bookmark.hasContentSourceOffset = hasContentSourceOffset;
    bookmark.contentSourceOffset = hasContentSourceOffset ? obj["sourceOffset"].as<uint32_t>() : 0;
    bookmark.positionKind = textPosition    ? BookmarkEntry::PositionKind::Text
                            : fixedPosition ? BookmarkEntry::PositionKind::FixedLayout
                                            : BookmarkEntry::PositionKind::Epub;
    bookmark.byteOffset = textPosition ? obj["byteOffset"].as<uint32_t>() : 0;
    bookmark.pageIndex = fixedPosition ? obj["pageIndex"].as<uint32_t>() : 0;
  }
  if (destination) destination->swap(parsed);
  if (metadataDestination) *metadataDestination = std::move(parsedMetadata);
  return true;
}

struct BookmarkParseContext {
  std::vector<BookmarkEntry>* bookmarks = nullptr;
  BookmarkBookMetadata* metadata = nullptr;
};

bool validateBookmarkJson(const uint8_t* data, const size_t size, void* context) {
  auto* parseContext = static_cast<BookmarkParseContext*>(context);
  return parseBookmarks(data, size, parseContext ? parseContext->bookmarks : nullptr,
                        parseContext ? parseContext->metadata : nullptr);
}

bool equalBookmark(const BookmarkEntry& left, const BookmarkEntry& right) {
  return left.xpath == right.xpath && left.summary == right.summary &&
         std::fabs(left.percentage - right.percentage) <= 0.000001f &&
         left.computedSpineIndex == right.computedSpineIndex &&
         left.computedChapterPageCount == right.computedChapterPageCount &&
         left.computedChapterProgress == right.computedChapterProgress &&
         left.hasContentSourceOffset == right.hasContentSourceOffset &&
         (!left.hasContentSourceOffset || left.contentSourceOffset == right.contentSourceOffset) &&
         left.positionKind == right.positionKind && left.byteOffset == right.byteOffset &&
         left.pageIndex == right.pageIndex;
}

bool equalBookmarks(const std::vector<BookmarkEntry>& left, const std::vector<BookmarkEntry>& right) {
  return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(),
                                                   [](const BookmarkEntry& first, const BookmarkEntry& second) {
                                                     return equalBookmark(first, second);
                                                   });
}

}  // namespace

bool JsonSettingsIO::saveBookmarks(const std::vector<BookmarkEntry>& bookmarks, const char* path,
                                   const BookmarkBookMetadata* metadata) {
  if (bookmarks.size() > MAX_BOOKMARK_COUNT) return false;
  JsonDocument doc;
  if (metadata && validBookmarkMetadata(*metadata)) {
    JsonObject book = doc["book"].to<JsonObject>();
    book["path"] = metadata->path;
    book["title"] = metadata->title;
    book["author"] = metadata->author;
    book["type"] = metadata->bookType;
  } else if (metadata) {
    LOG_ERR("BKM", "Optional bookmark catalog metadata is invalid; saving bookmarks without it");
  }
  JsonArray arr = doc["bookmarks"].to<JsonArray>();
  LOG_DBG("BKM", "Saving %zu bookmarks to file", bookmarks.size());
  for (const auto& bookmark : bookmarks) {
    JsonObject obj = arr.add<JsonObject>();
    obj["xpath"] = bookmark.xpath;
    obj["percentage"] = bookmark.percentage;
    obj["summary"] = bookmark.summary;
    obj["si"] = bookmark.computedSpineIndex;
    obj["pc"] = bookmark.computedChapterPageCount;
    obj["pp"] = bookmark.computedChapterProgress;
    if (bookmark.positionKind == BookmarkEntry::PositionKind::Epub && bookmark.hasContentSourceOffset) {
      obj["sourceOffset"] = bookmark.contentSourceOffset;
    }
    if (bookmark.positionKind == BookmarkEntry::PositionKind::Text) {
      obj["positionKind"] = "text";
      obj["byteOffset"] = bookmark.byteOffset;
    } else if (bookmark.positionKind == BookmarkEntry::PositionKind::FixedLayout) {
      obj["positionKind"] = "fixed";
      obj["pageIndex"] = bookmark.pageIndex;
    }
  }

  String json;
  serializeJson(doc, json);
  const auto saved = AtomicFile::save(path, reinterpret_cast<const uint8_t*>(json.c_str()), json.length(),
                                      BOOKMARK_FILE_MAX_BYTES, validateBookmarkJson);
  return saved == AtomicFile::SaveStatus::Saved || saved == AtomicFile::SaveStatus::Unchanged;
}

bool JsonSettingsIO::loadBookmarks(std::vector<BookmarkEntry>& bookmarks, const char* json,
                                   BookmarkBookMetadata* metadata) {
  if (!json || !parseBookmarks(reinterpret_cast<const uint8_t*>(json), strlen(json), &bookmarks, metadata)) {
    LOG_ERR("BKM", "Bookmark JSON validation failed");
    return false;
  }
  LOG_DBG("BKM", "Loaded %zu bookmarks from file", bookmarks.size());
  return true;
}

JsonSettingsIO::BookmarkLoadStatus JsonSettingsIO::loadBookmarksFromFile(std::vector<BookmarkEntry>& bookmarks,
                                                                         const char* path,
                                                                         BookmarkBookMetadata* metadata) {
  if (metadata) *metadata = {};
  std::string json;
  BookmarkParseContext parseContext{&bookmarks, metadata};
  const AtomicFile::LoadStatus loaded =
      AtomicFile::load(path, json, BOOKMARK_FILE_MAX_BYTES, validateBookmarkJson, &parseContext);
  switch (loaded) {
    case AtomicFile::LoadStatus::Primary:
    case AtomicFile::LoadStatus::Backup:
    case AtomicFile::LoadStatus::Temp:
      LOG_DBG("BKM", "Loaded %zu bookmarks from file", bookmarks.size());
      return BookmarkLoadStatus::Loaded;
    case AtomicFile::LoadStatus::Missing:
      return BookmarkLoadStatus::Missing;
    case AtomicFile::LoadStatus::Oversize:
      return BookmarkLoadStatus::Oversize;
    case AtomicFile::LoadStatus::IoError:
      return BookmarkLoadStatus::IoError;
    case AtomicFile::LoadStatus::Invalid:
    default:
      return BookmarkLoadStatus::Invalid;
  }
}

JsonSettingsIO::BookmarkPathMoveStatus JsonSettingsIO::classifyBookmarkMetadataForPathMove(
    const char* sourcePath, const std::string& sourceBookPath) {
  std::vector<BookmarkEntry> bookmarks;
  BookmarkBookMetadata metadata;
  switch (loadBookmarksFromFile(bookmarks, sourcePath, &metadata)) {
    case BookmarkLoadStatus::Loaded:
      if (metadata.path.empty()) return BookmarkPathMoveStatus::Unchanged;
      if (metadata.path != sourceBookPath) return BookmarkPathMoveStatus::Conflict;
      return BookmarkPathMoveStatus::Rewritten;
    case BookmarkLoadStatus::Invalid:
    case BookmarkLoadStatus::Oversize:
      return BookmarkPathMoveStatus::Unchanged;
    case BookmarkLoadStatus::Missing:
      return BookmarkPathMoveStatus::Conflict;
    case BookmarkLoadStatus::IoError:
    default:
      return BookmarkPathMoveStatus::IoError;
  }
}

bool JsonSettingsIO::resolveBookmarkDocumentForPathMove(const char* sourcePath, std::string& resolvedPath) {
  resolvedPath.clear();
  if (!sourcePath) return false;
  std::string json;
  switch (AtomicFile::load(sourcePath, json, BOOKMARK_FILE_MAX_BYTES, validateBookmarkJson)) {
    case AtomicFile::LoadStatus::Primary:
      resolvedPath = sourcePath;
      return true;
    case AtomicFile::LoadStatus::Backup:
      resolvedPath = std::string(sourcePath) + ".bak";
      return true;
    case AtomicFile::LoadStatus::Temp:
      resolvedPath = std::string(sourcePath) + ".tmp";
      return true;
    case AtomicFile::LoadStatus::Invalid:
    case AtomicFile::LoadStatus::Oversize:
      // Preserve a malformed/future opaque payload during a path move. It is
      // not interpreted or rewritten, and later loaders will still fail
      // closed instead of silently dropping user state.
      for (const char* suffix : {"", ".bak", ".tmp"}) {
        const std::string candidate = std::string(sourcePath) + suffix;
        if (Storage.exists(candidate.c_str())) {
          resolvedPath = candidate;
          return true;
        }
      }
      return false;
    case AtomicFile::LoadStatus::Missing:
    case AtomicFile::LoadStatus::IoError:
    default:
      return false;
  }
}

JsonSettingsIO::BookmarkPathMoveStatus JsonSettingsIO::stageBookmarkMetadataForPathMove(
    const char* sourcePath, const char* stagedPath, const std::string& sourceBookPath,
    const std::string& destinationBookPath) {
  const BookmarkPathMoveStatus status = classifyBookmarkMetadataForPathMove(sourcePath, sourceBookPath);
  if (status != BookmarkPathMoveStatus::Rewritten) return status;

  std::vector<BookmarkEntry> bookmarks;
  BookmarkBookMetadata metadata;
  if (loadBookmarksFromFile(bookmarks, sourcePath, &metadata) != BookmarkLoadStatus::Loaded) {
    return BookmarkPathMoveStatus::IoError;
  }
  metadata.path = destinationBookPath;
  return saveBookmarks(bookmarks, stagedPath, &metadata) ? BookmarkPathMoveStatus::Rewritten
                                                         : BookmarkPathMoveStatus::IoError;
}

bool JsonSettingsIO::validateBookmarkPathMove(const char* sourcePath, const char* movedPath,
                                              const std::string& sourceBookPath,
                                              const std::string& destinationBookPath) {
  std::vector<BookmarkEntry> sourceBookmarks;
  BookmarkBookMetadata sourceMetadata;
  const BookmarkLoadStatus sourceStatus = loadBookmarksFromFile(sourceBookmarks, sourcePath, &sourceMetadata);
  if (sourceStatus == BookmarkLoadStatus::Invalid || sourceStatus == BookmarkLoadStatus::Oversize) return false;
  if (sourceStatus != BookmarkLoadStatus::Loaded || sourceMetadata.path.empty() ||
      sourceMetadata.path != sourceBookPath) {
    return false;
  }

  std::vector<BookmarkEntry> movedBookmarks;
  BookmarkBookMetadata movedMetadata;
  if (loadBookmarksFromFile(movedBookmarks, movedPath, &movedMetadata) != BookmarkLoadStatus::Loaded) return false;
  return movedMetadata.path == destinationBookPath && movedMetadata.title == sourceMetadata.title &&
         movedMetadata.author == sourceMetadata.author && movedMetadata.bookType == sourceMetadata.bookType &&
         equalBookmarks(sourceBookmarks, movedBookmarks);
}
