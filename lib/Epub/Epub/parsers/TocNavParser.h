#pragma once
#include <Print.h>
#include <expat.h>

#include <string>

class BookMetadataCache;

// Parser for EPUB 3 nav.xhtml navigation documents
// Parses HTML5 nav elements with epub:type="toc" to extract table of contents
class TocNavParser final : public Print {
 public:
  static constexpr size_t MAX_ENTRY_TEXT_BYTES = 4096;
  static constexpr size_t MAX_TOC_ENTRIES = 2048;
  static constexpr uint8_t MAX_TOC_DEPTH = 32;

 private:
  enum ParserState {
    START,
    IN_HTML,
    IN_BODY,
    IN_NAV_TOC,  // Inside <nav epub:type="toc">
    IN_OL,       // Inside <ol>
    IN_LI,       // Inside <li>
    IN_ANCHOR,   // Inside <a>
  };

  const std::string& baseContentPath;
  size_t remainingSize;
  XML_Parser parser = nullptr;
  ParserState state = START;
  BookMetadataCache* cache;

  // Track nesting depth for <ol> elements to determine TOC depth
  uint8_t olDepth = 0;
  // Current entry data being collected
  std::string currentLabel;
  std::string currentHref;
  size_t entryCount = 0;
  bool failed = false;

  static void startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void characterData(void* userData, const XML_Char* s, int len);
  static void endElement(void* userData, const XML_Char* name);

 public:
  explicit TocNavParser(const std::string& baseContentPath, const size_t xmlSize, BookMetadataCache* cache)
      : baseContentPath(baseContentPath), remainingSize(xmlSize), cache(cache) {}
  ~TocNavParser() override;

  bool setup();
  bool succeeded() const { return !failed && parser != nullptr && remainingSize == 0; }
  size_t usableEntryCount() const { return entryCount; }

  size_t write(uint8_t) override;
  size_t write(const uint8_t* buffer, size_t size) override;
};
