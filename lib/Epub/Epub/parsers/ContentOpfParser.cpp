#include "ContentOpfParser.h"

#include <FsHelpers.h>
#include <Logging.h>
#include <XmlParserUtils.h>

#include <cctype>
#include <limits>
#include <string_view>

#include "Epub/BookMetadataCache.h"

namespace {
constexpr char MEDIA_TYPE_NCX[] = "application/x-dtbncx+xml";
constexpr char MEDIA_TYPE_CSS[] = "text/css";
constexpr char MEDIA_TYPE_IMAGE_PREFIX[] = "image/";
constexpr char itemCacheFile[] = "/.items.bin";

bool startsWithImageMediaType(const std::string& mediaType) {
  constexpr size_t prefixLen = sizeof(MEDIA_TYPE_IMAGE_PREFIX) - 1;
  if (mediaType.size() < prefixLen) {
    return false;
  }

  for (size_t i = 0; i < prefixLen; ++i) {
    const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(mediaType[i])));
    if (c != MEDIA_TYPE_IMAGE_PREFIX[i]) {
      return false;
    }
  }

  return true;
}

std::string trimAsciiWhitespace(const std::string& value) {
  const size_t first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const size_t last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

bool hasWhitespaceSeparatedToken(const std::string& value, const std::string_view token) {
  constexpr std::string_view whitespace = " \t\r\n";
  size_t start = value.find_first_not_of(whitespace);
  while (start != std::string::npos) {
    const size_t end = value.find_first_of(whitespace, start);
    const size_t length = (end == std::string::npos ? value.size() : end) - start;
    if (length == token.size() && value.compare(start, length, token) == 0) return true;
    start = value.find_first_not_of(whitespace, end);
  }
  return false;
}

bool assignBoundedAttribute(std::string& destination, const char* value, const size_t maxBytes) {
  size_t length = 0;
  while (length <= maxBytes && value[length] != '\0') ++length;
  if (length > maxBytes) return false;
  destination.assign(value, length);
  return true;
}

bool appendBoundedText(std::string& destination, const char* value, const int length, const size_t maxBytes) {
  if (length < 0 || static_cast<size_t>(length) > maxBytes - std::min(destination.size(), maxBytes)) return false;
  destination.append(value, static_cast<size_t>(length));
  return true;
}
}  // namespace

bool ContentOpfParser::setup() {
  parser = XML_ParserCreate(nullptr);
  if (!parser) {
    LOG_DBG("COF", "Couldn't allocate memory for parser");
    return false;
  }

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);
  return true;
}

bool ContentOpfParser::writeItemRecord(const std::string& itemId, const std::string& href) {
  constexpr size_t MAX_ITEM_STRING_LENGTH = std::numeric_limits<uint16_t>::max();
  if (!tempItemStore || itemId.size() > MAX_ITEM_STRING_LENGTH || href.size() > MAX_ITEM_STRING_LENGTH ||
      tempItemStore.position() > std::numeric_limits<uint32_t>::max()) {
    return false;
  }

  const uint32_t itemIdLength = static_cast<uint32_t>(itemId.size());
  const uint32_t hrefLength = static_cast<uint32_t>(href.size());
  const auto writeExact = [this](const void* data, const size_t size) {
    return tempItemStore.write(data, size) == size;
  };

  return writeExact(&itemIdLength, sizeof(itemIdLength)) && writeExact(itemId.data(), itemId.size()) &&
         writeExact(&hrefLength, sizeof(hrefLength)) && writeExact(href.data(), href.size());
}

bool ContentOpfParser::readItemRecord(const uint32_t offset, const uint16_t expectedIdLength, std::string& itemId,
                                      std::string& href) {
  constexpr uint32_t MAX_ITEM_STRING_LENGTH = std::numeric_limits<uint16_t>::max();
  if (!tempItemStore || !tempItemStore.seek(offset)) return false;

  const auto readExact = [this](void* data, const size_t size) {
    const int available = tempItemStore.available();
    return available >= 0 && size <= static_cast<size_t>(available) &&
           tempItemStore.read(data, size) == static_cast<int>(size);
  };
  uint32_t itemIdLength = 0;
  if (!readExact(&itemIdLength, sizeof(itemIdLength)) || itemIdLength != expectedIdLength ||
      itemIdLength > MAX_ITEM_STRING_LENGTH) {
    return false;
  }
  itemId.resize(itemIdLength);
  if (!readExact(itemId.data(), itemId.size())) return false;

  uint32_t hrefLength = 0;
  if (!readExact(&hrefLength, sizeof(hrefLength)) || hrefLength > MAX_ITEM_STRING_LENGTH) return false;
  href.resize(hrefLength);
  return readExact(href.data(), href.size());
}

bool ContentOpfParser::closeItemStore() {
  if (!tempItemStore) return true;
  return tempItemStore.close();
}

ContentOpfParser::~ContentOpfParser() {
  destroyXmlParser(parser);
  if (tempItemStore) {
    tempItemStore.close();
  }
  const auto itemCachePath = cachePath + itemCacheFile;
  if (Storage.exists(itemCachePath.c_str())) {
    Storage.remove(itemCachePath.c_str());
  }
}

size_t ContentOpfParser::write(const uint8_t data) { return write(&data, 1); }

size_t ContentOpfParser::write(const uint8_t* buffer, const size_t size) {
  if (!parser || ioFailed) return 0;

  const uint8_t* currentBufferPos = buffer;
  auto remainingInBuffer = size;

  while (remainingInBuffer > 0) {
    void* const buf = XML_GetBuffer(parser, 1024);

    if (!buf) {
      LOG_ERR("COF", "Couldn't allocate memory for buffer");
      destroyXmlParser(parser);
      return 0;
    }

    const auto toRead = remainingInBuffer < 1024 ? remainingInBuffer : 1024;
    memcpy(buf, currentBufferPos, toRead);

    if (XML_ParseBuffer(parser, static_cast<int>(toRead), remainingSize == toRead) == XML_STATUS_ERROR) {
      LOG_DBG("COF", "Parse error at line %lu: %s", XML_GetCurrentLineNumber(parser),
              XML_ErrorString(XML_GetErrorCode(parser)));
      destroyXmlParser(parser);
      return 0;
    }

    if (ioFailed) {
      LOG_ERR("COF", "Storage error while parsing content.opf");
      destroyXmlParser(parser);
      return 0;
    }

    currentBufferPos += toRead;
    remainingInBuffer -= toRead;
    remainingSize -= toRead;
  }

  return size;
}

void XMLCALL ContentOpfParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<ContentOpfParser*>(userData);
  (void)atts;

  if (self->ioFailed) return;

  if (self->state == START && (strcmp(name, "package") == 0 || strcmp(name, "opf:package") == 0)) {
    self->state = IN_PACKAGE;
    return;
  }

  if (self->state == IN_PACKAGE && (strcmp(name, "metadata") == 0 || strcmp(name, "opf:metadata") == 0)) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_METADATA && strcmp(name, "dc:title") == 0) {
    // Only capture the first dc:title element; subsequent ones are subtitles
    if (self->title.empty()) {
      self->state = IN_BOOK_TITLE;
    }
    return;
  }

  if (self->state == IN_METADATA && strcmp(name, "dc:creator") == 0) {
    self->currentAuthor.clear();
    self->state = IN_BOOK_AUTHOR;
    return;
  }

  if (self->state == IN_METADATA && strcmp(name, "dc:language") == 0) {
    self->currentLanguage.clear();
    self->state = IN_BOOK_LANGUAGE;
    return;
  }

  if (self->state == IN_PACKAGE && (strcmp(name, "manifest") == 0 || strcmp(name, "opf:manifest") == 0)) {
    self->state = IN_MANIFEST;
    if (self->cache && !Storage.openFileForWrite("COF", self->cachePath + itemCacheFile, self->tempItemStore)) {
      LOG_ERR("COF", "Couldn't open temp items file for writing");
      self->ioFailed = true;
    }
    return;
  }

  if (self->state == IN_PACKAGE && (strcmp(name, "spine") == 0 || strcmp(name, "opf:spine") == 0)) {
    self->state = IN_SPINE;
    if (self->cache && !Storage.openFileForRead("COF", self->cachePath + itemCacheFile, self->tempItemStore)) {
      LOG_ERR("COF", "Couldn't open temp items file for reading");
      self->ioFailed = true;
      return;
    }

    // Sort the (unconditionally-built) item index so every idref lookup uses binary
    // search. Without this, small/medium manifests fell back to an O(spine × manifest)
    // linear rescan of .items.bin per itemref (up to ~200ms/item at large scale).
    if (!self->itemIndex.empty()) {
      std::sort(self->itemIndex.begin(), self->itemIndex.end(), [](const ItemIndexEntry& a, const ItemIndexEntry& b) {
        return a.idHash < b.idHash || (a.idHash == b.idHash && a.idLen < b.idLen);
      });
      self->useItemIndex = true;
      LOG_DBG("COF", "Using fast index for %zu manifest items", self->itemIndex.size());
    }
    return;
  }

  if (self->state == IN_PACKAGE && (strcmp(name, "guide") == 0 || strcmp(name, "opf:guide") == 0)) {
    self->state = IN_GUIDE;
    return;
  }

  if (self->state == IN_METADATA && (strcmp(name, "meta") == 0 || strcmp(name, "opf:meta") == 0)) {
    bool isCover = false;
    std::string coverItemId;

    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "name") == 0 && strcmp(atts[i + 1], "cover") == 0) {
        isCover = true;
      } else if (strcmp(atts[i], "content") == 0) {
        if (!assignBoundedAttribute(coverItemId, atts[i + 1], MAX_ITEM_ID_BYTES)) {
          LOG_ERR("COF", "Cover item id exceeds limit");
          self->ioFailed = true;
          return;
        }
      }
    }

    if (isCover) {
      self->coverItemId = coverItemId;
    }
    return;
  }

  if (self->state == IN_MANIFEST && (strcmp(name, "item") == 0 || strcmp(name, "opf:item") == 0)) {
    if (++self->manifestItemCount > MAX_MANIFEST_ITEMS) {
      LOG_ERR("COF", "Manifest exceeds %u items", static_cast<unsigned>(MAX_MANIFEST_ITEMS));
      self->ioFailed = true;
      return;
    }
    std::string itemId;
    std::string rawHref;
    std::string mediaType;
    std::string properties;

    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "id") == 0) {
        if (!assignBoundedAttribute(itemId, atts[i + 1], MAX_ITEM_ID_BYTES)) self->ioFailed = true;
      } else if (strcmp(atts[i], "href") == 0) {
        if (!assignBoundedAttribute(rawHref, atts[i + 1], MAX_RESOURCE_PATH_BYTES)) self->ioFailed = true;
      } else if (strcmp(atts[i], "media-type") == 0) {
        if (!assignBoundedAttribute(mediaType, atts[i + 1], 256)) self->ioFailed = true;
      } else if (strcmp(atts[i], "properties") == 0) {
        if (!assignBoundedAttribute(properties, atts[i + 1], 256)) self->ioFailed = true;
      }
    }
    if (self->ioFailed || itemId.empty() || rawHref.empty() ||
        self->baseContentPath.size() > MAX_RESOURCE_PATH_BYTES - rawHref.size()) {
      LOG_ERR("COF", "Invalid or oversized manifest item");
      self->ioFailed = true;
      return;
    }
    const std::string href =
        FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(self->baseContentPath + rawHref));
    if (href.empty() || href.size() > MAX_RESOURCE_PATH_BYTES) {
      LOG_ERR("COF", "Normalized manifest path exceeds limit");
      self->ioFailed = true;
      return;
    }

    // The scratch item store is only needed while building book.bin. Warm
    // metadata/CSS discovery never writes derived data to the SD card.
    if (self->cache) {
      ItemIndexEntry entry;
      entry.idHash = fnvHash(itemId);
      entry.idLen = static_cast<uint16_t>(itemId.size());
      entry.fileOffset = static_cast<uint32_t>(self->tempItemStore.position());
      if (!self->writeItemRecord(itemId, href)) {
        LOG_ERR("COF", "Couldn't write temp manifest item");
        self->ioFailed = true;
        return;
      }
      self->itemIndex.push_back(entry);
    }

    if (itemId == self->coverItemId) {
      // Some EPUBs set meta name="cover" to an XHTML wrapper item.
      // Only treat it as a cover image when the manifest media-type is image/*.
      if (startsWithImageMediaType(mediaType)) {
        self->coverItemHref = href;
      } else {
        LOG_DBG("COF", "Ignoring meta cover item '%s' with non-image media type: %s", itemId.c_str(),
                mediaType.c_str());
      }
    }

    if (mediaType == MEDIA_TYPE_NCX) {
      if (self->tocNcxPath.empty()) {
        self->tocNcxPath = href;
      } else {
        LOG_DBG("COF", "Warning: Multiple NCX files found in manifest. Ignoring duplicate: %s", href.c_str());
      }
    }

    // Collect CSS files
    if (mediaType == MEDIA_TYPE_CSS) {
      if (self->cssFiles.size() >= MAX_CSS_FILES) {
        LOG_ERR("COF", "Manifest exceeds %u CSS files", static_cast<unsigned>(MAX_CSS_FILES));
        self->ioFailed = true;
        return;
      }
      self->cssFiles.push_back(href);
    }

    // EPUB 3: Check for nav document (properties contains "nav")
    if (!properties.empty() && self->tocNavPath.empty()) {
      if (hasWhitespaceSeparatedToken(properties, "nav")) {
        self->tocNavPath = href;
        LOG_DBG("COF", "Found EPUB 3 nav document: %s", href.c_str());
      }
    }

    // EPUB 3: Check for cover image (properties contains "cover-image")
    if (!properties.empty() && self->coverItemHref.empty()) {
      if (hasWhitespaceSeparatedToken(properties, "cover-image")) {
        self->coverItemHref = href;
      }
    }
    return;
  }

  // NOTE: This relies on spine appearing after item manifest (which is pretty safe as it's part of the EPUB spec)
  // Only run the spine parsing if there's a cache to add it to
  if (self->cache) {
    if (self->state == IN_SPINE && (strcmp(name, "itemref") == 0 || strcmp(name, "opf:itemref") == 0)) {
      if (++self->spineItemCount > MAX_SPINE_ITEMS) {
        LOG_ERR("COF", "Spine exceeds %u items", static_cast<unsigned>(MAX_SPINE_ITEMS));
        self->ioFailed = true;
        return;
      }
      std::string idref;
      bool linear = true;
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "idref") == 0) {
          if (!assignBoundedAttribute(idref, atts[i + 1], MAX_ITEM_ID_BYTES)) self->ioFailed = true;
        } else if (strcmp(atts[i], "linear") == 0 && strcmp(atts[i + 1], "no") == 0) {
          linear = false;
        }
      }
      if (self->ioFailed || idref.empty() || !self->useItemIndex) {
        LOG_ERR("COF", "Spine itemref is missing or invalid");
        self->ioFailed = true;
        return;
      }

      std::string href;
      bool found = false;
      const uint32_t targetHash = fnvHash(idref);
      const uint16_t targetLen = static_cast<uint16_t>(idref.size());
      auto it = std::lower_bound(self->itemIndex.begin(), self->itemIndex.end(),
                                 ItemIndexEntry{targetHash, targetLen, 0},
                                 [](const ItemIndexEntry& a, const ItemIndexEntry& b) {
                                   return a.idHash < b.idHash || (a.idHash == b.idHash && a.idLen < b.idLen);
                                 });
      while (it != self->itemIndex.end() && it->idHash == targetHash) {
        std::string itemId;
        std::string candidateHref;
        if (!self->readItemRecord(it->fileOffset, it->idLen, itemId, candidateHref)) {
          LOG_ERR("COF", "Couldn't read temp manifest item");
          self->ioFailed = true;
          return;
        }
        if (itemId == idref) {
          if (found) {
            LOG_ERR("COF", "Duplicate manifest id referenced by spine: %s", idref.c_str());
            self->ioFailed = true;
            return;
          }
          href = std::move(candidateHref);
          found = true;
        }
        ++it;
      }
      if (!found) {
        LOG_ERR("COF", "Unresolved spine idref: %s", idref.c_str());
        self->ioFailed = true;
        return;
      }
      self->cache->createSpineEntry(href, linear);
      return;
    }
  }
  // parse the guide
  if (self->state == IN_GUIDE && (strcmp(name, "reference") == 0 || strcmp(name, "opf:reference") == 0)) {
    std::string type;
    std::string guideHref;
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "type") == 0) {
        if (!assignBoundedAttribute(type, atts[i + 1], 64)) self->ioFailed = true;
      } else if (strcmp(atts[i], "href") == 0) {
        std::string rawHref;
        if (!assignBoundedAttribute(rawHref, atts[i + 1], MAX_RESOURCE_PATH_BYTES) ||
            self->baseContentPath.size() > MAX_RESOURCE_PATH_BYTES - rawHref.size()) {
          self->ioFailed = true;
        } else {
          guideHref = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(self->baseContentPath + rawHref));
        }
      }
    }
    if (self->ioFailed || guideHref.size() > MAX_RESOURCE_PATH_BYTES) return;
    if (!guideHref.empty()) {
      // EPUB 2's guide reference type "text" is ambiguous: publishers use it
      // for arbitrary front matter as well as the reading start. Only the
      // explicit "start" type is safe to treat as a resume/start target.
      if (type == "start" && !self->hasExplicitStartReference) {
        LOG_DBG("COF", "Found %s reference in guide: %s", type.c_str(), guideHref.c_str());
        self->textReferenceHref = guideHref;
        self->hasExplicitStartReference = true;
      } else if ((type == "cover" || type == "cover-page") && self->guideCoverPageHref.empty()) {
        LOG_DBG("COF", "Found cover reference in guide: %s", guideHref.c_str());
        self->guideCoverPageHref = guideHref;
      }
    }
    return;
  }
}

void XMLCALL ContentOpfParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<ContentOpfParser*>(userData);

  if (self->state == IN_BOOK_TITLE) {
    if (!appendBoundedText(self->title, s, len, MAX_METADATA_TEXT_BYTES)) self->ioFailed = true;
    return;
  }

  if (self->state == IN_BOOK_AUTHOR) {
    if (!appendBoundedText(self->currentAuthor, s, len, MAX_METADATA_TEXT_BYTES)) self->ioFailed = true;
    return;
  }

  if (self->state == IN_BOOK_LANGUAGE) {
    if (!appendBoundedText(self->currentLanguage, s, len, MAX_LANGUAGE_BYTES)) self->ioFailed = true;
    return;
  }
}

void XMLCALL ContentOpfParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<ContentOpfParser*>(userData);
  (void)name;

  if (self->ioFailed) return;

  if (self->state == IN_SPINE && (strcmp(name, "spine") == 0 || strcmp(name, "opf:spine") == 0)) {
    self->state = IN_PACKAGE;
    if (self->cache && !self->closeItemStore()) self->ioFailed = true;
    return;
  }

  if (self->state == IN_GUIDE && (strcmp(name, "guide") == 0 || strcmp(name, "opf:guide") == 0)) {
    self->state = IN_PACKAGE;
    return;
  }

  if (self->state == IN_MANIFEST && (strcmp(name, "manifest") == 0 || strcmp(name, "opf:manifest") == 0)) {
    self->state = IN_PACKAGE;
    if (self->cache && !self->closeItemStore()) self->ioFailed = true;
    return;
  }

  if (self->state == IN_BOOK_TITLE && strcmp(name, "dc:title") == 0) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_AUTHOR && strcmp(name, "dc:creator") == 0) {
    if (!self->currentAuthor.empty()) {
      const size_t separatorBytes = self->author.empty() ? 0 : 2;
      if (self->author.size() > MAX_METADATA_TEXT_BYTES - separatorBytes ||
          self->currentAuthor.size() > MAX_METADATA_TEXT_BYTES - separatorBytes - self->author.size()) {
        self->ioFailed = true;
        return;
      }
      if (!self->author.empty()) self->author.append(", ");
      self->author.append(self->currentAuthor);
    }
    self->currentAuthor.clear();
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_LANGUAGE && strcmp(name, "dc:language") == 0) {
    if (self->language.empty()) self->language = trimAsciiWhitespace(self->currentLanguage);
    self->currentLanguage.clear();
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_METADATA && (strcmp(name, "metadata") == 0 || strcmp(name, "opf:metadata") == 0)) {
    self->state = IN_PACKAGE;
    return;
  }

  if (self->state == IN_PACKAGE && (strcmp(name, "package") == 0 || strcmp(name, "opf:package") == 0)) {
    self->state = START;
    return;
  }
}
