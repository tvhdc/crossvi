#include "TocNavParser.h"

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

bool TocNavParser::setup() {
  parser = XML_ParserCreate(nullptr);
  if (!parser) {
    LOG_DBG("NAV", "Couldn't allocate memory for parser");
    return false;
  }

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);
  return true;
}

TocNavParser::~TocNavParser() { destroyXmlParser(parser); }

size_t TocNavParser::write(const uint8_t data) { return write(&data, 1); }

size_t TocNavParser::write(const uint8_t* buffer, const size_t size) {
  if (!parser || failed) return 0;

  const uint8_t* currentBufferPos = buffer;
  auto remainingInBuffer = size;

  while (remainingInBuffer > 0) {
    void* const buf = XML_GetBuffer(parser, 1024);
    if (!buf) {
      LOG_DBG("NAV", "Couldn't allocate memory for buffer");
      destroyXmlParser(parser);
      return 0;
    }

    const auto toRead = remainingInBuffer < 1024 ? remainingInBuffer : 1024;
    memcpy(buf, currentBufferPos, toRead);

    if (XML_ParseBuffer(parser, static_cast<int>(toRead), remainingSize == toRead) == XML_STATUS_ERROR) {
      LOG_DBG("NAV", "Parse error at line %lu: %s", XML_GetCurrentLineNumber(parser),
              XML_ErrorString(XML_GetErrorCode(parser)));
      destroyXmlParser(parser);
      return 0;
    }

    if (failed) {
      LOG_ERR("NAV", "Navigation document exceeds parser limits");
      destroyXmlParser(parser);
      return 0;
    }

    currentBufferPos += toRead;
    remainingInBuffer -= toRead;
    remainingSize -= toRead;
  }
  return size;
}

void XMLCALL TocNavParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<TocNavParser*>(userData);

  // Track HTML structure loosely - we mainly care about finding <nav epub:type="toc">
  if (strcmp(name, "html") == 0) {
    self->state = IN_HTML;
    return;
  }

  if (self->state == IN_HTML && strcmp(name, "body") == 0) {
    self->state = IN_BODY;
    return;
  }

  // Look for <nav epub:type="toc"> anywhere in body (or nested elements)
  if (self->state >= IN_BODY && strcmp(name, "nav") == 0) {
    for (int i = 0; atts[i]; i += 2) {
      if ((strcmp(atts[i], "epub:type") == 0 || strcmp(atts[i], "type") == 0) && strcmp(atts[i + 1], "toc") == 0) {
        self->state = IN_NAV_TOC;
        LOG_DBG("NAV", "Found nav toc element");
        return;
      }
    }
    return;
  }

  // Only process ol/li/a if we're inside the toc nav
  if (self->state < IN_NAV_TOC) {
    return;
  }

  if (strcmp(name, "ol") == 0) {
    if (self->olDepth >= MAX_TOC_DEPTH) {
      self->failed = true;
      return;
    }
    self->olDepth++;
    self->state = IN_OL;
    return;
  }

  if (self->state == IN_OL && strcmp(name, "li") == 0) {
    self->state = IN_LI;
    self->currentLabel.clear();
    self->currentHref.clear();
    return;
  }

  if (self->state == IN_LI && strcmp(name, "a") == 0) {
    self->state = IN_ANCHOR;
    // Get href attribute
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "href") == 0) {
        if (!assignBoundedAttribute(self->currentHref, atts[i + 1], MAX_ENTRY_TEXT_BYTES)) self->failed = true;
        break;
      }
    }
    return;
  }
}

void XMLCALL TocNavParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<TocNavParser*>(userData);

  // Only collect text when inside an anchor within the TOC nav
  if (self->state == IN_ANCHOR) {
    if (len < 0 || static_cast<size_t>(len) > MAX_ENTRY_TEXT_BYTES - std::min(self->currentLabel.size(), MAX_ENTRY_TEXT_BYTES)) {
      self->failed = true;
      return;
    }
    self->currentLabel.append(s, static_cast<size_t>(len));
  }
}

void XMLCALL TocNavParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<TocNavParser*>(userData);

  if (strcmp(name, "a") == 0 && self->state == IN_ANCHOR) {
    // Create TOC entry when closing anchor tag (we have all data now)
    if (!self->currentLabel.empty() && !self->currentHref.empty()) {
      if (self->entryCount >= MAX_TOC_ENTRIES ||
          self->baseContentPath.size() > MAX_ENTRY_TEXT_BYTES - self->currentHref.size()) {
        self->failed = true;
        return;
      }
      const std::string rawTarget = self->baseContentPath + self->currentHref;
      const size_t pos = rawTarget.find('#');
      const std::string rawPath = pos == std::string::npos ? rawTarget : rawTarget.substr(0, pos);
      std::string href = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(rawPath));
      std::string anchor;

      if (pos != std::string::npos) {
        anchor = FsHelpers::decodeUriEscapes(rawTarget.substr(pos + 1));
      }

      if (self->cache) {
        // olDepth gives us the nesting level (1-based from the outer ol)
        self->cache->createTocEntry(self->currentLabel, href, anchor, self->olDepth);
      }
      ++self->entryCount;

      self->currentLabel.clear();
      self->currentHref.clear();
    }
    self->state = IN_LI;
    return;
  }

  if (strcmp(name, "li") == 0 && (self->state == IN_LI || self->state == IN_OL)) {
    self->state = IN_OL;
    return;
  }

  if (strcmp(name, "ol") == 0 && self->state >= IN_NAV_TOC) {
    if (self->olDepth == 0) {
      self->failed = true;
      return;
    }
    self->olDepth--;
    if (self->olDepth == 0) {
      self->state = IN_NAV_TOC;
    } else {
      self->state = IN_LI;  // Back to parent li
    }
    return;
  }

  if (strcmp(name, "nav") == 0 && self->state >= IN_NAV_TOC) {
    self->state = IN_BODY;
    LOG_DBG("NAV", "Finished parsing nav toc");
    return;
  }
}
