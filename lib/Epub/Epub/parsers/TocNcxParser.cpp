#include "TocNcxParser.h"

#include <FsHelpers.h>
#include <Logging.h>
#include <XmlParserUtils.h>

#include "Epub/BookMetadataCache.h"

namespace {
bool assignBoundedAttribute(std::string& destination, const char* value, const size_t maxBytes) {
  size_t length = 0;
  while (length <= maxBytes && value[length] != '\0') ++length;
  if (length > maxBytes) return false;
  destination.assign(value, length);
  return true;
}
}  // namespace

bool TocNcxParser::setup() {
  parser = XML_ParserCreate(nullptr);
  if (!parser) {
    LOG_DBG("TOC", "Couldn't allocate memory for parser");
    return false;
  }

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);
  return true;
}

TocNcxParser::~TocNcxParser() { destroyXmlParser(parser); }

size_t TocNcxParser::write(const uint8_t data) { return write(&data, 1); }

size_t TocNcxParser::write(const uint8_t* buffer, const size_t size) {
  if (!parser || failed) return 0;

  const uint8_t* currentBufferPos = buffer;
  auto remainingInBuffer = size;

  while (remainingInBuffer > 0) {
    void* const buf = XML_GetBuffer(parser, 1024);
    if (!buf) {
      LOG_DBG("TOC", "Couldn't allocate memory for buffer");
      destroyXmlParser(parser);
      return 0;
    }

    const auto toRead = remainingInBuffer < 1024 ? remainingInBuffer : 1024;
    memcpy(buf, currentBufferPos, toRead);

    if (XML_ParseBuffer(parser, static_cast<int>(toRead), remainingSize == toRead) == XML_STATUS_ERROR) {
      LOG_DBG("TOC", "Parse error at line %lu: %s", XML_GetCurrentLineNumber(parser),
              XML_ErrorString(XML_GetErrorCode(parser)));
      destroyXmlParser(parser);
      return 0;
    }

    if (failed) {
      LOG_ERR("TOC", "NCX document exceeds parser limits");
      destroyXmlParser(parser);
      return 0;
    }

    currentBufferPos += toRead;
    remainingInBuffer -= toRead;
    remainingSize -= toRead;
  }
  return size;
}

void XMLCALL TocNcxParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  // NOTE: We rely on navPoint label and content coming before any nested navPoints, this will be fine:
  // <navPoint>
  //   <navLabel><text>Chapter 1</text></navLabel>
  //   <content src="ch1.html"/>
  //   <navPoint> ...nested... </navPoint>
  // </navPoint>
  //
  // This will NOT:
  // <navPoint>
  //   <navPoint> ...nested... </navPoint>
  //   <navLabel><text>Chapter 1</text></navLabel>
  //   <content src="ch1.html"/>
  // </navPoint>

  auto* self = static_cast<TocNcxParser*>(userData);

  if (self->state == START && strcmp(name, "ncx") == 0) {
    self->state = IN_NCX;
    return;
  }

  if (self->state == IN_NCX && strcmp(name, "navMap") == 0) {
    self->state = IN_NAV_MAP;
    return;
  }

  // Handles both top-level and nested navPoints
  if ((self->state == IN_NAV_MAP || self->state == IN_NAV_POINT) && strcmp(name, "navPoint") == 0) {
    if (self->currentDepth >= MAX_TOC_DEPTH) {
      self->failed = true;
      return;
    }
    self->state = IN_NAV_POINT;
    self->currentDepth++;

    self->currentLabel.clear();
    self->currentSrc.clear();
    return;
  }

  if (self->state == IN_NAV_POINT && strcmp(name, "navLabel") == 0) {
    self->state = IN_NAV_LABEL;
    return;
  }

  if (self->state == IN_NAV_LABEL && strcmp(name, "text") == 0) {
    self->state = IN_NAV_LABEL_TEXT;
    return;
  }

  if (self->state == IN_NAV_POINT && strcmp(name, "content") == 0) {
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "src") == 0) {
        if (!assignBoundedAttribute(self->currentSrc, atts[i + 1], MAX_ENTRY_TEXT_BYTES)) self->failed = true;
        break;
      }
    }
    return;
  }
}

void XMLCALL TocNcxParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<TocNcxParser*>(userData);
  if (self->state == IN_NAV_LABEL_TEXT) {
    if (len < 0 || static_cast<size_t>(len) > MAX_ENTRY_TEXT_BYTES ||
        self->currentLabel.size() > MAX_ENTRY_TEXT_BYTES - static_cast<size_t>(len)) {
      self->failed = true;
      return;
    }
    self->currentLabel.append(s, static_cast<size_t>(len));
  }
}

void XMLCALL TocNcxParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<TocNcxParser*>(userData);

  if (self->state == IN_NAV_LABEL_TEXT && strcmp(name, "text") == 0) {
    self->state = IN_NAV_LABEL;
    return;
  }

  if (self->state == IN_NAV_LABEL && strcmp(name, "navLabel") == 0) {
    self->state = IN_NAV_POINT;
    return;
  }

  if (self->state == IN_NAV_POINT && strcmp(name, "navPoint") == 0) {
    if (self->currentDepth == 0) {
      self->failed = true;
      return;
    }
    self->currentDepth--;
    if (self->currentDepth == 0) {
      self->state = IN_NAV_MAP;
    }
    return;
  }

  if (self->state == IN_NAV_POINT && strcmp(name, "content") == 0) {
    // At this point (end of content tag), we likely have both Label (from previous tags) and Src.
    // This is the safest place to push the data, assuming <navLabel> always comes before <content>.
    // NCX spec says navLabel comes before content.
    if (!self->currentLabel.empty() && !self->currentSrc.empty()) {
      if (self->entryCount >= MAX_TOC_ENTRIES ||
          self->baseContentPath.size() > MAX_ENTRY_TEXT_BYTES - self->currentSrc.size()) {
        self->failed = true;
        return;
      }
      const std::string rawTarget = self->baseContentPath + self->currentSrc;
      const size_t pos = rawTarget.find('#');
      const std::string rawPath = pos == std::string::npos ? rawTarget : rawTarget.substr(0, pos);
      std::string href = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(rawPath));
      std::string anchor;

      if (pos != std::string::npos) {
        anchor = FsHelpers::decodeUriEscapes(rawTarget.substr(pos + 1));
      }

      if (self->cache) {
        self->cache->createTocEntry(self->currentLabel, href, anchor, self->currentDepth);
      }
      ++self->entryCount;

      // Clear them so we don't re-add them if there are weird XML structures
      self->currentLabel.clear();
      self->currentSrc.clear();
    }
  }
}
