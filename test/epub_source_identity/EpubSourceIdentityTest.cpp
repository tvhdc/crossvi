#include <Arduino.h>
#include <HalStorage.h>
#include <ZipFile.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "Epub.h"
#include "Epub/BookMetadataCache.h"
#include "Epub/SourceIdentityCodec.h"
#include "Epub/SourceIdentityStore.h"
#include "Epub/parsers/ContainerParser.h"
#include "Epub/parsers/ContentOpfParser.h"
#include "Epub/parsers/TocNavParser.h"
#include "Epub/parsers/TocNcxParser.h"
#include "ThumbnailConverterStub.h"

namespace {

constexpr char EPUB_PATH[] = "/books/test.epub";
constexpr char CACHE_PATH[] = "/.crosspoint/epub_test";
constexpr char BOOK_CACHE_PATH[] = "/.crosspoint/epub_test/book.bin";

template <typename T>
void appendPod(std::vector<uint8_t>& bytes, const T value) {
  const auto* raw = reinterpret_cast<const uint8_t*>(&value);
  bytes.insert(bytes.end(), raw, raw + sizeof(value));
}

template <typename T>
void overwritePod(std::vector<uint8_t>& bytes, const size_t offset, const T value) {
  ASSERT_LE(offset + sizeof(value), bytes.size());
  memcpy(bytes.data() + offset, &value, sizeof(value));
}

void appendString(std::vector<uint8_t>& bytes, const std::string& value) {
  appendPod(bytes, static_cast<uint32_t>(value.size()));
  bytes.insert(bytes.end(), value.begin(), value.end());
}

void put16(std::vector<uint8_t>& bytes, const size_t offset, const uint16_t value) {
  bytes[offset] = static_cast<uint8_t>(value);
  bytes[offset + 1] = static_cast<uint8_t>(value >> 8U);
}

void put32(std::vector<uint8_t>& bytes, const size_t offset, const uint32_t value) {
  bytes[offset] = static_cast<uint8_t>(value);
  bytes[offset + 1] = static_cast<uint8_t>(value >> 8U);
  bytes[offset + 2] = static_cast<uint8_t>(value >> 16U);
  bytes[offset + 3] = static_cast<uint8_t>(value >> 24U);
}

uint32_t get32(const std::vector<uint8_t>& bytes, const size_t offset) {
  return static_cast<uint32_t>(bytes[offset]) | (static_cast<uint32_t>(bytes[offset + 1]) << 8U) |
         (static_cast<uint32_t>(bytes[offset + 2]) << 16U) | (static_cast<uint32_t>(bytes[offset + 3]) << 24U);
}

std::vector<uint8_t> makeZip(const uint32_t entryCrc = 0x12345678U, const char nameByte = 'a') {
  std::vector<uint8_t> bytes(32, 0x5AU);
  const uint32_t centralOffset = static_cast<uint32_t>(bytes.size());
  constexpr uint16_t nameLength = 5;
  const size_t centralStart = bytes.size();
  bytes.resize(bytes.size() + 46 + nameLength, 0);
  put32(bytes, centralStart, 0x02014B50U);
  put32(bytes, centralStart + 16, entryCrc);
  put32(bytes, centralStart + 20, 12);
  put32(bytes, centralStart + 24, 20);
  put16(bytes, centralStart + 28, nameLength);
  bytes[centralStart + 46] = static_cast<uint8_t>(nameByte);
  bytes[centralStart + 47] = '.';
  bytes[centralStart + 48] = 'x';
  bytes[centralStart + 49] = 'h';
  bytes[centralStart + 50] = 't';
  const uint32_t centralSize = static_cast<uint32_t>(bytes.size() - centralStart);

  const size_t eocd = bytes.size();
  bytes.resize(bytes.size() + 22, 0);
  put32(bytes, eocd, 0x06054B50U);
  put16(bytes, eocd + 8, 1);
  put16(bytes, eocd + 10, 1);
  put32(bytes, eocd + 12, centralSize);
  put32(bytes, eocd + 16, centralOffset);
  return bytes;
}

void appendLe16(std::vector<uint8_t>& bytes, const uint16_t value) {
  bytes.push_back(static_cast<uint8_t>(value));
  bytes.push_back(static_cast<uint8_t>(value >> 8U));
}

void appendLe32(std::vector<uint8_t>& bytes, const uint32_t value) {
  bytes.push_back(static_cast<uint8_t>(value));
  bytes.push_back(static_cast<uint8_t>(value >> 8U));
  bytes.push_back(static_cast<uint8_t>(value >> 16U));
  bytes.push_back(static_cast<uint8_t>(value >> 24U));
}

std::vector<uint8_t> makeValidBmp(const uint8_t pixel = 0x80U) {
  constexpr uint32_t pixelOffset = 14U + 40U + 8U;
  constexpr uint32_t fileSize = pixelOffset + 4U;
  std::vector<uint8_t> bytes;
  bytes.reserve(fileSize);
  appendLe16(bytes, 0x4D42U);
  appendLe32(bytes, fileSize);
  appendLe32(bytes, 0);
  appendLe32(bytes, pixelOffset);
  appendLe32(bytes, 40);
  appendLe32(bytes, 1);
  appendLe32(bytes, 1);
  appendLe16(bytes, 1);
  appendLe16(bytes, 1);
  appendLe32(bytes, 0);
  appendLe32(bytes, 4);
  appendLe32(bytes, 0);
  appendLe32(bytes, 0);
  appendLe32(bytes, 2);
  appendLe32(bytes, 0);
  bytes.insert(bytes.end(), {0, 0, 0, 0, 255, 255, 255, 0});
  bytes.insert(bytes.end(), {pixel, 0, 0, 0});
  return bytes;
}

std::vector<uint8_t> makePngHeader(const uint32_t width, const uint32_t height) {
  std::vector<uint8_t> bytes = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
                                0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52};
  for (const uint32_t value : {width, height}) {
    bytes.push_back(static_cast<uint8_t>(value >> 24U));
    bytes.push_back(static_cast<uint8_t>(value >> 16U));
    bytes.push_back(static_cast<uint8_t>(value >> 8U));
    bytes.push_back(static_cast<uint8_t>(value));
  }
  return bytes;
}

std::vector<uint8_t> makeValidBmpSized(const uint32_t width, const uint32_t height, const uint8_t pixel = 0x80U) {
  constexpr uint32_t pixelOffset = 14U + 40U + 8U;
  const uint32_t rowBytes = ((width + 31U) / 32U) * 4U;
  const uint32_t imageSize = rowBytes * height;
  const uint32_t fileSize = pixelOffset + imageSize;
  std::vector<uint8_t> bytes;
  bytes.reserve(fileSize);
  appendLe16(bytes, 0x4D42U);
  appendLe32(bytes, fileSize);
  appendLe32(bytes, 0);
  appendLe32(bytes, pixelOffset);
  appendLe32(bytes, 40);
  appendLe32(bytes, width);
  appendLe32(bytes, height);
  appendLe16(bytes, 1);
  appendLe16(bytes, 1);
  appendLe32(bytes, 0);
  appendLe32(bytes, imageSize);
  appendLe32(bytes, 0);
  appendLe32(bytes, 0);
  appendLe32(bytes, 2);
  appendLe32(bytes, 0);
  bytes.insert(bytes.end(), {0, 0, 0, 0, 255, 255, 255, 0});
  bytes.resize(fileSize, pixel);
  return bytes;
}

std::string asString(const std::vector<uint8_t>& bytes) {
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::string parseGuideStartReference(const std::string& guideXml) {
  const std::string xml =
      R"(<?xml version="1.0"?><package xmlns:dc="http://purl.org/dc/elements/1.1/"><metadata><dc:title>Guide</dc:title></metadata><manifest/><spine/><guide>)" +
      guideXml + R"(</guide></package>)";
  const std::string cachePath = CACHE_PATH;
  const std::string baseContentPath = "OPS/";
  ContentOpfParser parser(cachePath, baseContentPath, xml.size(), nullptr);
  EXPECT_TRUE(parser.setup());
  EXPECT_EQ(parser.write(reinterpret_cast<const uint8_t*>(xml.data()), xml.size()), xml.size());
  EXPECT_TRUE(parser.succeeded());
  return parser.textReferenceHref;
}

bool parseOpfIntoScratchCache(const std::string& xml) {
  BookMetadataCache cache(CACHE_PATH);
  if (!cache.beginWrite() || !cache.beginContentOpfPass()) return false;
  const std::string cachePath = CACHE_PATH;
  const std::string baseContentPath = "OPS/";
  ContentOpfParser parser(cachePath, baseContentPath, xml.size(), &cache);
  const bool parsed = parser.setup() &&
                      parser.write(reinterpret_cast<const uint8_t*>(xml.data()), xml.size()) == xml.size() &&
                      parser.succeeded();
  cache.cancelWrite();
  return parsed;
}

bool parseNavIntoScratchCache(const std::string& xml) {
  BookMetadataCache cache(CACHE_PATH);
  if (!cache.beginWrite() || !cache.beginContentOpfPass()) return false;
  cache.createSpineEntry("OPS/chapter.xhtml");
  if (!cache.endContentOpfPass() || !cache.beginTocPass()) {
    cache.cancelWrite();
    return false;
  }
  const std::string baseContentPath = "OPS/";
  TocNavParser parser(baseContentPath, xml.size(), &cache);
  const bool parsed =
      parser.setup() && parser.write(reinterpret_cast<const uint8_t*>(xml.data()), xml.size()) == xml.size();
  cache.cancelWrite();
  return parsed;
}

bool parseNcxIntoScratchCache(const std::string& xml) {
  BookMetadataCache cache(CACHE_PATH);
  if (!cache.beginWrite() || !cache.beginContentOpfPass()) return false;
  cache.createSpineEntry("OPS/chapter.xhtml");
  if (!cache.endContentOpfPass() || !cache.beginTocPass()) {
    cache.cancelWrite();
    return false;
  }
  const std::string baseContentPath = "OPS/";
  TocNcxParser parser(baseContentPath, xml.size(), &cache);
  const bool parsed = parser.setup() &&
                      parser.write(reinterpret_cast<const uint8_t*>(xml.data()), xml.size()) == xml.size() &&
                      parser.succeeded();
  cache.cancelWrite();
  return parsed;
}

struct StoredZipEntry {
  StoredZipEntry(std::string entryName, std::string entryContents, const bool useDeflate = false)
      : name(std::move(entryName)), contents(std::move(entryContents)), deflated(useDeflate) {}

  std::string name;
  std::string contents;
  bool deflated = false;
  uint32_t localOffset = 0;
  uint32_t crc = 0;
  std::vector<uint8_t> payload;
};

std::vector<uint8_t> makeRawDeflateStoredBlock(const std::string& contents) {
  EXPECT_LE(contents.size(), std::numeric_limits<uint16_t>::max());
  const uint16_t length = static_cast<uint16_t>(contents.size());
  const uint16_t inverseLength = static_cast<uint16_t>(~length);
  std::vector<uint8_t> payload = {0x01U, static_cast<uint8_t>(length), static_cast<uint8_t>(length >> 8U),
                                  static_cast<uint8_t>(inverseLength), static_cast<uint8_t>(inverseLength >> 8U)};
  payload.insert(payload.end(), contents.begin(), contents.end());
  return payload;
}

std::vector<uint8_t> makeStoredZip(std::vector<StoredZipEntry> entries) {
  std::vector<uint8_t> bytes;
  for (size_t i = 0; i < entries.size(); ++i) {
    auto& entry = entries[i];
    entry.localOffset = static_cast<uint32_t>(bytes.size());
    entry.crc = 0x13572468U + static_cast<uint32_t>(i);
    entry.payload = entry.deflated ? makeRawDeflateStoredBlock(entry.contents)
                                   : std::vector<uint8_t>(entry.contents.begin(), entry.contents.end());
    appendLe32(bytes, 0x04034B50U);
    appendLe16(bytes, 20);
    appendLe16(bytes, 0);
    appendLe16(bytes, entry.deflated ? 8U : 0U);
    appendLe16(bytes, 0);
    appendLe16(bytes, 0);
    appendLe32(bytes, entry.crc);
    appendLe32(bytes, static_cast<uint32_t>(entry.payload.size()));
    appendLe32(bytes, static_cast<uint32_t>(entry.contents.size()));
    appendLe16(bytes, static_cast<uint16_t>(entry.name.size()));
    appendLe16(bytes, 0);
    bytes.insert(bytes.end(), entry.name.begin(), entry.name.end());
    bytes.insert(bytes.end(), entry.payload.begin(), entry.payload.end());
  }

  const uint32_t centralOffset = static_cast<uint32_t>(bytes.size());
  for (const auto& entry : entries) {
    appendLe32(bytes, 0x02014B50U);
    appendLe16(bytes, 20);
    appendLe16(bytes, 20);
    appendLe16(bytes, 0);
    appendLe16(bytes, entry.deflated ? 8U : 0U);
    appendLe16(bytes, 0);
    appendLe16(bytes, 0);
    appendLe32(bytes, entry.crc);
    appendLe32(bytes, static_cast<uint32_t>(entry.payload.size()));
    appendLe32(bytes, static_cast<uint32_t>(entry.contents.size()));
    appendLe16(bytes, static_cast<uint16_t>(entry.name.size()));
    appendLe16(bytes, 0);
    appendLe16(bytes, 0);
    appendLe16(bytes, 0);
    appendLe16(bytes, 0);
    appendLe32(bytes, 0);
    appendLe32(bytes, entry.localOffset);
    bytes.insert(bytes.end(), entry.name.begin(), entry.name.end());
  }

  const uint32_t centralSize = static_cast<uint32_t>(bytes.size()) - centralOffset;
  appendLe32(bytes, 0x06054B50U);
  appendLe16(bytes, 0);
  appendLe16(bytes, 0);
  appendLe16(bytes, static_cast<uint16_t>(entries.size()));
  appendLe16(bytes, static_cast<uint16_t>(entries.size()));
  appendLe32(bytes, centralSize);
  appendLe32(bytes, centralOffset);
  appendLe16(bytes, 0);
  return bytes;
}

std::vector<uint8_t> makeCssTestEpub(std::string stylesheet = ".note { text-align: right; margin-left: 2px; }") {
  return makeStoredZip({
      {"META-INF/container.xml",
       R"(<?xml version="1.0"?><container><rootfiles><rootfile full-path="OPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>)"},
      {"OPS/content.opf",
       R"(<?xml version="1.0"?><package xmlns:dc="http://purl.org/dc/elements/1.1/"><metadata><dc:title>CSS test</dc:title><dc:creator>CrossVi</dc:creator><dc:language>en</dc:language></metadata><manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/><item id="style" href="styles.css" media-type="text/css"/></manifest><spine><itemref idref="chapter"/></spine></package>)"},
      {"OPS/chapter.xhtml", "<html><body><p class=\"note\">Test</p></body></html>"},
      {"OPS/styles.css", std::move(stylesheet)},
  });
}

std::vector<uint8_t> makeNestedNcxEpub() {
  return makeStoredZip({
      {"META-INF/container.xml",
       R"(<?xml version="1.0"?><container><rootfiles><rootfile full-path="OPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>)"},
      {"OPS/content.opf",
       R"(<?xml version="1.0"?><package xmlns:dc="http://purl.org/dc/elements/1.1/"><metadata><dc:title>Nested NCX</dc:title><dc:creator>CrossVi</dc:creator></metadata><manifest><item id="chapter" href="toc/ch1.xhtml" media-type="application/xhtml+xml"/><item id="ncx" href="toc/toc.ncx" media-type="application/x-dtbncx+xml"/></manifest><spine toc="ncx"><itemref idref="chapter"/></spine></package>)"},
      {"OPS/toc/toc.ncx",
       R"(<?xml version="1.0"?><ncx><navMap><navPoint><navLabel><text>Chapter 1</text></navLabel><content src="ch1.xhtml"/></navPoint></navMap></ncx>)"},
      {"OPS/toc/ch1.xhtml", "<html><body><p>Chapter 1</p></body></html>"},
  });
}

std::vector<uint8_t> makeNavFallbackEpub() {
  return makeStoredZip({
      {"META-INF/container.xml",
       R"(<?xml version="1.0"?><container><rootfiles><rootfile full-path="OPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>)"},
      {"OPS/content.opf",
       R"(<?xml version="1.0"?><package xmlns:dc="http://purl.org/dc/elements/1.1/"><metadata><dc:title>TOC fallback</dc:title></metadata><manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/><item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/><item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/></manifest><spine toc="ncx"><itemref idref="chapter"/></spine></package>)"},
      {"OPS/nav.xhtml",
       R"(<?xml version="1.0"?><html><body><nav epub:type="landmarks"><ol><li><a href="chapter.xhtml">Landmark only</a></li></ol></nav></body></html>)"},
      {"OPS/toc.ncx",
       R"(<?xml version="1.0"?><ncx><navMap><navPoint><navLabel><text>NCX chapter</text></navLabel><content src="chapter.xhtml"/></navPoint></navMap></ncx>)"},
      {"OPS/chapter.xhtml", "<html><body><p>Chapter</p></body></html>"},
  });
}

std::vector<uint8_t> makeNonLinearSpineEpub() {
  return makeStoredZip({
      {"META-INF/container.xml",
       R"(<?xml version="1.0"?><container><rootfiles><rootfile full-path="OPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>)"},
      {"OPS/content.opf",
       R"(<?xml version="1.0"?><package xmlns:dc="http://purl.org/dc/elements/1.1/"><metadata><dc:title>Linear spine</dc:title></metadata><manifest><item id="one" href="one.xhtml" media-type="application/xhtml+xml"/><item id="notes" href="notes.xhtml" media-type="application/xhtml+xml"/><item id="two" href="two.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="one"/><itemref idref="notes" linear="no"/><itemref idref="two"/></spine></package>)"},
      {"OPS/one.xhtml", "<html><body><p>One</p></body></html>"},
      {"OPS/notes.xhtml", "<html><body><p>Notes</p></body></html>"},
      {"OPS/two.xhtml", "<html><body><p>Two</p></body></html>"},
  });
}

std::vector<uint8_t> makeDuplicateBasenameEpub() {
  return makeStoredZip({
      {"META-INF/container.xml",
       R"(<?xml version="1.0"?><container><rootfiles><rootfile full-path="OPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>)"},
      {"OPS/content.opf",
       R"(<?xml version="1.0"?><package xmlns:dc="http://purl.org/dc/elements/1.1/"><metadata><dc:title>Relative links</dc:title></metadata><manifest><item id="one" href="part1/ch1.xhtml" media-type="application/xhtml+xml"/><item id="current" href="text/current.xhtml" media-type="application/xhtml+xml"/><item id="other" href="other/ch1.xhtml" media-type="application/xhtml+xml"/><item id="two" href="part2/ch1.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="one"/><itemref idref="current"/><itemref idref="other"/><itemref idref="two"/></spine></package>)"},
      {"OPS/part1/ch1.xhtml", "<html><body><p>One</p></body></html>"},
      {"OPS/text/current.xhtml", "<html><body><p>Current</p></body></html>"},
      {"OPS/other/ch1.xhtml", "<html><body><p>Other</p></body></html>"},
      {"OPS/part2/ch1.xhtml", "<html><body><p>Two</p></body></html>"},
  });
}

std::vector<uint8_t> makeGuideCoverEpub(std::string coverPage, const bool deflateCover = false,
                                        std::string coverContents = "not-decoded-by-this-test") {
  return makeStoredZip({
      {"META-INF/container.xml",
       R"(<?xml version="1.0"?><container><rootfiles><rootfile full-path="OPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>)"},
      {"OPS/content.opf",
       R"(<?xml version="1.0"?><package xmlns:dc="http://purl.org/dc/elements/1.1/"><metadata><dc:title>Guide cover</dc:title><dc:creator>CrossVi</dc:creator><dc:language>en</dc:language></metadata><manifest><item id="cover-page" href="cover.xhtml" media-type="application/xhtml+xml"/><item id="cover-image" href="images/cover.jpg" media-type="image/jpeg"/></manifest><spine/><guide><reference type="cover" href="cover.xhtml"/></guide></package>)"},
      {"OPS/cover.xhtml", std::move(coverPage)},
      {"OPS/images/cover.jpg", std::move(coverContents), deflateCover},
  });
}

ZipFile::SourceIdentity identify(const std::vector<uint8_t>& bytes) {
  Storage.setFile(EPUB_PATH, bytes);
  const std::string path = EPUB_PATH;
  ZipFile zip(path);
  ZipFile::SourceIdentity identity;
  EXPECT_TRUE(zip.getSourceIdentity(identity));
  return identity;
}

std::vector<uint8_t> makeBookCache(const ZipFile::SourceIdentity& identity, const bool withCover = true) {
  constexpr uint8_t version = 12;
  constexpr uint16_t spineCount = 1;
  constexpr uint16_t tocCount = 1;
  constexpr uint32_t commitMarker = 0x424D434B;

  SourceIdentityCodec::Payload identityPayload{};
  EXPECT_TRUE(SourceIdentityCodec::encodePayload(identity, identityPayload));

  std::vector<uint8_t> bytes;
  appendPod(bytes, version);
  const size_t lutOffsetPosition = bytes.size();
  appendPod(bytes, uint32_t{0});
  appendPod(bytes, spineCount);
  appendPod(bytes, tocCount);
  bytes.insert(bytes.end(), identityPayload.begin(), identityPayload.end());
  appendPod(bytes, SourceIdentityCodec::crc32(identityPayload.data(), identityPayload.size()));
  appendString(bytes, "A safe title");
  appendString(bytes, "An author");
  appendString(bytes, "en");
  appendString(bytes, withCover ? "cover.xhtml" : "");
  appendString(bytes, "text.xhtml");

  const uint32_t lutOffset = static_cast<uint32_t>(bytes.size());
  overwritePod(bytes, lutOffsetPosition, lutOffset);
  const size_t spineLutPosition = bytes.size();
  appendPod(bytes, uint32_t{0});
  const size_t tocLutPosition = bytes.size();
  appendPod(bytes, uint32_t{0});

  const uint32_t spineOffset = static_cast<uint32_t>(bytes.size());
  overwritePod(bytes, spineLutPosition, spineOffset);
  appendString(bytes, "OPS/chapter.xhtml");
  appendPod(bytes, uint8_t{1});
  appendPod(bytes, uint32_t{1234});
  appendPod(bytes, int16_t{0});

  const uint32_t tocOffset = static_cast<uint32_t>(bytes.size());
  overwritePod(bytes, tocLutPosition, tocOffset);
  appendString(bytes, "Chapter 1");
  appendString(bytes, "OPS/chapter.xhtml");
  appendString(bytes, "start");
  appendPod(bytes, uint8_t{1});
  appendPod(bytes, int16_t{0});
  appendPod(bytes, commitMarker);
  return bytes;
}

std::vector<uint8_t> makeSpineOnlyBookCache(const ZipFile::SourceIdentity& identity, const uint16_t spineCount) {
  constexpr uint8_t version = 12;
  constexpr uint32_t commitMarker = 0x424D434B;
  SourceIdentityCodec::Payload identityPayload{};
  EXPECT_TRUE(SourceIdentityCodec::encodePayload(identity, identityPayload));

  std::vector<uint8_t> bytes;
  appendPod(bytes, version);
  const size_t lutOffsetPosition = bytes.size();
  appendPod(bytes, uint32_t{0});
  appendPod(bytes, spineCount);
  appendPod(bytes, uint16_t{0});
  bytes.insert(bytes.end(), identityPayload.begin(), identityPayload.end());
  appendPod(bytes, SourceIdentityCodec::crc32(identityPayload.data(), identityPayload.size()));
  for (const std::string value : {"Book", "Author", "en", "", ""}) appendString(bytes, value);

  const uint32_t lutOffset = static_cast<uint32_t>(bytes.size());
  overwritePod(bytes, lutOffsetPosition, lutOffset);
  std::vector<size_t> lutPositions;
  lutPositions.reserve(spineCount);
  for (uint16_t index = 0; index < spineCount; ++index) {
    lutPositions.push_back(bytes.size());
    appendPod(bytes, uint32_t{0});
  }
  for (uint16_t index = 0; index < spineCount; ++index) {
    overwritePod(bytes, lutPositions[index], static_cast<uint32_t>(bytes.size()));
    appendString(bytes, "chapter-" + std::to_string(index));
    appendPod(bytes, uint8_t{1});
    appendPod(bytes, static_cast<uint32_t>(index + 1));
    appendPod(bytes, int16_t{-1});
  }
  appendPod(bytes, commitMarker);
  return bytes;
}

class EpubSourceIdentityTest : public testing::Test {
 protected:
  void SetUp() override {
    ESP.setHeap(1024U * 1024U, 1024U * 1024U);
    Storage.reset();
    ThumbnailConverterStub::reset();
    Storage.mkdir("/.crosspoint");
    Storage.mkdir(CACHE_PATH);
  }
};

TEST_F(EpubSourceIdentityTest, CentralDirectoryIdentityIsStableAndContentSensitive) {
  const auto first = identify(makeZip());
  const auto again = identify(makeZip());
  const auto changedCrc = identify(makeZip(0x12345679U));
  const auto changedName = identify(makeZip(0x12345678U, 'b'));

  EXPECT_EQ(first, again);
  EXPECT_EQ(first.fileSize, changedCrc.fileSize);
  EXPECT_NE(first, changedCrc);
  EXPECT_NE(first, changedName);
  EXPECT_LE(Storage.maxRead(), 1024U);
}

TEST_F(EpubSourceIdentityTest, CentralDirectoryIdentityCanBeHashedInBoundedSteps) {
  const auto bytes = makeStoredZip({{"one.xhtml", "one"}, {"two.xhtml", "two"}, {"three.xhtml", "three"}});
  const auto expected = identify(bytes);
  Storage.resetIoCounters();

  ZipSourceIdentityJob job;
  ASSERT_TRUE(job.begin(EPUB_PATH));
  ZipFile::SourceIdentity actual;
  ZipSourceIdentityJob::StepStatus status = ZipSourceIdentityJob::StepStatus::InProgress;
  size_t steps = 0;
  while (status == ZipSourceIdentityJob::StepStatus::InProgress && steps++ < 64U) {
    status = job.step(32, actual);
  }

  ASSERT_EQ(status, ZipSourceIdentityJob::StepStatus::Done);
  EXPECT_GT(steps, 1U);
  EXPECT_EQ(actual, expected);
  EXPECT_LE(Storage.maxRead(), 1024U);
}

TEST_F(EpubSourceIdentityTest, VerifiedRecoveryIdentityAvoidsInitialArchiveRescan) {
  const auto identity = identify(makeZip());
  Epub epub(EPUB_PATH, "/.crosspoint", identity);
  ASSERT_TRUE(Storage.mkdir(epub.getCachePath().c_str()));
  ASSERT_EQ(SourceIdentityStore::save(epub.getCachePath(), identity), SourceIdentityStore::SaveStatus::Saved);

  Storage.resetIoCounters();
  EXPECT_EQ(epub.inspectSourceBinding(), Epub::SourceBindingStatus::Match);
  EXPECT_EQ(Storage.openReadAttemptsFor(EPUB_PATH), 0U);
}

TEST_F(EpubSourceIdentityTest, PreparedLoadReusesRecoveredBindingProofOnce) {
  const auto identity = identify(makeCssTestEpub());
  Epub epub(EPUB_PATH, "/.crosspoint", identity);
  const std::string cachePath = epub.getCachePath();
  ASSERT_TRUE(Storage.mkdir(cachePath.c_str()));
  ASSERT_EQ(SourceIdentityStore::save(cachePath, identity), SourceIdentityStore::SaveStatus::Saved);
  Storage.setFile(cachePath + "/book.bin", makeBookCache(identity));

  Storage.resetIoCounters();
  ASSERT_EQ(epub.inspectSourceBindingForLoad(), Epub::SourceBindingStatus::Match);
  ASSERT_EQ(epub.inspectCache(), BookMetadataCache::LoadStatus::Loaded);
  ASSERT_TRUE(epub.load(false, true));

  EXPECT_EQ(Storage.openReadAttemptsFor(cachePath + "/source_identity.bin"), 1U);
  EXPECT_EQ(Storage.openReadAttemptsFor(EPUB_PATH), 1U);
}

TEST_F(EpubSourceIdentityTest, PreparedWarmLoadCanDeferOnlyItsFinalSourceCheck) {
  const auto identity = identify(makeCssTestEpub());
  Epub epub(EPUB_PATH, "/.crosspoint", identity);
  const std::string cachePath = epub.getCachePath();
  ASSERT_TRUE(Storage.mkdir(cachePath.c_str()));
  ASSERT_EQ(SourceIdentityStore::save(cachePath, identity), SourceIdentityStore::SaveStatus::Saved);
  Storage.setFile(cachePath + "/book.bin", makeBookCache(identity));

  ASSERT_EQ(epub.inspectSourceBindingForLoad(), Epub::SourceBindingStatus::Match);
  ASSERT_EQ(epub.inspectCache(), BookMetadataCache::LoadStatus::Loaded);
  Storage.resetIoCounters();
  ASSERT_TRUE(epub.loadForCooperativeSourceCheck(false, true));
  EXPECT_EQ(Storage.openReadAttemptsFor(EPUB_PATH), 0U);

  ZipSourceIdentityJob finalCheck;
  ASSERT_TRUE(finalCheck.begin(EPUB_PATH));
  ZipFile::SourceIdentity verified;
  auto status = ZipSourceIdentityJob::StepStatus::InProgress;
  while (status == ZipSourceIdentityJob::StepStatus::InProgress) status = finalCheck.step(32, verified);
  ASSERT_EQ(status, ZipSourceIdentityJob::StepStatus::Done);
  EXPECT_EQ(verified, identity);
}

TEST_F(EpubSourceIdentityTest, PreparedWarmCacheInspectionYieldsAndIsReusedByLoad) {
  const auto identity = identify(makeZip());
  Epub epub(EPUB_PATH, "/.crosspoint", identity);
  const std::string cachePath = epub.getCachePath();
  ASSERT_TRUE(Storage.mkdir(cachePath.c_str()));
  ASSERT_EQ(SourceIdentityStore::save(cachePath, identity), SourceIdentityStore::SaveStatus::Saved);
  Storage.setFile(cachePath + "/book.bin", makeSpineOnlyBookCache(identity, 65));

  ASSERT_EQ(epub.inspectSourceBindingForLoad(), Epub::SourceBindingStatus::Match);
  ASSERT_EQ(epub.beginCacheInspection(), BookMetadataCache::LoadStepResult::InProgress);
  size_t steps = 0;
  auto result = BookMetadataCache::LoadStepResult::InProgress;
  while (result == BookMetadataCache::LoadStepResult::InProgress) {
    result = epub.stepCacheInspection(8);
    ++steps;
  }
  ASSERT_EQ(result, BookMetadataCache::LoadStepResult::Loaded);
  EXPECT_EQ(steps, 9U);

  Storage.resetIoCounters();
  ASSERT_TRUE(epub.loadForCooperativeSourceCheck(false, true));
  EXPECT_EQ(Storage.openReadAttemptsFor(cachePath + "/book.bin"), 0U);
}

TEST_F(EpubSourceIdentityTest, GuideUsesOnlyFirstExplicitStartReference) {
  EXPECT_EQ(
      parseGuideStartReference(
          R"(<reference type="text" href="front.xhtml"/><reference type="start" href="chapter.xhtml"/><reference type="start" href="later.xhtml"/>)"),
      "OPS/chapter.xhtml");
  EXPECT_TRUE(parseGuideStartReference(
                  R"(<reference type="text" href="front.xhtml"/><reference type="text" href="preface.xhtml"/>)")
                  .empty());
}

TEST_F(EpubSourceIdentityTest, CreatorEntitiesDoNotInventAuthorSeparators) {
  const std::string xml =
      R"(<?xml version="1.0"?><package xmlns:dc="http://purl.org/dc/elements/1.1/"><metadata><dc:creator>John &amp; Smith</dc:creator><dc:creator>Jane Doe</dc:creator></metadata></package>)";
  const std::string cachePath = CACHE_PATH;
  const std::string baseContentPath = "OPS/";
  ContentOpfParser parser(cachePath, baseContentPath, xml.size(), nullptr);
  ASSERT_TRUE(parser.setup());
  ASSERT_EQ(parser.write(reinterpret_cast<const uint8_t*>(xml.data()), xml.size()), xml.size());
  ASSERT_TRUE(parser.succeeded());
  EXPECT_EQ(parser.author, "John & Smith, Jane Doe");
}

TEST_F(EpubSourceIdentityTest, ContainerUsesTheFirstSupportedRootfile) {
  const std::string xml =
      R"(<container><rootfiles><rootfile full-path="OPS/default.opf" media-type="application/oebps-package+xml"/><rootfile full-path="OPS/alternate.opf" media-type="application/oebps-package+xml"/></rootfiles></container>)";
  ContainerParser parser(xml.size());
  ASSERT_TRUE(parser.setup());
  ASSERT_EQ(parser.write(reinterpret_cast<const uint8_t*>(xml.data()), xml.size()), xml.size());
  EXPECT_EQ(parser.fullPath, "OPS/default.opf");
}

TEST_F(EpubSourceIdentityTest, ContentOpfKeepsTrimmedPrimaryLanguageAcrossChunkedInput) {
  const std::string xml =
      R"(<package xmlns:dc="http://purl.org/dc/elements/1.1/"><metadata><dc:language>  en-US  </dc:language><dc:language>fr</dc:language></metadata></package>)";
  const std::string cachePath;
  const std::string basePath;
  ContentOpfParser parser(cachePath, basePath, xml.size(), nullptr);
  ASSERT_TRUE(parser.setup());
  for (const char c : xml) {
    ASSERT_EQ(parser.write(static_cast<uint8_t>(c)), 1U);
  }
  ASSERT_TRUE(parser.succeeded());
  EXPECT_EQ(parser.language, "en-US");
}

TEST_F(EpubSourceIdentityTest, ContentOpfPropertiesRequireExactTokens) {
  const std::string xml =
      R"(<package><manifest><item id="decoy" href="wrong.xhtml" properties="scripted navigation cover-image-extra"/><item id="nav" href="nav.xhtml" properties="scripted nav"/><item id="cover" href="cover.jpg" properties="cover-image remote-resources"/></manifest></package>)";
  const std::string cachePath;
  const std::string basePath = "OPS/";
  ContentOpfParser parser(cachePath, basePath, xml.size(), nullptr);
  ASSERT_TRUE(parser.setup());
  ASSERT_EQ(parser.write(reinterpret_cast<const uint8_t*>(xml.data()), xml.size()), xml.size());
  ASSERT_TRUE(parser.succeeded());
  EXPECT_EQ(parser.tocNavPath, "OPS/nav.xhtml");
  EXPECT_EQ(parser.coverItemHref, "OPS/cover.jpg");
}

TEST_F(EpubSourceIdentityTest, ContentOpfRejectsUnresolvedSpineIdref) {
  const std::string xml =
      R"(<package><manifest><item id="chapter" href="chapter.xhtml"/></manifest><spine><itemref idref="missing"/></spine></package>)";

  EXPECT_FALSE(parseOpfIntoScratchCache(xml));
}

TEST_F(EpubSourceIdentityTest, ContentOpfRejectsDuplicateManifestIds) {
  const std::string xml =
      R"(<package><manifest><item id="chapter" href="one.xhtml"/><item id="chapter" href="two.xhtml"/></manifest><spine><itemref idref="chapter"/></spine></package>)";

  EXPECT_FALSE(parseOpfIntoScratchCache(xml));
}

TEST_F(EpubSourceIdentityTest, ContentOpfRejectsOversizedMetadataText) {
  const std::string xml = "<package xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><metadata><dc:title>" +
                          std::string(4097, 'x') + "</dc:title></metadata><manifest/><spine/></package>";

  EXPECT_FALSE(parseOpfIntoScratchCache(xml));
}

TEST_F(EpubSourceIdentityTest, ContentOpfBoundsManifestIndexEntries) {
  std::string xml = "<package><manifest>";
  for (int i = 0; i < 2049; ++i) {
    xml += "<item id=\"i" + std::to_string(i) + "\" href=\"r" + std::to_string(i) + ".xhtml\"/>";
  }
  xml += "</manifest><spine/></package>";

  EXPECT_FALSE(parseOpfIntoScratchCache(xml));
}

TEST_F(EpubSourceIdentityTest, TocNavRejectsOversizedEntryLabel) {
  const std::string xml = "<html><body><nav epub:type=\"toc\"><ol><li><a href=\"chapter.xhtml\">" +
                          std::string(4097, 'x') + "</a></li></ol></nav></body></html>";

  EXPECT_FALSE(parseNavIntoScratchCache(xml));
}

TEST_F(EpubSourceIdentityTest, TocNcxRejectsOversizedEntryLabel) {
  const std::string xml = "<ncx><navMap><navPoint><navLabel><text>" + std::string(4097, 'x') +
                          "</text></navLabel><content src=\"chapter.xhtml\"/></navPoint></navMap></ncx>";

  EXPECT_FALSE(parseNcxIntoScratchCache(xml));
}

TEST_F(EpubSourceIdentityTest, ColdIndexingRejectsSpineResourcesMissingFromArchive) {
  identify(makeStoredZip({
      {"META-INF/container.xml",
       R"(<?xml version="1.0"?><container><rootfiles><rootfile full-path="OPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>)"},
      {"OPS/content.opf",
       R"(<?xml version="1.0"?><package><manifest><item id="missing" href="missing.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="missing"/></spine></package>)"},
  }));
  Epub epub(EPUB_PATH, "/.crosspoint");
  ASSERT_TRUE(Storage.mkdir(epub.getCachePath().c_str()));
  ASSERT_TRUE(epub.bindCurrentSource());

  EXPECT_FALSE(epub.load(true, true));
}

TEST_F(EpubSourceIdentityTest, NcxTargetsResolveRelativeToTheNcxDirectory) {
  identify(makeNestedNcxEpub());
  Epub epub(EPUB_PATH, "/.crosspoint");
  ASSERT_TRUE(Storage.mkdir(epub.getCachePath().c_str()));
  ASSERT_TRUE(epub.bindCurrentSource());
  ASSERT_TRUE(epub.load(true, true));

  ASSERT_EQ(epub.getSpineItemsCount(), 1);
  ASSERT_EQ(epub.getTocItemsCount(), 1);
  EXPECT_EQ(epub.getTocItem(0).href, "OPS/toc/ch1.xhtml");
  EXPECT_EQ(epub.getSpineIndexForTocIndex(0), 0);
}

TEST_F(EpubSourceIdentityTest, EmptyEpub3NavigationFallsBackToNcxWithoutKeepingPartialEntries) {
  identify(makeNavFallbackEpub());
  Epub epub(EPUB_PATH, "/.crosspoint");
  ASSERT_TRUE(Storage.mkdir(epub.getCachePath().c_str()));
  ASSERT_TRUE(epub.bindCurrentSource());
  ASSERT_TRUE(epub.load(true, true));

  ASSERT_EQ(epub.getTocItemsCount(), 1);
  EXPECT_EQ(epub.getTocItem(0).title, "NCX chapter");
  EXPECT_EQ(epub.getTocItem(0).href, "OPS/chapter.xhtml");
}

TEST_F(EpubSourceIdentityTest, NonLinearSpineEntriesRemainAddressableButAreSkippedBySequentialReading) {
  identify(makeNonLinearSpineEpub());
  Epub epub(EPUB_PATH, "/.crosspoint");
  ASSERT_TRUE(Storage.mkdir(epub.getCachePath().c_str()));
  ASSERT_TRUE(epub.bindCurrentSource());
  ASSERT_TRUE(epub.load(true, true));

  ASSERT_EQ(epub.getSpineItemsCount(), 3);
  EXPECT_TRUE(epub.getSpineItem(0).linear);
  EXPECT_FALSE(epub.getSpineItem(1).linear);
  EXPECT_TRUE(epub.getSpineItem(2).linear);
  EXPECT_EQ(epub.getAdjacentLinearSpineIndex(0, true), 2);
  EXPECT_EQ(epub.getAdjacentLinearSpineIndex(2, false), 0);
  EXPECT_EQ(epub.getAdjacentLinearSpineIndex(2, true), epub.getSpineItemsCount());
  EXPECT_EQ(epub.resolveHrefToSpineIndex("notes.xhtml", 0), 1);
}

TEST_F(EpubSourceIdentityTest, ColdIndexingCanBeCancelledAndRetriedBetweenPasses) {
  identify(makeNestedNcxEpub());
  Epub epub(EPUB_PATH, "/.crosspoint");
  const std::string cachePath = epub.getCachePath();
  ASSERT_TRUE(Storage.mkdir(cachePath.c_str()));
  ASSERT_TRUE(epub.bindCurrentSource());
  ASSERT_EQ(epub.inspectSourceBindingForLoad(), Epub::SourceBindingStatus::Match);
  ASSERT_EQ(epub.inspectCache(), BookMetadataCache::LoadStatus::Missing);
  ASSERT_TRUE(epub.beginIndexing(/*skipLoadingCss=*/true));
  ASSERT_TRUE(epub.isIndexing());

  ASSERT_EQ(epub.stepIndexing(), Epub::IndexStepResult::InProgress);
  ASSERT_TRUE(Storage.exists((cachePath + "/spine.bin.tmp").c_str()));
  epub.cancelIndexing();

  EXPECT_FALSE(epub.isIndexing());
  EXPECT_FALSE(Storage.exists((cachePath + "/spine.bin.tmp").c_str()));
  EXPECT_FALSE(Storage.exists((cachePath + "/toc.bin.tmp").c_str()));
  EXPECT_FALSE(Storage.exists((cachePath + "/.items.bin").c_str()));
  EXPECT_FALSE(Storage.exists((cachePath + "/book.bin").c_str()));

  Epub retry(EPUB_PATH, "/.crosspoint");
  ASSERT_EQ(retry.inspectSourceBindingForLoad(), Epub::SourceBindingStatus::Match);
  ASSERT_EQ(retry.inspectCache(), BookMetadataCache::LoadStatus::Missing);
  ASSERT_TRUE(retry.beginIndexing(/*skipLoadingCss=*/true));

  Epub::IndexStepResult result = Epub::IndexStepResult::InProgress;
  size_t steps = 0;
  while (result == Epub::IndexStepResult::InProgress && steps++ < 64U) result = retry.stepIndexing();
  ASSERT_EQ(result, Epub::IndexStepResult::Loaded);
  EXPECT_GT(steps, 8U);
  EXPECT_FALSE(retry.isIndexing());
  EXPECT_EQ(retry.getTitle(), "Nested NCX");
  ASSERT_EQ(retry.getTocItemsCount(), 1);
  EXPECT_EQ(retry.getTocItem(0).href, "OPS/toc/ch1.xhtml");
  EXPECT_FALSE(Storage.exists((cachePath + "/spine.bin.tmp").c_str()));
  EXPECT_FALSE(Storage.exists((cachePath + "/toc.bin.tmp").c_str()));
}

TEST_F(EpubSourceIdentityTest, MetadataScratchCleanupReportsRemovalFailure) {
  ASSERT_TRUE(Storage.exists(CACHE_PATH));
  const std::string spineScratch = std::string(CACHE_PATH) + "/spine.bin.tmp";
  Storage.setFile(spineScratch, {0x01U});
  Storage.failRemoveFor(spineScratch);

  BookMetadataCache cache(CACHE_PATH);
  EXPECT_FALSE(cache.cleanupTmpFiles());
  EXPECT_TRUE(Storage.exists(spineScratch.c_str()));
  EXPECT_TRUE(cache.cleanupTmpFiles());
  EXPECT_FALSE(Storage.exists(spineScratch.c_str()));
}

TEST_F(EpubSourceIdentityTest, ReaderLinksResolveRelativeToTheCurrentSpineDirectory) {
  identify(makeDuplicateBasenameEpub());
  Epub epub(EPUB_PATH, "/.crosspoint");
  ASSERT_TRUE(Storage.mkdir(epub.getCachePath().c_str()));
  ASSERT_TRUE(epub.bindCurrentSource());
  ASSERT_TRUE(epub.load(true, true));

  ASSERT_EQ(epub.getSpineItemsCount(), 4);
  ASSERT_EQ(epub.getSpineItem(0).href, "OPS/part1/ch1.xhtml");
  ASSERT_EQ(epub.getSpineItem(1).href, "OPS/text/current.xhtml");
  ASSERT_EQ(epub.getSpineItem(2).href, "OPS/other/ch1.xhtml");
  ASSERT_EQ(epub.getSpineItem(3).href, "OPS/part2/ch1.xhtml");
  EXPECT_EQ(epub.resolveHrefToSpineIndex("../part2/ch1.xhtml#note", 1), 3);
  EXPECT_EQ(epub.resolveHrefToSpineIndex("ch1.xhtml#note", -1), -1);
}

TEST_F(EpubSourceIdentityTest, RejectsMalformedBoundsAndConcurrentGrowth) {
  auto malformed = makeZip();
  put32(malformed, malformed.size() - 22 + 12, UINT32_MAX);
  Storage.setFile(EPUB_PATH, malformed);
  const std::string path = EPUB_PATH;
  ZipFile malformedZip(path);
  ZipFile::SourceIdentity identity;
  EXPECT_FALSE(malformedZip.getSourceIdentity(identity));

  Storage.reset();
  Storage.setFile(EPUB_PATH, makeZip());
  Storage.growOnReadCall(2);
  ZipFile changingZip(path);
  EXPECT_FALSE(changingZip.getSourceIdentity(identity));
}

TEST_F(EpubSourceIdentityTest, OpensOnlyTheBoundedPayloadOfAStoredEntry) {
  constexpr char entryName[] = "OPS/images/cover.jpg";
  constexpr char payload[] = "stored-jpeg-payload";
  identify(makeStoredZip({{entryName, payload}}));

  const std::string path = EPUB_PATH;
  ZipFile zip(path);
  HalFile archive;
  uint64_t offset = 0;
  uint32_t length = 0;
  ASSERT_EQ(zip.openStoredEntry(entryName, archive, offset, length), ZipFile::StoredEntryOpenStatus::Opened);
  ASSERT_EQ(length, strlen(payload));
  std::vector<uint8_t> bytes(length);
  ASSERT_EQ(archive.read(bytes.data(), bytes.size()), static_cast<int>(bytes.size()));
  EXPECT_EQ(std::string(bytes.begin(), bytes.end()), payload);
  ASSERT_TRUE(archive.seek64(offset + length));
  uint8_t followingByte = 0;
  ASSERT_EQ(archive.read(&followingByte, 1), 1);
  EXPECT_NE(followingByte, static_cast<uint8_t>(payload[length - 1]));
  EXPECT_TRUE(archive.close());
}

TEST_F(EpubSourceIdentityTest, StoredEntryFastPathRejectsHeaderMismatchEncryptionAndOutOfBoundsPayload) {
  constexpr char entryName[] = "cover.jpg";
  const auto makeArchive = [&] { return makeStoredZip({{entryName, "jpeg"}}); };

  auto expectInvalid = [&](std::vector<uint8_t> bytes) {
    Storage.setFile(EPUB_PATH, std::move(bytes));
    const std::string path = EPUB_PATH;
    ZipFile zip(path);
    HalFile archive;
    uint64_t offset = 0;
    uint32_t length = 0;
    EXPECT_EQ(zip.openStoredEntry(entryName, archive, offset, length), ZipFile::StoredEntryOpenStatus::Invalid);
    EXPECT_FALSE(archive);
    EXPECT_EQ(offset, 0U);
    EXPECT_EQ(length, 0U);
  };

  auto nameMismatch = makeArchive();
  nameMismatch[30] ^= 0x01U;
  expectInvalid(std::move(nameMismatch));

  auto encrypted = makeArchive();
  const size_t encryptedCentral = get32(encrypted, encrypted.size() - 22U + 16U);
  put16(encrypted, 6, 1U);
  put16(encrypted, encryptedCentral + 8U, 1U);
  expectInvalid(std::move(encrypted));

  auto oversized = makeArchive();
  const size_t oversizedCentral = get32(oversized, oversized.size() - 22U + 16U);
  put32(oversized, 18, 0x1000U);
  put32(oversized, 22, 0x1000U);
  put32(oversized, oversizedCentral + 20U, 0x1000U);
  put32(oversized, oversizedCentral + 24U, 0x1000U);
  expectInvalid(std::move(oversized));
}

TEST_F(EpubSourceIdentityTest, NonStoredEntryUsesFallback) {
  constexpr char entryName[] = "cover.jpg";
  auto bytes = makeStoredZip({{entryName, "0123456789abcdef", true}});
  Storage.setFile(EPUB_PATH, bytes);

  const std::string path = EPUB_PATH;
  ZipFile compressed(path);
  HalFile archive;
  uint64_t offset = 0;
  uint32_t length = 0;
  EXPECT_EQ(compressed.openStoredEntry(entryName, archive, offset, length), ZipFile::StoredEntryOpenStatus::NotStored);
}

TEST_F(EpubSourceIdentityTest, StreamRejectsOversizedInflatedEntryBeforeWriting) {
  constexpr char entryName[] = "chapter.xhtml";
  constexpr char payload[] = "0123456789abcdef";
  Storage.setFile(EPUB_PATH, makeStoredZip({{entryName, payload, true}}));

  HalFile output;
  ASSERT_TRUE(Storage.openFileForWrite("TEST", "/out.bin", output));
  const std::string path = EPUB_PATH;
  ZipFile zip(path);
  bool limitExceeded = false;
  EXPECT_FALSE(zip.readFileToStream(entryName, output, 4, false, strlen(payload) - 1U, &limitExceeded));
  EXPECT_TRUE(limitExceeded);
  EXPECT_EQ(output.fileSize64(), 0U);
  EXPECT_TRUE(output.close());
}

TEST_F(EpubSourceIdentityTest, StreamAcceptsInflatedEntryAtExactLimit) {
  constexpr char entryName[] = "chapter.xhtml";
  constexpr char payload[] = "0123456789abcdef";
  Storage.setFile(EPUB_PATH, makeStoredZip({{entryName, payload, true}}));

  HalFile output;
  ASSERT_TRUE(Storage.openFileForWrite("TEST", "/out.bin", output));
  const std::string path = EPUB_PATH;
  ZipFile zip(path);
  bool limitExceeded = true;
  EXPECT_TRUE(zip.readFileToStream(entryName, output, 4, false, strlen(payload), &limitExceeded));
  EXPECT_FALSE(limitExceeded);
  EXPECT_EQ(output.fileSize64(), strlen(payload));
  EXPECT_TRUE(output.close());
}

TEST_F(EpubSourceIdentityTest, CooperativeStreamJobProducesOneBoundedChunkPerStep) {
  constexpr char entryName[] = "cover.jpg";
  constexpr char payload[] = "0123456789abcdef";
  auto bytes = makeStoredZip({{entryName, payload, true}});
  Storage.setFile(EPUB_PATH, bytes);

  HalFile output;
  ASSERT_TRUE(Storage.openFileForWrite("TEST", "/out.bin", output));
  ZipStreamReadJob job;
  ASSERT_EQ(job.begin(EPUB_PATH, entryName, output, 4, 1024), ZipStreamReadJob::BeginStatus::Started);

  ZipStreamReadJob::StepStatus status = ZipStreamReadJob::StepStatus::InProgress;
  uint64_t previousSize = 0;
  size_t steps = 0;
  while (status == ZipStreamReadJob::StepStatus::InProgress && steps++ < 16U) {
    status = job.step();
    const uint64_t currentSize = output.fileSize64();
    EXPECT_GT(currentSize, previousSize);
    EXPECT_LE(currentSize - previousSize, 4U);
    previousSize = currentSize;
  }
  EXPECT_EQ(status, ZipStreamReadJob::StepStatus::Done);
  EXPECT_GE(steps, 4U);
  EXPECT_EQ(output.fileSize64(), strlen(payload));
  EXPECT_EQ(Storage.file("/out.bin"), std::vector<uint8_t>(payload, payload + strlen(payload)));
  EXPECT_TRUE(output.close());
}

TEST_F(EpubSourceIdentityTest, CooperativeStreamJobReadsStoredEntriesOnlyWhenExplicitlyAllowed) {
  constexpr char entryName[] = "chapter.xhtml";
  constexpr char payload[] = "0123456789abcdef";
  Storage.setFile(EPUB_PATH, makeStoredZip({{entryName, payload}}));

  HalFile output;
  ASSERT_TRUE(Storage.openFileForWrite("TEST", "/out.bin", output));
  ZipStreamReadJob job;
  EXPECT_EQ(job.begin(EPUB_PATH, entryName, output, 4, 1024), ZipStreamReadJob::BeginStatus::NotApplicable);
  ASSERT_EQ(job.begin(EPUB_PATH, entryName, output, 4, 1024, true), ZipStreamReadJob::BeginStatus::Started);

  auto status = ZipStreamReadJob::StepStatus::InProgress;
  size_t steps = 0;
  while (status == ZipStreamReadJob::StepStatus::InProgress) {
    status = job.step();
    ++steps;
  }
  EXPECT_EQ(status, ZipStreamReadJob::StepStatus::Done);
  EXPECT_EQ(steps, 4U);
  EXPECT_EQ(Storage.file("/out.bin"), std::vector<uint8_t>(payload, payload + strlen(payload)));
  EXPECT_TRUE(output.close());
}

TEST_F(EpubSourceIdentityTest, CooperativeStreamJobAcceptsAnEmptyStoredChapter) {
  constexpr char entryName[] = "empty.xhtml";
  Storage.setFile(EPUB_PATH, makeStoredZip({{entryName, ""}}));

  HalFile output;
  ASSERT_TRUE(Storage.openFileForWrite("TEST", "/out.bin", output));
  ZipStreamReadJob job;
  ASSERT_EQ(job.begin(EPUB_PATH, entryName, output, 4, 1024, true), ZipStreamReadJob::BeginStatus::Started);
  EXPECT_EQ(job.step(), ZipStreamReadJob::StepStatus::Done);
  EXPECT_EQ(output.fileSize64(), 0U);
  EXPECT_TRUE(output.close());
}

TEST_F(EpubSourceIdentityTest, CooperativeStreamJobAcceptsAnEmptyDeflatedChapter) {
  constexpr char entryName[] = "empty.xhtml";
  Storage.setFile(EPUB_PATH, makeStoredZip({{entryName, "", true}}));

  HalFile output;
  ASSERT_TRUE(Storage.openFileForWrite("TEST", "/out.bin", output));
  ZipStreamReadJob job;
  ASSERT_EQ(job.begin(EPUB_PATH, entryName, output, 4, 1024, true), ZipStreamReadJob::BeginStatus::Started);
  EXPECT_EQ(job.step(), ZipStreamReadJob::StepStatus::Done);
  EXPECT_EQ(output.fileSize64(), 0U);
  EXPECT_TRUE(output.close());
}

TEST_F(EpubSourceIdentityTest, CooperativeStreamJobRejectsOversizedOrInvalidEntryBeforeWriting) {
  constexpr char entryName[] = "cover.jpg";
  const auto valid = makeStoredZip({{entryName, "0123456789abcdef", true}});

  HalFile output;
  ASSERT_TRUE(Storage.openFileForWrite("TEST", "/out.bin", output));
  ZipStreamReadJob job;

  Storage.setFile(EPUB_PATH, valid);
  EXPECT_EQ(job.begin(EPUB_PATH, entryName, output, 4, 8), ZipStreamReadJob::BeginStatus::Error);
  EXPECT_EQ(output.fileSize64(), 0U);

  auto nameMismatch = valid;
  nameMismatch[30] ^= 0x01U;
  Storage.setFile(EPUB_PATH, std::move(nameMismatch));
  EXPECT_EQ(job.begin(EPUB_PATH, entryName, output, 4, 1024), ZipStreamReadJob::BeginStatus::Error);
  EXPECT_EQ(output.fileSize64(), 0U);

  auto encrypted = valid;
  const size_t central = get32(encrypted, encrypted.size() - 22U + 16U);
  put16(encrypted, 6, 1U);
  put16(encrypted, central + 8U, 1U);
  Storage.setFile(EPUB_PATH, std::move(encrypted));
  EXPECT_EQ(job.begin(EPUB_PATH, entryName, output, 4, 1024), ZipStreamReadJob::BeginStatus::Error);
  EXPECT_EQ(output.fileSize64(), 0U);
  EXPECT_TRUE(output.close());
}

TEST_F(EpubSourceIdentityTest, CooperativeStreamJobCancellationStopsBeforeTheNextChunk) {
  constexpr char entryName[] = "cover.jpg";
  auto bytes = makeStoredZip({{entryName, "0123456789abcdef", true}});
  Storage.setFile(EPUB_PATH, bytes);

  HalFile output;
  ASSERT_TRUE(Storage.openFileForWrite("TEST", "/out.bin", output));
  ZipStreamReadJob job;
  ASSERT_EQ(job.begin(EPUB_PATH, entryName, output, 4, 1024), ZipStreamReadJob::BeginStatus::Started);
  ASSERT_EQ(job.step(), ZipStreamReadJob::StepStatus::InProgress);
  EXPECT_EQ(output.fileSize64(), 4U);
  job.cancel();
  EXPECT_EQ(job.step(), ZipStreamReadJob::StepStatus::Error);
  EXPECT_EQ(output.fileSize64(), 4U);
  EXPECT_TRUE(output.close());
}

TEST_F(EpubSourceIdentityTest, CooperativeStreamLookupFindsALateEntryInBoundedSteps) {
  constexpr char entryName[] = "OPS/images/page.png";
  constexpr char payload[] = "0123456789abcdef";
  std::vector<StoredZipEntry> entries;
  for (size_t i = 0; i < 96U; ++i) {
    entries.emplace_back("OPS/filler/entry_" + std::to_string(i), "x");
  }
  entries.emplace_back(entryName, payload, true);
  Storage.setFile(EPUB_PATH, makeStoredZip(std::move(entries)));

  HalFile output;
  ASSERT_TRUE(Storage.openFileForWrite("TEST", "/late.bin", output));
  ZipStreamReadJob job;
  Storage.resetIoCounters();
  ASSERT_EQ(job.beginCooperativeLookup(EPUB_PATH, entryName, output, 4, 1024), ZipStreamReadJob::BeginStatus::Started);
  EXPECT_EQ(output.fileSize64(), 0U);
  EXPECT_LE(Storage.maxRead(), 1024U);

  for (size_t i = 0; i < 12U; ++i) {
    Storage.resetIoCounters();
    ASSERT_EQ(job.step(), ZipStreamReadJob::StepStatus::InProgress);
    EXPECT_EQ(output.fileSize64(), 0U);
    EXPECT_LE(Storage.readCalls(), 16U);
    EXPECT_LE(Storage.maxRead(), 255U);
  }
  ASSERT_EQ(job.step(), ZipStreamReadJob::StepStatus::InProgress);
  EXPECT_EQ(output.fileSize64(), 0U);

  ZipStreamReadJob::StepStatus status = ZipStreamReadJob::StepStatus::InProgress;
  size_t steps = 0;
  while (status == ZipStreamReadJob::StepStatus::InProgress && steps++ < 8U) status = job.step();
  EXPECT_EQ(status, ZipStreamReadJob::StepStatus::Done);
  EXPECT_EQ(Storage.file("/late.bin"), std::vector<uint8_t>(payload, payload + strlen(payload)));
  EXPECT_TRUE(output.close());
}

TEST_F(EpubSourceIdentityTest, CooperativeStreamLookupCanBeCancelledBeforePayloadRead) {
  constexpr char entryName[] = "OPS/images/page.png";
  std::vector<StoredZipEntry> entries;
  for (size_t i = 0; i < 24U; ++i) entries.emplace_back("OPS/filler/" + std::to_string(i), "x");
  entries.emplace_back(entryName, "payload", true);
  Storage.setFile(EPUB_PATH, makeStoredZip(std::move(entries)));

  HalFile output;
  ASSERT_TRUE(Storage.openFileForWrite("TEST", "/cancel_lookup.bin", output));
  ZipStreamReadJob job;
  ASSERT_EQ(job.beginCooperativeLookup(EPUB_PATH, entryName, output, 4, 1024), ZipStreamReadJob::BeginStatus::Started);
  ASSERT_EQ(job.step(), ZipStreamReadJob::StepStatus::InProgress);
  ASSERT_EQ(output.fileSize64(), 0U);
  job.cancel();
  EXPECT_EQ(job.step(), ZipStreamReadJob::StepStatus::Error);
  EXPECT_EQ(output.fileSize64(), 0U);
  EXPECT_TRUE(output.close());
}

TEST_F(EpubSourceIdentityTest, PageImagePreparationPublishesOnlyAfterBoundedStreamCompletes) {
  constexpr char entryName[] = "OPS/images/page.png";
  auto raster = makePngHeader(320, 480);
  raster.resize(9000U, 0x5AU);
  identify(makeStoredZip({{entryName, asString(raster), true}}));
  Epub epub(EPUB_PATH, "/.crosspoint");
  epub.setupCacheDir();
  ASSERT_TRUE(epub.bindCurrentSource());
  const std::string finalPath = epub.getCachePath() + "/img_0_0.png";

  ASSERT_EQ(epub.beginImagePreparation(entryName, finalPath), Epub::ImagePreparationStatus::InProgress);
  EXPECT_TRUE(epub.imagePreparationActive());
  EXPECT_FALSE(Storage.exists(finalPath.c_str()));
  EXPECT_TRUE(Storage.exists((finalPath + ".tmp").c_str()));

  ASSERT_EQ(epub.stepImagePreparation(), Epub::ImagePreparationStatus::InProgress);
  EXPECT_FALSE(Storage.exists(finalPath.c_str()));
  EXPECT_LE(Storage.file(finalPath + ".tmp").size(), 4096U);

  Epub::ImagePreparationStatus status = Epub::ImagePreparationStatus::InProgress;
  size_t steps = 1;
  while (status == Epub::ImagePreparationStatus::InProgress && steps++ < 16U) {
    status = epub.stepImagePreparation();
  }
  EXPECT_EQ(status, Epub::ImagePreparationStatus::Ready);
  EXPECT_FALSE(epub.imagePreparationActive());
  EXPECT_FALSE(Storage.exists((finalPath + ".tmp").c_str()));
  EXPECT_EQ(Storage.file(finalPath), raster);
  EXPECT_LE(Storage.maxRead(), 4096U);
}

TEST_F(EpubSourceIdentityTest, PageImagePreparationVerifiesLargeCentralDirectoryCooperatively) {
  constexpr char entryName[] = "OPS/images/page.png";
  auto raster = makePngHeader(320, 480);
  raster.resize(9000U, 0x5AU);
  std::vector<StoredZipEntry> entries;
  for (size_t i = 0; i < 96U; ++i) {
    entries.emplace_back("OPS/filler/" + std::string(48U, static_cast<char>('a' + i % 26U)) + std::to_string(i), "x");
  }
  entries.emplace_back(entryName, asString(raster), true);
  identify(makeStoredZip(std::move(entries)));
  Epub epub(EPUB_PATH, "/.crosspoint");
  epub.setupCacheDir();
  ASSERT_TRUE(epub.bindCurrentSource());
  const std::string finalPath = epub.getCachePath() + "/img_large_directory.png";

  ASSERT_EQ(epub.beginImagePreparation(entryName, finalPath), Epub::ImagePreparationStatus::InProgress);
  Epub::ImagePreparationStatus status = Epub::ImagePreparationStatus::InProgress;
  for (size_t i = 0; i < 12U; ++i) {
    Storage.resetIoCounters();
    status = epub.stepImagePreparation();
    ASSERT_EQ(status, Epub::ImagePreparationStatus::InProgress);
    EXPECT_EQ(Storage.file(finalPath + ".tmp").size(), 0U);
    EXPECT_LE(Storage.readCalls(), 16U);
    EXPECT_LE(Storage.maxRead(), 255U);
  }

  size_t completionSteps = 0;
  while (status == Epub::ImagePreparationStatus::InProgress && completionSteps++ < 32U) {
    status = epub.stepImagePreparation();
  }
  EXPECT_EQ(status, Epub::ImagePreparationStatus::Ready);
  EXPECT_FALSE(epub.imagePreparationActive());
  EXPECT_EQ(Storage.file(finalPath), raster);
}

TEST_F(EpubSourceIdentityTest, PageImagePreparationRejectsSourceChangedAfterPendingPublish) {
  constexpr char entryName[] = "OPS/images/page.png";
  auto raster = makePngHeader(320, 480);
  raster.resize(9000U, 0x5AU);
  identify(makeStoredZip({{entryName, asString(raster), true}}));
  Epub epub(EPUB_PATH, "/.crosspoint");
  epub.setupCacheDir();
  ASSERT_TRUE(epub.bindCurrentSource());
  const std::string finalPath = epub.getCachePath() + "/img_changed_source.png";

  ASSERT_EQ(epub.beginImagePreparation(entryName, finalPath), Epub::ImagePreparationStatus::InProgress);
  Epub::ImagePreparationStatus status = Epub::ImagePreparationStatus::InProgress;
  size_t steps = 0;
  while (!Storage.exists(finalPath.c_str()) && steps++ < 16U) {
    status = epub.stepImagePreparation();
    ASSERT_EQ(status, Epub::ImagePreparationStatus::InProgress);
  }
  ASSERT_TRUE(Storage.exists(finalPath.c_str()));
  ASSERT_TRUE(Storage.exists((finalPath + ".pending").c_str()));

  Storage.setFile(EPUB_PATH, makeStoredZip({{entryName, asString(raster), true}, {"OPS/changed.txt", "changed"}}));
  while (status == Epub::ImagePreparationStatus::InProgress && steps++ < 32U) {
    status = epub.stepImagePreparation();
  }
  EXPECT_EQ(status, Epub::ImagePreparationStatus::Error);
  EXPECT_FALSE(epub.imagePreparationActive());
  EXPECT_FALSE(Storage.exists(finalPath.c_str()));
  EXPECT_FALSE(Storage.exists((finalPath + ".tmp").c_str()));
  EXPECT_FALSE(Storage.exists((finalPath + ".pending").c_str()));
}

TEST_F(EpubSourceIdentityTest, CancellingPageImagePreparationRemovesOnlyScratchData) {
  constexpr char entryName[] = "OPS/images/page.png";
  auto raster = makePngHeader(320, 480);
  raster.resize(9000U, 0x5AU);
  identify(makeStoredZip({{entryName, asString(raster), true}}));
  Epub epub(EPUB_PATH, "/.crosspoint");
  epub.setupCacheDir();
  ASSERT_TRUE(epub.bindCurrentSource());
  const std::string finalPath = epub.getCachePath() + "/img_0_0.png";

  ASSERT_EQ(epub.beginImagePreparation(entryName, finalPath), Epub::ImagePreparationStatus::InProgress);
  ASSERT_EQ(epub.stepImagePreparation(), Epub::ImagePreparationStatus::InProgress);
  epub.cancelImagePreparation();

  EXPECT_FALSE(epub.imagePreparationActive());
  EXPECT_FALSE(Storage.exists(finalPath.c_str()));
  EXPECT_FALSE(Storage.exists((finalPath + ".tmp").c_str()));
}

TEST_F(EpubSourceIdentityTest, DeferringUnpublishedImageCancellationKeepsScratchUntilIdleCleanup) {
  constexpr char entryName[] = "OPS/images/page.png";
  auto raster = makePngHeader(320, 480);
  raster.resize(9000U, 0x5AU);
  identify(makeStoredZip({{entryName, asString(raster), true}}));
  Epub epub(EPUB_PATH, "/.crosspoint");
  epub.setupCacheDir();
  ASSERT_TRUE(epub.bindCurrentSource());
  const std::string finalPath = epub.getCachePath() + "/img_deferred_cancel.png";
  const std::string stagingPath = finalPath + ".tmp";

  ASSERT_EQ(epub.beginImagePreparation(entryName, finalPath), Epub::ImagePreparationStatus::InProgress);
  for (size_t step = 0; Storage.file(stagingPath).empty() && step < 4U; ++step) {
    ASSERT_EQ(epub.stepImagePreparation(), Epub::ImagePreparationStatus::InProgress);
  }
  ASSERT_FALSE(Storage.file(stagingPath).empty());
  const size_t abandonedBytes = Storage.file(stagingPath).size();

  EXPECT_TRUE(epub.deferImagePreparationCleanup());
  EXPECT_FALSE(epub.imagePreparationActive());
  EXPECT_TRUE(Storage.exists(stagingPath.c_str()));
  EXPECT_EQ(Storage.file(stagingPath).size(), abandonedBytes);
  EXPECT_FALSE(Storage.exists(finalPath.c_str()));
  EXPECT_FALSE(Storage.exists((finalPath + ".pending").c_str()));

  epub.cancelImagePreparation();
  EXPECT_FALSE(Storage.exists(stagingPath.c_str()));
}

TEST_F(EpubSourceIdentityTest, CancellingPageImageVerificationRollsBackPendingPublication) {
  constexpr char entryName[] = "OPS/images/page.png";
  auto raster = makePngHeader(320, 480);
  raster.resize(9000U, 0x5AU);
  identify(makeStoredZip({{entryName, asString(raster), true}}));
  Epub epub(EPUB_PATH, "/.crosspoint");
  epub.setupCacheDir();
  ASSERT_TRUE(epub.bindCurrentSource());
  const std::string finalPath = epub.getCachePath() + "/img_cancel_verify.png";

  ASSERT_EQ(epub.beginImagePreparation(entryName, finalPath), Epub::ImagePreparationStatus::InProgress);
  Epub::ImagePreparationStatus status = Epub::ImagePreparationStatus::InProgress;
  size_t steps = 0;
  while (!Storage.exists(finalPath.c_str()) && steps++ < 16U) {
    status = epub.stepImagePreparation();
    ASSERT_EQ(status, Epub::ImagePreparationStatus::InProgress);
  }
  ASSERT_TRUE(Storage.exists(finalPath.c_str()));
  ASSERT_TRUE(Storage.exists((finalPath + ".pending").c_str()));
  ASSERT_TRUE(epub.imagePreparationActive());

  epub.cancelImagePreparation();

  EXPECT_FALSE(epub.imagePreparationActive());
  EXPECT_FALSE(Storage.exists(finalPath.c_str()));
  EXPECT_FALSE(Storage.exists((finalPath + ".tmp").c_str()));
  EXPECT_FALSE(Storage.exists((finalPath + ".bak").c_str()));
  EXPECT_FALSE(Storage.exists((finalPath + ".pending").c_str()));
}

TEST_F(EpubSourceIdentityTest, PageImagePublicationRejectsCanonicalFileGrowthDuringDigest) {
  constexpr char entryName[] = "OPS/images/page.png";
  auto raster = makePngHeader(320, 480);
  raster.resize(9000U, 0x5AU);
  identify(makeStoredZip({{entryName, asString(raster), true}}));
  Epub epub(EPUB_PATH, "/.crosspoint");
  epub.setupCacheDir();
  ASSERT_TRUE(epub.bindCurrentSource());
  const std::string finalPath = epub.getCachePath() + "/img_grows_during_digest.png";

  ASSERT_EQ(epub.beginImagePreparation(entryName, finalPath), Epub::ImagePreparationStatus::InProgress);
  Epub::ImagePreparationStatus status = Epub::ImagePreparationStatus::InProgress;
  size_t steps = 0;
  while (!Storage.exists(finalPath.c_str()) && steps++ < 16U) {
    status = epub.stepImagePreparation();
    ASSERT_EQ(status, Epub::ImagePreparationStatus::InProgress);
  }
  ASSERT_TRUE(Storage.exists(finalPath.c_str()));
  ASSERT_TRUE(Storage.exists((finalPath + ".pending").c_str()));

  Storage.resetIoCounters();
  Storage.growOnReadCall(1);
  while (status == Epub::ImagePreparationStatus::InProgress && steps++ < 32U) {
    status = epub.stepImagePreparation();
  }
  EXPECT_EQ(status, Epub::ImagePreparationStatus::Error);
  EXPECT_FALSE(epub.imagePreparationActive());
  EXPECT_FALSE(Storage.exists(finalPath.c_str()));
  EXPECT_FALSE(Storage.exists((finalPath + ".tmp").c_str()));
  EXPECT_FALSE(Storage.exists((finalPath + ".pending").c_str()));
}

TEST_F(EpubSourceIdentityTest, PageImagePublicationRejectsRenameCorruptionAndRestoresOldFinal) {
  constexpr char entryName[] = "OPS/images/page.png";
  auto raster = makePngHeader(320, 480);
  raster.resize(9000U, 0x5AU);
  identify(makeStoredZip({{entryName, asString(raster), true}}));
  Epub epub(EPUB_PATH, "/.crosspoint");
  epub.setupCacheDir();
  ASSERT_TRUE(epub.bindCurrentSource());
  const std::string finalPath = epub.getCachePath() + "/img_corrupt_publish.png";
  const std::vector<uint8_t> oldFinal{'o', 'l', 'd'};
  Storage.setFile(finalPath, oldFinal);
  Storage.corruptRenameTo(finalPath);

  ASSERT_EQ(epub.beginImagePreparation(entryName, finalPath), Epub::ImagePreparationStatus::InProgress);
  Epub::ImagePreparationStatus status = Epub::ImagePreparationStatus::InProgress;
  size_t steps = 0;
  while (status == Epub::ImagePreparationStatus::InProgress && steps++ < 32U) {
    status = epub.stepImagePreparation();
  }
  EXPECT_EQ(status, Epub::ImagePreparationStatus::Error);
  EXPECT_FALSE(epub.imagePreparationActive());
  ASSERT_TRUE(Storage.exists(finalPath.c_str()));
  EXPECT_EQ(Storage.file(finalPath), oldFinal);
  EXPECT_FALSE(Storage.exists((finalPath + ".tmp").c_str()));
  EXPECT_FALSE(Storage.exists((finalPath + ".bak").c_str()));
  EXPECT_FALSE(Storage.exists((finalPath + ".pending").c_str()));
}

TEST_F(EpubSourceIdentityTest, PageImagePreparationRecoversPendingBackupBeforeReuse) {
  constexpr char entryName[] = "OPS/images/page.png";
  const auto raster = makePngHeader(320, 480);
  identify(makeStoredZip({{entryName, asString(raster), true}}));
  Epub epub(EPUB_PATH, "/.crosspoint");
  epub.setupCacheDir();
  ASSERT_TRUE(epub.bindCurrentSource());
  const std::string finalPath = epub.getCachePath() + "/img_recover_backup.png";
  const auto oldFinal = makePngHeader(100, 200);
  Storage.setFile(finalPath, raster);
  Storage.setFile(finalPath + ".bak", oldFinal);
  Storage.setFile(finalPath + ".pending", {'P'});

  EXPECT_EQ(epub.beginImagePreparation(entryName, finalPath), Epub::ImagePreparationStatus::NotNeeded);
  EXPECT_EQ(Storage.file(finalPath), oldFinal);
  EXPECT_FALSE(Storage.exists((finalPath + ".bak").c_str()));
  EXPECT_FALSE(Storage.exists((finalPath + ".pending").c_str()));
}

TEST_F(EpubSourceIdentityTest, PageImagePreparationDiscardsPendingFinalWithoutBackup) {
  constexpr char entryName[] = "OPS/images/page.png";
  auto raster = makePngHeader(320, 480);
  raster.resize(9000U, 0x5AU);
  identify(makeStoredZip({{entryName, asString(raster), true}}));
  Epub epub(EPUB_PATH, "/.crosspoint");
  epub.setupCacheDir();
  ASSERT_TRUE(epub.bindCurrentSource());
  const std::string finalPath = epub.getCachePath() + "/img_discard_pending.png";
  Storage.setFile(finalPath, raster);
  Storage.setFile(finalPath + ".pending", {'P'});

  ASSERT_EQ(epub.beginImagePreparation(entryName, finalPath), Epub::ImagePreparationStatus::InProgress);
  EXPECT_FALSE(Storage.exists(finalPath.c_str()));
  Epub::ImagePreparationStatus status = Epub::ImagePreparationStatus::InProgress;
  size_t steps = 0;
  while (status == Epub::ImagePreparationStatus::InProgress && steps++ < 32U) {
    status = epub.stepImagePreparation();
  }
  EXPECT_EQ(status, Epub::ImagePreparationStatus::Ready);
  EXPECT_EQ(Storage.file(finalPath), raster);
  EXPECT_FALSE(Storage.exists((finalPath + ".pending").c_str()));
}

TEST_F(EpubSourceIdentityTest, PageImagePublishFailureCleansUnpublishedScratch) {
  constexpr char entryName[] = "OPS/images/page.png";
  auto raster = makePngHeader(320, 480);
  raster.resize(9000U, 0x5AU);
  identify(makeStoredZip({{entryName, asString(raster), true}}));
  Epub epub(EPUB_PATH, "/.crosspoint");
  epub.setupCacheDir();
  ASSERT_TRUE(epub.bindCurrentSource());
  const std::string finalPath = epub.getCachePath() + "/img_publish_failure.png";
  Storage.failRenameTo(finalPath);

  ASSERT_EQ(epub.beginImagePreparation(entryName, finalPath), Epub::ImagePreparationStatus::InProgress);
  Epub::ImagePreparationStatus status = Epub::ImagePreparationStatus::InProgress;
  size_t steps = 0;
  while (status == Epub::ImagePreparationStatus::InProgress && steps++ < 16U) {
    status = epub.stepImagePreparation();
  }

  EXPECT_EQ(status, Epub::ImagePreparationStatus::Error);
  EXPECT_FALSE(epub.imagePreparationActive());
  EXPECT_FALSE(Storage.exists(finalPath.c_str()));
  EXPECT_FALSE(Storage.exists((finalPath + ".tmp").c_str()));
  EXPECT_FALSE(Storage.exists((finalPath + ".bak").c_str()));
}

TEST_F(EpubSourceIdentityTest, CodecCrcRejectsIdentityBitFlipInsteadOfCallingItMismatch) {
  const auto identity = identify(makeZip());
  SourceIdentityCodec::Encoded encoded;
  ASSERT_TRUE(SourceIdentityCodec::encode(identity, encoded));

  ZipFile::SourceIdentity decoded;
  ASSERT_EQ(SourceIdentityCodec::decode(encoded.data(), encoded.size(), decoded),
            SourceIdentityCodec::DecodeStatus::OK);
  EXPECT_EQ(decoded, identity);

  encoded[SourceIdentityCodec::PAYLOAD_OFFSET + 3] ^= 0x01U;
  EXPECT_EQ(SourceIdentityCodec::decode(encoded.data(), encoded.size(), decoded),
            SourceIdentityCodec::DecodeStatus::BAD_CRC);
}

TEST_F(EpubSourceIdentityTest, BookMetadataCacheValidatesAndReadsEveryEntry) {
  const auto identity = identify(makeZip());
  Storage.setFile(BOOK_CACHE_PATH, makeBookCache(identity));

  BookMetadataCache cache(CACHE_PATH);
  ASSERT_EQ(cache.load(identity), BookMetadataCache::LoadStatus::Loaded);
  EXPECT_EQ(cache.coreMetadata.title, "A safe title");
  EXPECT_EQ(cache.getSpineCount(), 1);
  EXPECT_EQ(cache.getTocCount(), 1);
  EXPECT_EQ(cache.getSpineEntry(0).href, "OPS/chapter.xhtml");
  EXPECT_EQ(cache.getSpineEntry(0).cumulativeSize, 1234U);
  EXPECT_EQ(cache.getSpineCumulativeSize(0), 1234U);
  EXPECT_EQ(cache.getSpineTocIndex(0), 0);
  EXPECT_EQ(cache.getTocEntry(0).title, "Chapter 1");
}

TEST_F(EpubSourceIdentityTest, BookMetadataCacheRejectsUnboundedMetadataBeforeAllocation) {
  const auto identity = identify(makeZip());
  auto bytes = makeBookCache(identity);
  constexpr size_t fixedHeaderSize = 1 + 4 + 2 + 2 + SourceIdentityCodec::PAYLOAD_SIZE + 4;
  overwritePod(bytes, fixedHeaderSize, UINT32_MAX);
  Storage.setFile(BOOK_CACHE_PATH, std::move(bytes));

  BookMetadataCache cache(CACHE_PATH);
  EXPECT_EQ(cache.load(identity), BookMetadataCache::LoadStatus::Invalid);
  EXPECT_LE(Storage.maxRead(), 128U);
}

TEST_F(EpubSourceIdentityTest, BookMetadataCacheRejectsCorruptLutAndEntryLengths) {
  const auto identity = identify(makeZip());
  auto badLut = makeBookCache(identity);
  uint32_t lutOffset = 0;
  memcpy(&lutOffset, badLut.data() + 1, sizeof(lutOffset));
  overwritePod(badLut, lutOffset, uint32_t{0});
  Storage.setFile(BOOK_CACHE_PATH, std::move(badLut));
  BookMetadataCache badLutCache(CACHE_PATH);
  EXPECT_EQ(badLutCache.load(identity), BookMetadataCache::LoadStatus::Invalid);

  auto badLength = makeBookCache(identity);
  memcpy(&lutOffset, badLength.data() + 1, sizeof(lutOffset));
  uint32_t spineOffset = 0;
  memcpy(&spineOffset, badLength.data() + lutOffset, sizeof(spineOffset));
  overwritePod(badLength, spineOffset, UINT32_MAX);
  Storage.setFile(BOOK_CACHE_PATH, std::move(badLength));
  BookMetadataCache badLengthCache(CACHE_PATH);
  EXPECT_EQ(badLengthCache.load(identity), BookMetadataCache::LoadStatus::Invalid);
}

TEST_F(EpubSourceIdentityTest, BookMetadataCacheRejectsInvalidEntryReferencesAndOrdering) {
  const auto identity = identify(makeZip());
  auto badLinear = makeBookCache(identity);
  uint32_t lutOffset = 0;
  memcpy(&lutOffset, badLinear.data() + 1, sizeof(lutOffset));
  uint32_t spineOffset = 0;
  memcpy(&spineOffset, badLinear.data() + lutOffset, sizeof(spineOffset));
  const size_t linearOffset = spineOffset + sizeof(uint32_t) + strlen("OPS/chapter.xhtml");
  overwritePod(badLinear, linearOffset, uint8_t{2});
  Storage.setFile(BOOK_CACHE_PATH, std::move(badLinear));
  BookMetadataCache badLinearCache(CACHE_PATH);
  EXPECT_EQ(badLinearCache.load(identity), BookMetadataCache::LoadStatus::Invalid);

  auto zeroLevel = makeBookCache(identity);
  const size_t tocLevelOffset = zeroLevel.size() - sizeof(uint32_t) - sizeof(int16_t) - sizeof(uint8_t);
  overwritePod(zeroLevel, tocLevelOffset, uint8_t{0});
  Storage.setFile(BOOK_CACHE_PATH, std::move(zeroLevel));
  BookMetadataCache zeroLevelCache(CACHE_PATH);
  EXPECT_EQ(zeroLevelCache.load(identity), BookMetadataCache::LoadStatus::Invalid);

  auto badReference = makeBookCache(identity);
  memcpy(&lutOffset, badReference.data() + 1, sizeof(lutOffset));
  const size_t spineIndexOffset = badReference.size() - sizeof(uint32_t) - sizeof(int16_t);
  ASSERT_LT(spineIndexOffset, badReference.size());
  overwritePod(badReference, spineIndexOffset, int16_t{2});
  Storage.setFile(BOOK_CACHE_PATH, std::move(badReference));
  BookMetadataCache badReferenceCache(CACHE_PATH);
  EXPECT_EQ(badReferenceCache.load(identity), BookMetadataCache::LoadStatus::Invalid);

  auto overlapping = makeBookCache(identity);
  memcpy(&lutOffset, overlapping.data() + 1, sizeof(lutOffset));
  memcpy(&spineOffset, overlapping.data() + lutOffset, sizeof(spineOffset));
  overwritePod(overlapping, lutOffset + sizeof(uint32_t), spineOffset);
  Storage.setFile(BOOK_CACHE_PATH, std::move(overlapping));
  BookMetadataCache overlappingCache(CACHE_PATH);
  EXPECT_EQ(overlappingCache.load(identity), BookMetadataCache::LoadStatus::Invalid);
}

TEST_F(EpubSourceIdentityTest, BookMetadataGetterFailsClosedIfFileChangesAfterLoad) {
  const auto identity = identify(makeZip());
  auto bytes = makeBookCache(identity);
  uint32_t lutOffset = 0;
  memcpy(&lutOffset, bytes.data() + 1, sizeof(lutOffset));
  uint32_t spineOffset = 0;
  memcpy(&spineOffset, bytes.data() + lutOffset, sizeof(spineOffset));
  Storage.setFile(BOOK_CACHE_PATH, std::move(bytes));

  BookMetadataCache cache(CACHE_PATH);
  ASSERT_EQ(cache.load(identity), BookMetadataCache::LoadStatus::Loaded);
  overwritePod(Storage.mutableFile(BOOK_CACHE_PATH), spineOffset, UINT32_MAX);
  EXPECT_TRUE(cache.getSpineEntry(0).href.empty());
  EXPECT_FALSE(cache.isLoaded());
  EXPECT_EQ(cache.getLastLoadStatus(), BookMetadataCache::LoadStatus::Invalid);
}

TEST_F(EpubSourceIdentityTest, CheckedProgressNeverInventsZeroAfterMetadataReadFailure) {
  const auto identity = identify(makeZip());
  Epub epub(EPUB_PATH, "/.crosspoint");
  const std::string cachePath = epub.getCachePath();
  ASSERT_TRUE(Storage.mkdir(cachePath.c_str()));
  ASSERT_TRUE(epub.bindCurrentSource());
  auto bookCache = makeBookCache(identity);
  uint32_t lutOffset = 0;
  memcpy(&lutOffset, bookCache.data() + 1, sizeof(lutOffset));
  uint32_t spineOffset = 0;
  memcpy(&spineOffset, bookCache.data() + lutOffset, sizeof(spineOffset));
  Storage.setFile(cachePath + "/book.bin", std::move(bookCache));
  ASSERT_TRUE(epub.load(false, true));

  float progress = 0.0F;
  ASSERT_TRUE(epub.calculateProgressChecked(0, 0.5F, progress));
  EXPECT_FLOAT_EQ(progress, 0.5F);

  overwritePod(Storage.mutableFile(cachePath + "/book.bin"), spineOffset, UINT32_MAX);
  progress = 0.75F;
  EXPECT_FALSE(epub.calculateProgressChecked(0, 0.5F, progress));
  EXPECT_FLOAT_EQ(progress, 0.0F);
}

TEST_F(EpubSourceIdentityTest, BookMetadataCacheDistinguishesNewerFromCorruptWithoutDeletingEither) {
  const auto identity = identify(makeZip());
  auto legacy = makeBookCache(identity);
  legacy.front() = 10;
  Storage.setFile(BOOK_CACHE_PATH, legacy);
  BookMetadataCache legacyCache(CACHE_PATH);
  EXPECT_EQ(legacyCache.load(identity), BookMetadataCache::LoadStatus::LegacyVersion);
  EXPECT_EQ(Storage.file(BOOK_CACHE_PATH), legacy);

  auto newer = makeBookCache(identity);
  newer.front() = 13;
  Storage.setFile(BOOK_CACHE_PATH, newer);
  BookMetadataCache newerCache(CACHE_PATH);
  EXPECT_EQ(newerCache.load(identity), BookMetadataCache::LoadStatus::NewerVersion);
  EXPECT_EQ(Storage.file(BOOK_CACHE_PATH), newer);

  std::vector<uint8_t> truncated = {12, 0, 0};
  Storage.setFile(BOOK_CACHE_PATH, truncated);
  BookMetadataCache truncatedCache(CACHE_PATH);
  EXPECT_EQ(truncatedCache.load(identity), BookMetadataCache::LoadStatus::Invalid);
  EXPECT_EQ(Storage.file(BOOK_CACHE_PATH), truncated);
}

TEST_F(EpubSourceIdentityTest, BookMetadataCacheValidatesAcrossBoundedLutChunks) {
  const auto identity = identify(makeZip());
  Storage.setFile(BOOK_CACHE_PATH, makeSpineOnlyBookCache(identity, 65));

  BookMetadataCache cache(CACHE_PATH);
  ASSERT_EQ(cache.load(identity), BookMetadataCache::LoadStatus::Loaded);
  EXPECT_EQ(cache.getSpineCount(), 65);
  EXPECT_EQ(cache.getSpineEntry(0).href, "chapter-0");
  EXPECT_EQ(cache.getSpineEntry(64).href, "chapter-64");
  EXPECT_LE(Storage.maxRead(), 65U * sizeof(uint32_t));
}

TEST_F(EpubSourceIdentityTest, BookMetadataCacheValidationYieldsAtTheRequestedEntryBudget) {
  const auto identity = identify(makeZip());
  Storage.setFile(BOOK_CACHE_PATH, makeSpineOnlyBookCache(identity, 65));

  BookMetadataCache cache(CACHE_PATH);
  ASSERT_EQ(cache.beginLoad(identity), BookMetadataCache::LoadStepResult::InProgress);
  size_t steps = 0;
  auto result = BookMetadataCache::LoadStepResult::InProgress;
  while (result == BookMetadataCache::LoadStepResult::InProgress) {
    result = cache.stepLoad(8);
    ++steps;
  }

  EXPECT_EQ(result, BookMetadataCache::LoadStepResult::Loaded);
  EXPECT_EQ(steps, 9U);
  EXPECT_TRUE(cache.isLoaded());
  EXPECT_EQ(cache.getSpineEntry(64).href, "chapter-64");
  EXPECT_LE(Storage.maxRead(), 65U * sizeof(uint32_t));
}

TEST_F(EpubSourceIdentityTest, BookMetadataCacheValidationCanBeCancelledBetweenEntries) {
  const auto identity = identify(makeZip());
  Storage.setFile(BOOK_CACHE_PATH, makeSpineOnlyBookCache(identity, 65));

  BookMetadataCache cache(CACHE_PATH);
  ASSERT_EQ(cache.beginLoad(identity), BookMetadataCache::LoadStepResult::InProgress);
  ASSERT_EQ(cache.stepLoad(1), BookMetadataCache::LoadStepResult::InProgress);
  cache.cancelLoad();

  EXPECT_FALSE(cache.isLoaded());
  EXPECT_EQ(cache.stepLoad(1), BookMetadataCache::LoadStepResult::Error);
}

TEST_F(EpubSourceIdentityTest, BookMetadataCacheRejectsCorruptSpineScratchBeforeTocPass) {
  BookMetadataCache cache(CACHE_PATH);
  ASSERT_TRUE(cache.beginWrite());
  ASSERT_TRUE(cache.beginContentOpfPass());
  cache.createSpineEntry("a.xht");
  ASSERT_TRUE(cache.endContentOpfPass());

  auto& scratch = Storage.mutableFile(std::string(CACHE_PATH) + "/spine.bin.tmp");
  ASSERT_GE(scratch.size(), sizeof(uint32_t));
  overwritePod(scratch, 0, UINT32_MAX);

  EXPECT_FALSE(cache.beginTocPass());
  EXPECT_LE(Storage.maxRead(), sizeof(uint32_t));
  EXPECT_EQ(Storage.invalidOperationCount(), 0U);
}

TEST_F(EpubSourceIdentityTest, BookMetadataCacheValidatesScratchBeforeReplacingExistingCache) {
  const auto identity = identify(makeZip());
  BookMetadataCache cache(CACHE_PATH);
  ASSERT_TRUE(cache.beginWrite());
  ASSERT_TRUE(cache.beginContentOpfPass());
  cache.createSpineEntry("a.xht");
  ASSERT_TRUE(cache.endContentOpfPass());
  ASSERT_TRUE(cache.beginTocPass());
  cache.createTocEntry("Chapter", "a.xht", "", 1);
  ASSERT_TRUE(cache.endTocPass());
  ASSERT_TRUE(cache.endWrite());

  auto& scratch = Storage.mutableFile(std::string(CACHE_PATH) + "/toc.bin.tmp");
  ASSERT_GE(scratch.size(), sizeof(uint32_t));
  overwritePod(scratch, 0, UINT32_MAX);
  const std::vector<uint8_t> existingCache = {0xCAU, 0xFEU, 0xBAU, 0xBEU};
  Storage.setFile(BOOK_CACHE_PATH, existingCache);

  BookMetadataCache::BookMetadata metadata;
  EXPECT_FALSE(cache.buildBookBin(EPUB_PATH, metadata, identity));
  EXPECT_EQ(Storage.file(BOOK_CACHE_PATH), existingCache);
  EXPECT_EQ(Storage.invalidOperationCount(), 0U);
}

TEST_F(EpubSourceIdentityTest, BookMetadataCacheBuildCanYieldAndCancelWithoutLeavingPartialOutput) {
  const auto identity = identify(makeZip());
  BookMetadataCache cache(CACHE_PATH);
  ASSERT_TRUE(cache.beginWrite());
  ASSERT_TRUE(cache.beginContentOpfPass());
  cache.createSpineEntry("a.xht");
  ASSERT_TRUE(cache.endContentOpfPass());
  ASSERT_TRUE(cache.beginTocPass());
  cache.createTocEntry("Chapter", "a.xht", "", 1);
  ASSERT_TRUE(cache.endTocPass());
  ASSERT_TRUE(cache.endWrite());

  const std::vector<uint8_t> existingCache = {0xCAU, 0xFEU, 0xBAU, 0xBEU};
  Storage.setFile(BOOK_CACHE_PATH, existingCache);
  const std::string stagingPath = std::string(CACHE_PATH) + "/book.bin.tmp";

  BookMetadataCache::BookMetadata metadata;
  ASSERT_TRUE(cache.beginBuildBookBin(EPUB_PATH, metadata, identity));
  BookMetadataCache::BuildStepResult result = BookMetadataCache::BuildStepResult::InProgress;
  for (size_t step = 0; step < 16 && !Storage.exists(stagingPath.c_str()); ++step) {
    result = cache.stepBuildBookBin(1);
    ASSERT_EQ(result, BookMetadataCache::BuildStepResult::InProgress);
  }
  ASSERT_TRUE(Storage.exists(stagingPath.c_str()));
  EXPECT_EQ(Storage.file(BOOK_CACHE_PATH), existingCache);
  EXPECT_TRUE(cache.isBuildingBookBin());

  cache.cancelBuildBookBin();
  EXPECT_FALSE(cache.isBuildingBookBin());
  EXPECT_EQ(Storage.file(BOOK_CACHE_PATH), existingCache);
  EXPECT_FALSE(Storage.exists(stagingPath.c_str()));
  EXPECT_EQ(Storage.invalidOperationCount(), 0U);
}

TEST_F(EpubSourceIdentityTest, BookMetadataCacheLoadRecoversBackupAfterInterruptedPublish) {
  const auto identity = identify(makeZip());
  Storage.setFile(std::string(CACHE_PATH) + "/book.bin.bak", makeBookCache(identity));

  BookMetadataCache cache(CACHE_PATH);
  EXPECT_EQ(cache.load(identity), BookMetadataCache::LoadStatus::Loaded);
  EXPECT_TRUE(Storage.exists(BOOK_CACHE_PATH));
  EXPECT_FALSE(Storage.exists((std::string(CACHE_PATH) + "/book.bin.bak").c_str()));
  EXPECT_EQ(Storage.invalidOperationCount(), 0U);
}

TEST_F(EpubSourceIdentityTest, BookMetadataCacheBatchSizeLookupPreservesCumulativeSpineSizes) {
  const auto identity = identify(makeZip());
  BookMetadataCache cache(CACHE_PATH);
  ASSERT_TRUE(cache.beginWrite());
  ASSERT_TRUE(cache.beginContentOpfPass());
  constexpr uint16_t spineCount = 400;
  for (uint16_t index = 0; index < spineCount; ++index) cache.createSpineEntry("a.xht");
  ASSERT_TRUE(cache.endContentOpfPass());
  ASSERT_TRUE(cache.beginTocPass());
  ASSERT_TRUE(cache.endTocPass());
  ASSERT_TRUE(cache.endWrite());

  BookMetadataCache::BookMetadata metadata;
  ASSERT_TRUE(cache.buildBookBin(EPUB_PATH, metadata, identity));
  ASSERT_EQ(cache.load(identity), BookMetadataCache::LoadStatus::Loaded);
  ASSERT_EQ(cache.getSpineCount(), spineCount);
  EXPECT_EQ(cache.getSpineCumulativeSize(spineCount - 1), static_cast<uint32_t>(spineCount) * 20U);
  EXPECT_EQ(Storage.invalidOperationCount(), 0U);
}

TEST_F(EpubSourceIdentityTest, CssCacheBuildIsVerifiedAndIdempotent) {
  const auto identity = identify(makeCssTestEpub());
  Epub epub(EPUB_PATH, "/.crosspoint");
  const std::string cachePath = epub.getCachePath();
  ASSERT_TRUE(Storage.mkdir(cachePath.c_str()));
  ASSERT_TRUE(epub.bindCurrentSource());
  Storage.setFile(cachePath + "/book.bin", makeBookCache(identity));
  ASSERT_TRUE(epub.load(false, true));

  const std::string sectionsPath = cachePath + "/sections";
  const std::string sectionSentinel = sectionsPath + "/section_0.bin";
  ASSERT_TRUE(Storage.mkdir(sectionsPath.c_str()));
  Storage.setFile(sectionSentinel, {0xAA});

  ASSERT_TRUE(epub.ensureCssCache());
  EXPECT_FALSE(Storage.exists(sectionSentinel.c_str()));
  const auto& firstCssCache = Storage.file(cachePath + "/css_rules.cache");
  ASSERT_GE(firstCssCache.size(), 3U);
  EXPECT_GT(static_cast<uint16_t>(firstCssCache[1]) | (static_cast<uint16_t>(firstCssCache[2]) << 8U), 0U);
  ASSERT_TRUE(epub.getCssParser()->loadFromCache());
  const CssStyle rebuiltStyle = epub.getCssParser()->resolveStyle("p", "note");
  EXPECT_TRUE(rebuiltStyle.defined.textAlign);
  EXPECT_EQ(rebuiltStyle.textAlign, CssTextAlign::Right);
  epub.getCssParser()->clear();

  ASSERT_TRUE(Storage.mkdir(sectionsPath.c_str()));
  Storage.setFile(sectionSentinel, {0xBB});
  ASSERT_TRUE(epub.ensureCssCache());
  EXPECT_TRUE(Storage.exists(sectionSentinel.c_str()));

  auto& cssCache = Storage.mutableFile(cachePath + "/css_rules.cache");
  ASSERT_FALSE(cssCache.empty());
  cssCache.front() ^= 0xFFU;
  ASSERT_TRUE(epub.ensureCssCache());
  EXPECT_FALSE(Storage.exists(sectionSentinel.c_str()));
  ASSERT_TRUE(epub.getCssParser()->loadFromCache());
  EXPECT_EQ(epub.getCssParser()->resolveStyle("p", "note").textAlign, CssTextAlign::Right);
}

TEST_F(EpubSourceIdentityTest, FastMetadataWithoutCoverWritesVerifiedNoCoverMarker) {
  identify(makeCssTestEpub());
  Epub epub(EPUB_PATH, "/.crosspoint");
  BookMetadataCache::BookMetadata metadata;
  ASSERT_TRUE(epub.readCoreMetadata(metadata));
  ASSERT_TRUE(metadata.coverItemHref.empty());

  constexpr int thumbnailHeight = 120;
  EXPECT_FALSE(epub.generateThumbBmp(thumbnailHeight));
  EXPECT_TRUE(Storage.exists((epub.getThumbBmpPath(thumbnailHeight) + ".nocover").c_str()));
}

TEST_F(EpubSourceIdentityTest, CoreMetadataReadYieldsAndCanBeCancelledBeforeRetry) {
  const auto identity = identify(makeCssTestEpub(std::string(5000U, 'x')));
  Epub epub(EPUB_PATH, "/.crosspoint");
  BookMetadataCache::BookMetadata metadata;
  ASSERT_TRUE(epub.beginCoreMetadataRead());
  ASSERT_TRUE(epub.isReadingCoreMetadata());
  EXPECT_EQ(epub.stepCoreMetadataRead(metadata), Epub::CoreMetadataStepResult::InProgress);
  epub.cancelCoreMetadataRead();
  EXPECT_FALSE(epub.isReadingCoreMetadata());
  EXPECT_EQ(epub.stepCoreMetadataRead(metadata), Epub::CoreMetadataStepResult::Error);

  ASSERT_TRUE(epub.beginCoreMetadataRead());
  auto result = Epub::CoreMetadataStepResult::InProgress;
  size_t steps = 0;
  while (result == Epub::CoreMetadataStepResult::InProgress && steps++ < 64U) {
    result = epub.stepCoreMetadataRead(metadata);
  }
  EXPECT_EQ(result, Epub::CoreMetadataStepResult::Loaded);
  EXPECT_GT(steps, 4U);
  EXPECT_EQ(metadata.title, "CSS test");
  EXPECT_FALSE(epub.isReadingCoreMetadata());
  RawSourceIdentityHandoff handoff;
  ASSERT_TRUE(epub.getSourceIdentityHandoff(handoff));
  EXPECT_EQ(handoff.path, EPUB_PATH);
  EXPECT_EQ(handoff.identity, identity);
  EXPECT_FALSE(handoff.identity.isRawFile());
}

TEST_F(EpubSourceIdentityTest, CoreMetadataReadRejectsSourceReplacementBeforePublishingResult) {
  identify(makeCssTestEpub());
  Epub epub(EPUB_PATH, "/.crosspoint");
  BookMetadataCache::BookMetadata metadata;
  ASSERT_TRUE(epub.beginCoreMetadataRead());
  ASSERT_EQ(epub.stepCoreMetadataRead(metadata), Epub::CoreMetadataStepResult::InProgress);

  Storage.setFile(EPUB_PATH, makeCssTestEpub(std::string(5000U, 'y')));
  auto result = Epub::CoreMetadataStepResult::InProgress;
  for (size_t steps = 0; result == Epub::CoreMetadataStepResult::InProgress && steps < 64U; ++steps) {
    result = epub.stepCoreMetadataRead(metadata);
  }
  EXPECT_EQ(result, Epub::CoreMetadataStepResult::Error);
  EXPECT_TRUE(metadata.title.empty());
  EXPECT_FALSE(epub.isReadingCoreMetadata());
  RawSourceIdentityHandoff handoff;
  EXPECT_FALSE(epub.getSourceIdentityHandoff(handoff));
}

TEST_F(EpubSourceIdentityTest, ThumbnailPreparationRequiresCallerToPumpMissingCoreMetadata) {
  identify(
      makeGuideCoverEpub(R"(<html><body><img src="images/cover.jpg"/></body></html>)", true, std::string(9000U, 'J')));
  Epub epub(EPUB_PATH, "/.crosspoint");

  EXPECT_EQ(epub.beginThumbnailPreparation({true, false, true}), Epub::ThumbnailPreparationStatus::NeedsCoreMetadata);
  EXPECT_FALSE(epub.thumbnailPreparationActive());
  EXPECT_FALSE(epub.hasPreparedCoreMetadata());
}

TEST_F(EpubSourceIdentityTest, ReadCoreMetadataServesVerifiedBookCache) {
  const auto identity = identify(makeCssTestEpub());
  Epub epub(EPUB_PATH, "/.crosspoint");
  const std::string cachePath = epub.getCachePath();
  ASSERT_TRUE(Storage.mkdir(cachePath.c_str()));
  // makeBookCache persists a title that differs from the OPF's ("CSS test"),
  // so the returned values prove the cache was served instead of an OPF parse.
  Storage.setFile(cachePath + "/book.bin", makeBookCache(identity));

  BookMetadataCache::BookMetadata metadata;
  ASSERT_TRUE(epub.readCoreMetadata(metadata));
  EXPECT_EQ(metadata.title, "A safe title");
  EXPECT_EQ(metadata.author, "An author");
  EXPECT_EQ(metadata.coverItemHref, "cover.xhtml");
}

TEST_F(EpubSourceIdentityTest, ReaderCacheInspectionReusesPreparedCoreMetadata) {
  const auto identity = identify(makeCssTestEpub());
  Epub epub(EPUB_PATH, "/.crosspoint");
  const std::string cachePath = epub.getCachePath();
  ASSERT_TRUE(Storage.mkdir(cachePath.c_str()));
  Storage.setFile(cachePath + "/book.bin", makeBookCache(identity));

  BookMetadataCache::BookMetadata metadata;
  ASSERT_TRUE(epub.readCoreMetadata(metadata));
  Storage.resetIoCounters();

  EXPECT_EQ(epub.beginCacheInspection(), BookMetadataCache::LoadStepResult::Loaded);
  EXPECT_EQ(Storage.openReadAttemptsFor(cachePath + "/book.bin"), 0U);
}

TEST_F(EpubSourceIdentityTest, LoadReusesMetadataCacheAlreadyInspectedForTheSameSource) {
  const auto identity = identify(makeCssTestEpub());
  Epub epub(EPUB_PATH, "/.crosspoint");
  const std::string cachePath = epub.getCachePath();
  ASSERT_TRUE(Storage.mkdir(cachePath.c_str()));
  ASSERT_TRUE(epub.bindCurrentSource());
  const std::string bookCachePath = cachePath + "/book.bin";
  Storage.setFile(bookCachePath, makeBookCache(identity));

  ASSERT_EQ(epub.inspectCache(), BookMetadataCache::LoadStatus::Loaded);
  ASSERT_EQ(Storage.openReadAttemptsFor(bookCachePath), 1U);
  ASSERT_TRUE(epub.load(false, true));
  EXPECT_EQ(Storage.openReadAttemptsFor(bookCachePath), 1U);
  EXPECT_EQ(epub.getTitle(), "A safe title");
}

TEST_F(EpubSourceIdentityTest, ReadCoreMetadataFallsBackWhenBookCacheInvalid) {
  const auto identity = identify(makeCssTestEpub());
  Epub epub(EPUB_PATH, "/.crosspoint");
  const std::string cachePath = epub.getCachePath();
  ASSERT_TRUE(Storage.mkdir(cachePath.c_str()));
  auto cache = makeBookCache(identity);
  cache.back() ^= 0xFFU;  // corrupt the commit marker
  Storage.setFile(cachePath + "/book.bin", std::move(cache));

  BookMetadataCache::BookMetadata metadata;
  ASSERT_TRUE(epub.readCoreMetadata(metadata));
  EXPECT_EQ(metadata.title, "CSS test");
  EXPECT_TRUE(metadata.coverItemHref.empty());
}

TEST_F(EpubSourceIdentityTest, ReadCoreMetadataWithEmptyCachedCoverFallsBackAndWritesNoCoverMarker) {
  const auto identity = identify(makeCssTestEpub());
  Epub epub(EPUB_PATH, "/.crosspoint");
  const std::string cachePath = epub.getCachePath();
  ASSERT_TRUE(Storage.mkdir(cachePath.c_str()));
  Storage.setFile(cachePath + "/book.bin", makeBookCache(identity, /*withCover=*/false));

  // An empty cached cover may be a transient guide-page failure rather than a
  // verified no-cover result, so the OPF parse must run again (cached title
  // "A safe title" proves it did not).
  BookMetadataCache::BookMetadata metadata;
  ASSERT_TRUE(epub.readCoreMetadata(metadata));
  EXPECT_TRUE(metadata.coverItemHref.empty());
  EXPECT_EQ(metadata.title, "CSS test");

  constexpr int thumbnailHeight = 120;
  EXPECT_FALSE(epub.generateThumbBmp(thumbnailHeight));
  EXPECT_TRUE(Storage.exists((epub.getThumbBmpPath(thumbnailHeight) + ".nocover").c_str()));
}

TEST_F(EpubSourceIdentityTest, EmptyCachedCoverDoesNotHideReParsedGuideCover) {
  const auto identity = identify(makeGuideCoverEpub(R"(<html><body><img src="images/cover.jpg"/></body></html>)"));
  ThumbnailConverterStub::reset(true);
  Epub epub(EPUB_PATH, "/.crosspoint");
  const std::string cachePath = epub.getCachePath();
  ASSERT_TRUE(Storage.mkdir(cachePath.c_str()));
  Storage.setFile(cachePath + "/book.bin", makeBookCache(identity, /*withCover=*/false));

  // The stale cache baked in an empty cover (e.g. after a transient guide-page
  // read failure), but the fallback parse resolves the real guide cover. The
  // thumbnail must use the re-parsed result, not the stale cache.
  BookMetadataCache::BookMetadata metadata;
  ASSERT_TRUE(epub.readCoreMetadata(metadata));
  EXPECT_EQ(metadata.coverItemHref, "OPS/images/cover.jpg");

  constexpr int thumbnailHeight = 120;
  EXPECT_TRUE(epub.generateThumbBmp(thumbnailHeight));
  EXPECT_TRUE(Storage.exists(epub.getThumbBmpPath(thumbnailHeight).c_str()));
  EXPECT_FALSE(Storage.exists((epub.getThumbBmpPath(thumbnailHeight) + ".nocover").c_str()));
}

TEST_F(EpubSourceIdentityTest, ExtractsSharedAndCarouselThumbnailsIntoIndependentCaches) {
  const auto shared = makeValidBmpSized(Epub::SHARED_THUMB_WIDTH, Epub::SHARED_THUMB_HEIGHT, 0x11U);
  const auto carousel = makeValidBmpSized(Epub::CAROUSEL_THUMB_WIDTH, Epub::CAROUSEL_THUMB_HEIGHT, 0x22U);
  identify(makeStoredZip(
      {{Epub::sharedThumbnailEntry(), asString(shared)},
       {Epub::carouselThumbnailEntry(Epub::CAROUSEL_THUMB_WIDTH, Epub::CAROUSEL_THUMB_HEIGHT), asString(carousel)}}));

  Epub epub(EPUB_PATH, "/.crosspoint");
  EXPECT_EQ(epub.ensureSharedThumbnail(Epub::ThumbnailMode::EmbeddedOnly), Epub::ThumbnailStatus::Ready);
  EXPECT_EQ(epub.ensureCarouselThumbnail(Epub::CAROUSEL_THUMB_WIDTH, Epub::CAROUSEL_THUMB_HEIGHT,
                                         Epub::ThumbnailMode::EmbeddedOnly),
            Epub::ThumbnailStatus::Ready);
  EXPECT_EQ(Storage.file(epub.getThumbBmpPath(Epub::SHARED_THUMB_HEIGHT)), shared);
  EXPECT_EQ(Storage.file(epub.getThumbBmpPath(Epub::CAROUSEL_THUMB_HEIGHT)), carousel);
  EXPECT_TRUE(Storage.exists((epub.getThumbBmpPath(Epub::SHARED_THUMB_HEIGHT) + ".identity").c_str()));
  EXPECT_TRUE(Storage.exists((epub.getThumbBmpPath(Epub::CAROUSEL_THUMB_HEIGHT) + ".fit-v2.identity").c_str()));
}

TEST_F(EpubSourceIdentityTest, ExtractsAspectPreservingEmbeddedThumbnails) {
  const auto shared = makeValidBmpSized(240, Epub::SHARED_THUMB_HEIGHT, 0x11U);
  const auto carousel = makeValidBmpSized(Epub::CAROUSEL_THUMB_WIDTH, 273, 0x22U);
  identify(makeStoredZip(
      {{Epub::sharedThumbnailEntry(), asString(shared)},
       {Epub::carouselThumbnailEntry(Epub::CAROUSEL_THUMB_WIDTH, Epub::CAROUSEL_THUMB_HEIGHT), asString(carousel)}}));

  Epub epub(EPUB_PATH, "/.crosspoint");
  EXPECT_EQ(epub.ensureSharedThumbnail(Epub::ThumbnailMode::EmbeddedOnly), Epub::ThumbnailStatus::Ready);
  EXPECT_EQ(epub.ensureCarouselThumbnail(Epub::CAROUSEL_THUMB_WIDTH, Epub::CAROUSEL_THUMB_HEIGHT,
                                         Epub::ThumbnailMode::EmbeddedOnly),
            Epub::ThumbnailStatus::Ready);
  EXPECT_EQ(Storage.file(epub.getThumbBmpPath(Epub::SHARED_THUMB_HEIGHT)), shared);
  EXPECT_EQ(Storage.file(epub.getThumbBmpPath(Epub::CAROUSEL_THUMB_HEIGHT)), carousel);
}

TEST_F(EpubSourceIdentityTest, ExtractsDeviceSpecificX4CarouselThumbnail) {
  const auto carousel = makeValidBmpSized(Epub::CAROUSEL_X4_THUMB_WIDTH, Epub::CAROUSEL_X4_THUMB_HEIGHT, 0x33U);
  identify(makeStoredZip({{Epub::carouselThumbnailEntry(Epub::CAROUSEL_X4_THUMB_WIDTH, Epub::CAROUSEL_X4_THUMB_HEIGHT),
                           asString(carousel)}}));

  Epub epub(EPUB_PATH, "/.crosspoint");
  EXPECT_EQ(epub.ensureCarouselThumbnail(Epub::CAROUSEL_X4_THUMB_WIDTH, Epub::CAROUSEL_X4_THUMB_HEIGHT,
                                         Epub::ThumbnailMode::EmbeddedOnly),
            Epub::ThumbnailStatus::Ready);
  EXPECT_EQ(Storage.file(epub.getThumbBmpPath(Epub::CAROUSEL_X4_THUMB_HEIGHT)), carousel);
}

TEST_F(EpubSourceIdentityTest, DirectSdBatchExtractsCoverOnceForSharedAndX3CarouselCaches) {
  identify(makeGuideCoverEpub(R"(<html><body><img src="images/cover.jpg"/></body></html>)"));
  ThumbnailConverterStub::reset(true);

  Epub epub(EPUB_PATH, "/.crosspoint");
  const Epub::ThumbnailSetStatus result = epub.ensureThumbnails({true, true, true});

  EXPECT_EQ(result.shared, Epub::ThumbnailStatus::Ready);
  EXPECT_EQ(result.carousel, Epub::ThumbnailStatus::Ready);
  ASSERT_EQ(ThumbnailConverterStub::callCount, 2U);
  EXPECT_EQ(ThumbnailConverterStub::batchCallCount, 1U);
  EXPECT_EQ(ThumbnailConverterStub::rangedCallCount, 1U);
  EXPECT_GT(ThumbnailConverterStub::lastSourceOffset, 0U);
  EXPECT_EQ(ThumbnailConverterStub::lastSourceLength, strlen("not-decoded-by-this-test"));
  EXPECT_EQ(ThumbnailConverterStub::calls[0].width, Epub::SHARED_THUMB_WIDTH);
  EXPECT_EQ(ThumbnailConverterStub::calls[0].height, Epub::SHARED_THUMB_HEIGHT);
  EXPECT_TRUE(ThumbnailConverterStub::calls[0].crop);
  EXPECT_EQ(ThumbnailConverterStub::calls[1].width, Epub::CAROUSEL_THUMB_WIDTH);
  EXPECT_EQ(ThumbnailConverterStub::calls[1].height, Epub::CAROUSEL_THUMB_HEIGHT);
  EXPECT_FALSE(ThumbnailConverterStub::calls[1].crop);
  const std::string scratch = epub.getCachePath() + "/.cover.jpg";
  EXPECT_EQ(Storage.openWriteAttemptsFor(scratch), 0U);
  EXPECT_FALSE(Storage.exists(scratch.c_str()));
  EXPECT_TRUE(Storage.exists(epub.getThumbBmpPath(Epub::SHARED_THUMB_HEIGHT).c_str()));
  EXPECT_TRUE(Storage.exists((epub.getThumbBmpPath(Epub::SHARED_THUMB_HEIGHT) + ".identity").c_str()));
  EXPECT_TRUE(Storage.exists(epub.getThumbBmpPath(Epub::CAROUSEL_THUMB_HEIGHT).c_str()));
  EXPECT_TRUE(Storage.exists((epub.getThumbBmpPath(Epub::CAROUSEL_THUMB_HEIGHT) + ".fit-v2.identity").c_str()));

  ThumbnailConverterStub::reset(true);
  const Epub::ThumbnailSetStatus cached = epub.ensureThumbnails({true, true, true});
  EXPECT_EQ(cached.shared, Epub::ThumbnailStatus::Ready);
  EXPECT_EQ(cached.carousel, Epub::ThumbnailStatus::Ready);
  EXPECT_EQ(ThumbnailConverterStub::callCount, 0U);
  EXPECT_EQ(ThumbnailConverterStub::batchCallCount, 0U);
  EXPECT_EQ(Storage.openWriteAttemptsFor(scratch), 0U);
}

TEST_F(EpubSourceIdentityTest, DeflatedSingleVariantRequestYieldsThenCreatesBothThumbnailCaches) {
  auto bytes =
      makeGuideCoverEpub(R"(<html><body><img src="images/cover.jpg"/></body></html>)", true, std::string(9000U, 'J'));
  identify(bytes);
  ThumbnailConverterStub::reset(true);

  Epub epub(EPUB_PATH, "/.crosspoint");
  BookMetadataCache::BookMetadata metadata;
  ASSERT_TRUE(epub.readCoreMetadata(metadata));
  ASSERT_EQ(epub.beginThumbnailPreparation({true, false, true}), Epub::ThumbnailPreparationStatus::InProgress);
  ASSERT_TRUE(epub.thumbnailPreparationActive());
  const std::string scratch = epub.getCachePath() + "/.cover.jpg";
  ASSERT_TRUE(Storage.exists(scratch.c_str()));

  size_t steps = 0;
  Epub::ThumbnailPreparationStatus status = Epub::ThumbnailPreparationStatus::InProgress;
  while (status == Epub::ThumbnailPreparationStatus::InProgress && steps++ < 32U) {
    status = epub.stepThumbnailPreparation();
  }
  EXPECT_EQ(status, Epub::ThumbnailPreparationStatus::Ready);
  EXPECT_GT(steps, 1U);
  EXPECT_FALSE(epub.thumbnailPreparationActive());

  const Epub::ThumbnailSetStatus result = epub.ensureThumbnails({true, false, true});
  EXPECT_EQ(result.shared, Epub::ThumbnailStatus::Ready);
  EXPECT_EQ(result.carousel, Epub::ThumbnailStatus::Ready);
  EXPECT_EQ(ThumbnailConverterStub::batchCallCount, 1U);
  EXPECT_EQ(ThumbnailConverterStub::rangedCallCount, 0U);
  EXPECT_FALSE(Storage.exists(scratch.c_str()));
}

TEST_F(EpubSourceIdentityTest, CancellingDeflatedCoverPreparationRemovesOnlyScratchData) {
  auto bytes =
      makeGuideCoverEpub(R"(<html><body><img src="images/cover.jpg"/></body></html>)", true, std::string(9000U, 'J'));
  identify(bytes);
  Epub epub(EPUB_PATH, "/.crosspoint");
  BookMetadataCache::BookMetadata metadata;
  ASSERT_TRUE(epub.readCoreMetadata(metadata));
  const std::string existingCache = epub.getThumbBmpPath(Epub::CAROUSEL_THUMB_HEIGHT);
  Storage.setFile(existingCache, makeValidBmpSized(Epub::CAROUSEL_THUMB_WIDTH, Epub::CAROUSEL_THUMB_HEIGHT));

  ASSERT_EQ(epub.beginThumbnailPreparation({true, false, true}), Epub::ThumbnailPreparationStatus::InProgress);
  ASSERT_EQ(epub.stepThumbnailPreparation(), Epub::ThumbnailPreparationStatus::InProgress);
  epub.cancelThumbnailPreparation();

  EXPECT_FALSE(epub.thumbnailPreparationActive());
  EXPECT_FALSE(Storage.exists((epub.getCachePath() + "/.cover.jpg").c_str()));
  EXPECT_TRUE(Storage.exists(existingCache.c_str()));
}

TEST_F(EpubSourceIdentityTest, PageImagePreparationPreemptsDeflatedCoverStream) {
  constexpr char coverPath[] = "OPS/images/cover.jpg";
  auto bytes =
      makeGuideCoverEpub(R"(<html><body><img src="images/cover.jpg"/></body></html>)", true, std::string(9000U, 'J'));
  identify(bytes);
  Epub epub(EPUB_PATH, "/.crosspoint");
  BookMetadataCache::BookMetadata metadata;
  ASSERT_TRUE(epub.readCoreMetadata(metadata));
  ASSERT_EQ(metadata.coverItemHref, coverPath);
  ASSERT_EQ(epub.beginThumbnailPreparation({true, false, true}), Epub::ThumbnailPreparationStatus::InProgress);
  ASSERT_TRUE(epub.thumbnailPreparationActive());

  const std::string finalPath = epub.getCachePath() + "/visible.jpg";
  EXPECT_EQ(epub.beginImagePreparation(coverPath, finalPath), Epub::ImagePreparationStatus::InProgress);
  EXPECT_TRUE(epub.imagePreparationActive());
  EXPECT_FALSE(epub.thumbnailPreparationActive());
  EXPECT_FALSE(Storage.exists((epub.getCachePath() + "/.cover.jpg").c_str()));

  EXPECT_EQ(epub.beginThumbnailPreparation({true, false, true}), Epub::ThumbnailPreparationStatus::Error);
  EXPECT_TRUE(epub.imagePreparationActive());
  EXPECT_FALSE(epub.thumbnailPreparationActive());
}

TEST_F(EpubSourceIdentityTest, StoredCoverPreparationUsesTheDirectRangePathWithoutScratch) {
  identify(makeGuideCoverEpub(R"(<html><body><img src="images/cover.jpg"/></body></html>)"));
  Epub epub(EPUB_PATH, "/.crosspoint");
  BookMetadataCache::BookMetadata metadata;
  ASSERT_TRUE(epub.readCoreMetadata(metadata));

  EXPECT_EQ(epub.beginThumbnailPreparation({true, false, true}),
            Epub::ThumbnailPreparationStatus::NeedsSynchronousGeneration);
  EXPECT_FALSE(epub.thumbnailPreparationActive());
  EXPECT_FALSE(Storage.exists((epub.getCachePath() + "/.cover.jpg").c_str()));
}

TEST_F(EpubSourceIdentityTest, DeflatedCoverPreparationRejectsSourceReplacementBeforePublish) {
  auto bytes =
      makeGuideCoverEpub(R"(<html><body><img src="images/cover.jpg"/></body></html>)", true, std::string(9000U, 'J'));
  identify(bytes);
  Epub epub(EPUB_PATH, "/.crosspoint");
  BookMetadataCache::BookMetadata metadata;
  ASSERT_TRUE(epub.readCoreMetadata(metadata));

  ASSERT_EQ(epub.beginThumbnailPreparation({true, false, true}), Epub::ThumbnailPreparationStatus::InProgress);
  ASSERT_EQ(epub.stepThumbnailPreparation(), Epub::ThumbnailPreparationStatus::InProgress);
  Storage.setFile(EPUB_PATH, makeZip(0xA5A5A5A5U));
  Epub::ThumbnailPreparationStatus status = Epub::ThumbnailPreparationStatus::InProgress;
  for (size_t step = 0; status == Epub::ThumbnailPreparationStatus::InProgress && step < 8U; ++step) {
    status = epub.stepThumbnailPreparation();
  }
  EXPECT_EQ(status, Epub::ThumbnailPreparationStatus::Error);
  EXPECT_FALSE(Storage.exists((epub.getCachePath() + "/.cover.jpg").c_str()));
}

TEST_F(EpubSourceIdentityTest, DirectSdBatchRequestingOnlyX4CarouselAlsoCreatesSharedThumbnail) {
  identify(makeGuideCoverEpub(R"(<html><body><img src="images/cover.jpg"/></body></html>)"));
  ThumbnailConverterStub::reset(true);

  Epub epub(EPUB_PATH, "/.crosspoint");
  const Epub::ThumbnailSetStatus result = epub.ensureThumbnails({false, true, false});

  EXPECT_EQ(result.shared, Epub::ThumbnailStatus::Ready);
  EXPECT_EQ(result.carousel, Epub::ThumbnailStatus::Ready);
  ASSERT_EQ(ThumbnailConverterStub::callCount, 2U);
  EXPECT_EQ(ThumbnailConverterStub::batchCallCount, 1U);
  EXPECT_EQ(ThumbnailConverterStub::calls[0].width, Epub::SHARED_THUMB_WIDTH);
  EXPECT_EQ(ThumbnailConverterStub::calls[0].height, Epub::SHARED_THUMB_HEIGHT);
  EXPECT_EQ(ThumbnailConverterStub::calls[1].width, Epub::CAROUSEL_X4_THUMB_WIDTH);
  EXPECT_EQ(ThumbnailConverterStub::calls[1].height, Epub::CAROUSEL_X4_THUMB_HEIGHT);
  EXPECT_TRUE(Storage.exists(epub.getThumbBmpPath(Epub::SHARED_THUMB_HEIGHT).c_str()));
  EXPECT_TRUE(Storage.exists(epub.getThumbBmpPath(Epub::CAROUSEL_X4_THUMB_HEIGHT).c_str()));
}

TEST_F(EpubSourceIdentityTest, DirectSdBatchRequestingOnlySharedAlsoCreatesX3CarouselThumbnail) {
  identify(makeGuideCoverEpub(R"(<html><body><img src="images/cover.jpg"/></body></html>)"));
  ThumbnailConverterStub::reset(true);

  Epub epub(EPUB_PATH, "/.crosspoint");
  const Epub::ThumbnailSetStatus result = epub.ensureThumbnails({true, false, true});

  EXPECT_EQ(result.shared, Epub::ThumbnailStatus::Ready);
  EXPECT_EQ(result.carousel, Epub::ThumbnailStatus::Ready);
  ASSERT_EQ(ThumbnailConverterStub::callCount, 2U);
  EXPECT_EQ(ThumbnailConverterStub::batchCallCount, 1U);
  EXPECT_EQ(ThumbnailConverterStub::calls[0].width, Epub::SHARED_THUMB_WIDTH);
  EXPECT_EQ(ThumbnailConverterStub::calls[0].height, Epub::SHARED_THUMB_HEIGHT);
  EXPECT_EQ(ThumbnailConverterStub::calls[1].width, Epub::CAROUSEL_THUMB_WIDTH);
  EXPECT_EQ(ThumbnailConverterStub::calls[1].height, Epub::CAROUSEL_THUMB_HEIGHT);
  EXPECT_TRUE(Storage.exists(epub.getThumbBmpPath(Epub::SHARED_THUMB_HEIGHT).c_str()));
  EXPECT_TRUE(Storage.exists(epub.getThumbBmpPath(Epub::CAROUSEL_THUMB_HEIGHT).c_str()));
}

TEST_F(EpubSourceIdentityTest, DirectSdBatchDoesNothingWhenNoCoverUiNeedsACache) {
  identify(makeGuideCoverEpub(R"(<html><body><img src="images/cover.jpg"/></body></html>)"));
  ThumbnailConverterStub::reset(true);

  Epub epub(EPUB_PATH, "/.crosspoint");
  const Epub::ThumbnailSetStatus result = epub.ensureThumbnails({false, false, true});

  EXPECT_EQ(result.shared, Epub::ThumbnailStatus::Missing);
  EXPECT_EQ(result.carousel, Epub::ThumbnailStatus::Missing);
  EXPECT_EQ(ThumbnailConverterStub::callCount, 0U);
  EXPECT_EQ(Storage.openWriteAttemptsFor(epub.getCachePath() + "/.cover.jpg"), 0U);
  EXPECT_FALSE(Storage.exists(epub.getThumbBmpPath(Epub::SHARED_THUMB_HEIGHT).c_str()));
  EXPECT_FALSE(Storage.exists(epub.getThumbBmpPath(Epub::CAROUSEL_THUMB_HEIGHT).c_str()));
}

TEST_F(EpubSourceIdentityTest, EmbeddedOnlySingleVariantRequestNeverDecodesTheOriginalCover) {
  identify(makeGuideCoverEpub(R"(<html><body><img src="images/cover.jpg"/></body></html>)"));
  ThumbnailConverterStub::reset(true);

  Epub epub(EPUB_PATH, "/.crosspoint");
  const Epub::ThumbnailSetStatus result = epub.ensureThumbnails({true, false, true}, Epub::ThumbnailMode::EmbeddedOnly);

  EXPECT_EQ(result.shared, Epub::ThumbnailStatus::Missing);
  EXPECT_EQ(result.carousel, Epub::ThumbnailStatus::Missing);
  EXPECT_EQ(ThumbnailConverterStub::callCount, 0U);
  EXPECT_FALSE(Storage.exists((epub.getCachePath() + "/.cover.jpg").c_str()));
}

TEST_F(EpubSourceIdentityTest, DirectSdBatchKeepsSuccessfulSiblingWhenAnotherConversionFails) {
  identify(makeGuideCoverEpub(R"(<html><body><img src="images/cover.jpg"/></body></html>)"));
  ThumbnailConverterStub::reset(true);
  ThumbnailConverterStub::failCall = 2;

  Epub epub(EPUB_PATH, "/.crosspoint");
  const Epub::ThumbnailSetStatus result = epub.ensureThumbnails({true, true, true});

  EXPECT_EQ(result.shared, Epub::ThumbnailStatus::Ready);
  EXPECT_NE(result.carousel, Epub::ThumbnailStatus::Ready);
  EXPECT_TRUE(Storage.exists(epub.getThumbBmpPath(Epub::SHARED_THUMB_HEIGHT).c_str()));
  EXPECT_FALSE(Storage.exists(epub.getThumbBmpPath(Epub::CAROUSEL_THUMB_HEIGHT).c_str()));
  EXPECT_FALSE(Storage.exists((epub.getThumbBmpPath(Epub::CAROUSEL_THUMB_HEIGHT) + ".tmp").c_str()));
  EXPECT_FALSE(Storage.exists((epub.getCachePath() + "/.cover.jpg").c_str()));
}

TEST_F(EpubSourceIdentityTest, DirectSdBatchPublishFailurePreservesThePreviousThumbnail) {
  identify(makeGuideCoverEpub(R"(<html><body><img src="images/cover.jpg"/></body></html>)"));
  ThumbnailConverterStub::reset(true);

  Epub epub(EPUB_PATH, "/.crosspoint");
  const std::string finalPath = epub.getThumbBmpPath(Epub::SHARED_THUMB_HEIGHT);
  const auto previous = makeValidBmpSized(Epub::SHARED_THUMB_WIDTH, Epub::SHARED_THUMB_HEIGHT, 0x11U);
  Storage.setFile(finalPath, previous);
  Storage.failRenameTo(finalPath);

  const Epub::ThumbnailSetStatus result = epub.ensureThumbnails({true, true, true});

  EXPECT_EQ(result.shared, Epub::ThumbnailStatus::IoError);
  EXPECT_EQ(Storage.file(finalPath), previous);
  EXPECT_FALSE(Storage.exists((finalPath + ".tmp").c_str()));
  EXPECT_EQ(result.carousel, Epub::ThumbnailStatus::Ready);
}

TEST_F(EpubSourceIdentityTest, DirectSdBatchRejectsCachesIfTheEpubChangesDuringConversion) {
  const auto original = makeGuideCoverEpub(R"(<html><body><img src="images/cover.jpg"/></body></html>)");
  identify(original);
  ThumbnailConverterStub::reset(true);
  ThumbnailConverterStub::afterCall = [](const size_t call) {
    if (call == 1) Storage.setFile(EPUB_PATH, makeZip(0xABCDEF01U));
  };

  Epub epub(EPUB_PATH, "/.crosspoint");
  const Epub::ThumbnailSetStatus result = epub.ensureThumbnails({true, true, true});

  EXPECT_EQ(result.shared, Epub::ThumbnailStatus::IoError);
  EXPECT_EQ(result.carousel, Epub::ThumbnailStatus::IoError);
  EXPECT_FALSE(Storage.exists(epub.getThumbBmpPath(Epub::SHARED_THUMB_HEIGHT).c_str()));
  EXPECT_FALSE(Storage.exists((epub.getThumbBmpPath(Epub::SHARED_THUMB_HEIGHT) + ".identity").c_str()));
  EXPECT_FALSE(Storage.exists(epub.getThumbBmpPath(Epub::CAROUSEL_THUMB_HEIGHT).c_str()));
  EXPECT_FALSE(Storage.exists((epub.getCachePath() + "/.cover.jpg").c_str()));
}

TEST_F(EpubSourceIdentityTest, StoredJpegCoverNeverCreatesScratchFile) {
  identify(makeGuideCoverEpub(R"(<html><body><img src="images/cover.jpg"/></body></html>)"));
  ThumbnailConverterStub::reset(true);
  Epub epub(EPUB_PATH, "/.crosspoint");
  const std::string scratch = epub.getCachePath() + "/.cover.jpg";
  Storage.failRemoveFor(scratch);
  const Epub::ThumbnailSetStatus result = epub.ensureThumbnails({true, false, true});
  EXPECT_EQ(result.shared, Epub::ThumbnailStatus::Ready);
  EXPECT_EQ(ThumbnailConverterStub::rangedCallCount, 1U);
  EXPECT_EQ(Storage.openWriteAttemptsFor(scratch), 0U);
  EXPECT_FALSE(Storage.exists(scratch.c_str()));
}

TEST_F(EpubSourceIdentityTest, FitCarouselCacheAvoidsPostDitherScalingAndReplacesLegacyOversize) {
  const auto embedded = makeValidBmpSized(Epub::CAROUSEL_THUMB_WIDTH, Epub::CAROUSEL_THUMB_HEIGHT, 0x22U);
  identify(makeStoredZip(
      {{Epub::carouselThumbnailEntry(Epub::CAROUSEL_THUMB_WIDTH, Epub::CAROUSEL_THUMB_HEIGHT), asString(embedded)}}));

  Epub epub(EPUB_PATH, "/.crosspoint");
  ASSERT_EQ(epub.ensureCarouselThumbnail(Epub::CAROUSEL_THUMB_WIDTH, Epub::CAROUSEL_THUMB_HEIGHT,
                                         Epub::ThumbnailMode::EmbeddedOnly),
            Epub::ThumbnailStatus::Ready);
  const std::string finalPath = epub.getThumbBmpPath(Epub::CAROUSEL_THUMB_HEIGHT);

  const auto fitted = makeValidBmpSized(Epub::CAROUSEL_THUMB_WIDTH, 414, 0x44U);
  Storage.setFile(finalPath, fitted);
  EXPECT_EQ(epub.ensureCarouselThumbnail(Epub::CAROUSEL_THUMB_WIDTH, Epub::CAROUSEL_THUMB_HEIGHT,
                                         Epub::ThumbnailMode::EmbeddedOnly),
            Epub::ThumbnailStatus::Ready);
  EXPECT_EQ(Storage.file(finalPath), fitted);

  Storage.setFile(finalPath, makeValidBmpSized(300, Epub::CAROUSEL_THUMB_HEIGHT, 0x55U));
  EXPECT_EQ(epub.ensureCarouselThumbnail(Epub::CAROUSEL_THUMB_WIDTH, Epub::CAROUSEL_THUMB_HEIGHT,
                                         Epub::ThumbnailMode::EmbeddedOnly),
            Epub::ThumbnailStatus::Ready);
  EXPECT_EQ(Storage.file(finalPath), embedded);
}

TEST_F(EpubSourceIdentityTest, RejectsWrongDimensionsForEmbeddedCarouselThumbnail) {
  const auto wrong = makeValidBmpSized(Epub::SHARED_THUMB_WIDTH, Epub::SHARED_THUMB_HEIGHT);
  identify(makeStoredZip(
      {{Epub::carouselThumbnailEntry(Epub::CAROUSEL_THUMB_WIDTH, Epub::CAROUSEL_THUMB_HEIGHT), asString(wrong)}}));

  Epub epub(EPUB_PATH, "/.crosspoint");
  const std::string finalPath = epub.getThumbBmpPath(Epub::CAROUSEL_THUMB_HEIGHT);
  EXPECT_EQ(epub.ensureCarouselThumbnail(Epub::CAROUSEL_THUMB_WIDTH, Epub::CAROUSEL_THUMB_HEIGHT,
                                         Epub::ThumbnailMode::EmbeddedOnly),
            Epub::ThumbnailStatus::Invalid);
  EXPECT_FALSE(Storage.exists(finalPath.c_str()));
  EXPECT_FALSE(Storage.exists((finalPath + ".tmp").c_str()));
}

TEST_F(EpubSourceIdentityTest, ReplacesStaleCarouselThumbnailWhenSourceAtSamePathChanges) {
  const auto first = makeValidBmpSized(Epub::CAROUSEL_THUMB_WIDTH, Epub::CAROUSEL_THUMB_HEIGHT, 0x11U);
  const auto second = makeValidBmpSized(Epub::CAROUSEL_THUMB_WIDTH, Epub::CAROUSEL_THUMB_HEIGHT, 0x22U);
  identify(makeStoredZip(
      {{Epub::carouselThumbnailEntry(Epub::CAROUSEL_THUMB_WIDTH, Epub::CAROUSEL_THUMB_HEIGHT), asString(first)}}));
  {
    Epub epub(EPUB_PATH, "/.crosspoint");
    ASSERT_EQ(epub.ensureCarouselThumbnail(Epub::CAROUSEL_THUMB_WIDTH, Epub::CAROUSEL_THUMB_HEIGHT,
                                           Epub::ThumbnailMode::EmbeddedOnly),
              Epub::ThumbnailStatus::Ready);
    ASSERT_EQ(Storage.file(epub.getThumbBmpPath(Epub::CAROUSEL_THUMB_HEIGHT)), first);
  }

  Storage.setFile(EPUB_PATH,
                  makeStoredZip({{"META-INF/crossvi/revision", "2"},
                                 {Epub::carouselThumbnailEntry(Epub::CAROUSEL_THUMB_WIDTH, Epub::CAROUSEL_THUMB_HEIGHT),
                                  asString(second)}}));
  Epub replaced(EPUB_PATH, "/.crosspoint");
  ASSERT_EQ(replaced.ensureCarouselThumbnail(Epub::CAROUSEL_THUMB_WIDTH, Epub::CAROUSEL_THUMB_HEIGHT,
                                             Epub::ThumbnailMode::EmbeddedOnly),
            Epub::ThumbnailStatus::Ready);
  EXPECT_EQ(Storage.file(replaced.getThumbBmpPath(Epub::CAROUSEL_THUMB_HEIGHT)), second);
}

TEST_F(EpubSourceIdentityTest, ValidThumbnailSurvivesStaleBackupCleanupFailure) {
  Epub epub(EPUB_PATH, "/.crosspoint");
  constexpr int thumbnailHeight = 120;
  const std::string finalPath = epub.getThumbBmpPath(thumbnailHeight);
  const std::string backupPath = finalPath + ".bak";
  Storage.setFile(finalPath, makeValidBmp());
  Storage.setFile(backupPath, makeValidBmp(0x00U));
  Storage.failRemoveFor(backupPath);

  EXPECT_TRUE(epub.generateThumbBmp(thumbnailHeight));
  EXPECT_TRUE(Storage.exists(finalPath.c_str()));
  EXPECT_TRUE(Storage.exists(backupPath.c_str()));
}

TEST_F(EpubSourceIdentityTest, InvalidThumbnailBackupDoesNotBlockRegeneration) {
  Epub epub(EPUB_PATH, "/.crosspoint");
  constexpr int thumbnailHeight = 120;
  const std::string backupPath = epub.getThumbBmpPath(thumbnailHeight) + ".bak";
  Storage.setFile(backupPath, {0x42U, 0x4DU});

  EXPECT_FALSE(epub.generateThumbBmp(thumbnailHeight));
  EXPECT_FALSE(Storage.exists(backupPath.c_str()));
}

TEST_F(EpubSourceIdentityTest, UnreadableThumbnailBackupIsPreservedForRetry) {
  Epub epub(EPUB_PATH, "/.crosspoint");
  constexpr int thumbnailHeight = 120;
  const std::string backupPath = epub.getThumbBmpPath(thumbnailHeight) + ".bak";
  Storage.setFile(backupPath, makeValidBmp());
  Storage.makeUnreadable(backupPath);

  EXPECT_FALSE(epub.generateThumbBmp(thumbnailHeight));
  EXPECT_TRUE(Storage.exists(backupPath.c_str()));
}

TEST_F(EpubSourceIdentityTest, InvalidNoCoverBackupDoesNotBlockVerifiedMarker) {
  identify(makeCssTestEpub());
  Epub epub(EPUB_PATH, "/.crosspoint");
  BookMetadataCache::BookMetadata metadata;
  ASSERT_TRUE(epub.readCoreMetadata(metadata));
  ASSERT_TRUE(metadata.coverItemHref.empty());

  constexpr int thumbnailHeight = 120;
  const std::string markerPath = epub.getThumbBmpPath(thumbnailHeight) + ".nocover";
  const std::string backupPath = markerPath + ".bak";
  Storage.setFile(backupPath, {'b', 'a', 'd'});

  EXPECT_FALSE(epub.generateThumbBmp(thumbnailHeight));
  EXPECT_TRUE(Storage.exists(markerPath.c_str()));
  EXPECT_FALSE(Storage.exists(backupPath.c_str()));
}

TEST_F(EpubSourceIdentityTest, UnreadableNoCoverMarkerIsPreservedWithoutReplacement) {
  identify(makeCssTestEpub());
  Epub epub(EPUB_PATH, "/.crosspoint");
  BookMetadataCache::BookMetadata metadata;
  ASSERT_TRUE(epub.readCoreMetadata(metadata));
  ASSERT_TRUE(metadata.coverItemHref.empty());

  constexpr int thumbnailHeight = 120;
  const std::string markerPath = epub.getThumbBmpPath(thumbnailHeight) + ".nocover";
  const std::string stagingPath = markerPath + ".tmp";
  Storage.setFile(markerPath, {'C', 'V', 'N', 'C', '1', '\0'});
  Storage.makeUnreadable(markerPath);

  EXPECT_FALSE(epub.generateThumbBmp(thumbnailHeight));
  EXPECT_TRUE(Storage.exists(markerPath.c_str()));
  EXPECT_EQ(Storage.openWriteAttemptsFor(stagingPath), 0U);
}

TEST_F(EpubSourceIdentityTest, InspectedGuideWithoutSupportedImageWritesNoCoverMarker) {
  identify(makeGuideCoverEpub(R"(<html><body><img src="images/cover.gif"/></body></html>)"));
  Epub epub(EPUB_PATH, "/.crosspoint");
  BookMetadataCache::BookMetadata metadata;
  ASSERT_TRUE(epub.readCoreMetadata(metadata));
  ASSERT_TRUE(metadata.coverItemHref.empty());

  constexpr int thumbnailHeight = 120;
  EXPECT_FALSE(epub.generateThumbBmp(thumbnailHeight));
  EXPECT_TRUE(Storage.exists((epub.getThumbBmpPath(thumbnailHeight) + ".nocover").c_str()));
}

TEST_F(EpubSourceIdentityTest, FastMetadataResolvesGuideOnlyCoverWithinBound) {
  identify(makeGuideCoverEpub(R"(<html><body><img src="images/cover.jpg"/></body></html>)"));
  Epub epub(EPUB_PATH, "/.crosspoint");
  BookMetadataCache::BookMetadata metadata;

  ASSERT_TRUE(epub.readCoreMetadata(metadata));
  EXPECT_EQ(metadata.coverItemHref, "OPS/images/cover.jpg");
  EXPECT_LE(Storage.maxRead(), 1024U);
}

TEST_F(EpubSourceIdentityTest, FastMetadataResolvesSingleQuotedGuideCover) {
  identify(makeGuideCoverEpub(R"(<html><body><img src='images/cover.jpg'/></body></html>)"));
  Epub epub(EPUB_PATH, "/.crosspoint");
  BookMetadataCache::BookMetadata metadata;

  ASSERT_TRUE(epub.readCoreMetadata(metadata));
  EXPECT_EQ(metadata.coverItemHref, "OPS/images/cover.jpg");
}

TEST_F(EpubSourceIdentityTest, OversizedGuideCoverDoesNotCreatePermanentNoCoverMarker) {
  identify(makeGuideCoverEpub(std::string(32U * 1024U + 1U, 'x')));
  Epub epub(EPUB_PATH, "/.crosspoint");
  BookMetadataCache::BookMetadata metadata;
  ASSERT_TRUE(epub.readCoreMetadata(metadata));
  ASSERT_TRUE(metadata.coverItemHref.empty());

  constexpr int thumbnailHeight = 120;
  EXPECT_FALSE(epub.generateThumbBmp(thumbnailHeight));
  EXPECT_FALSE(Storage.exists((epub.getThumbBmpPath(thumbnailHeight) + ".nocover").c_str()));
  EXPECT_LE(Storage.maxRead(), 1024U);
}

TEST_F(EpubSourceIdentityTest, UnreadableGuideCoverDoesNotCreatePermanentNoCoverMarker) {
  auto bytes = makeGuideCoverEpub(R"(<html><body><img src="images/cover.jpg"/></body></html>)");
  const std::string guidePath = "OPS/cover.xhtml";
  const auto guideName = std::search(bytes.begin(), bytes.end(), guidePath.begin(), guidePath.end());
  ASSERT_NE(guideName, bytes.end());
  const size_t guideNameOffset = static_cast<size_t>(guideName - bytes.begin());
  ASSERT_GE(guideNameOffset, 30U);
  put32(bytes, guideNameOffset - 30U, 0U);
  identify(bytes);

  Epub epub(EPUB_PATH, "/.crosspoint");
  BookMetadataCache::BookMetadata metadata;
  ASSERT_TRUE(epub.readCoreMetadata(metadata));
  ASSERT_TRUE(metadata.coverItemHref.empty());

  constexpr int thumbnailHeight = 120;
  EXPECT_FALSE(epub.generateThumbBmp(thumbnailHeight));
  EXPECT_FALSE(Storage.exists((epub.getThumbBmpPath(thumbnailHeight) + ".nocover").c_str()));
}

TEST_F(EpubSourceIdentityTest, ThumbnailScratchAndConverterFailuresLeaveNoDerivedOutput) {
  enum class Fault { Converter, ShortWrite, Sync, Close };
  for (const Fault fault : {Fault::Converter, Fault::ShortWrite, Fault::Sync, Fault::Close}) {
    SCOPED_TRACE(static_cast<int>(fault));
    Storage.reset();
    identify(makeGuideCoverEpub(R"(<html><body><img src="images/cover.jpg"/></body></html>)"));
    Epub epub(EPUB_PATH, "/.crosspoint");
    BookMetadataCache::BookMetadata metadata;
    ASSERT_TRUE(epub.readCoreMetadata(metadata));
    ASSERT_EQ(metadata.coverItemHref, "OPS/images/cover.jpg");

    constexpr int thumbnailHeight = 120;
    const std::string finalPath = epub.getThumbBmpPath(thumbnailHeight);
    const std::string stagingPath = finalPath + ".tmp";
    const std::string sourcePath = epub.getCachePath() + "/.cover.jpg";
    switch (fault) {
      case Fault::Converter:
        break;
      case Fault::ShortWrite:
        Storage.shortWriteFor(sourcePath);
        break;
      case Fault::Sync:
        Storage.failSyncOnce();
        break;
      case Fault::Close:
        Storage.failCloseFor(sourcePath);
        break;
    }

    EXPECT_FALSE(epub.generateThumbBmp(thumbnailHeight));
    EXPECT_FALSE(Storage.exists(finalPath.c_str()));
    EXPECT_FALSE(Storage.exists(stagingPath.c_str()));
    EXPECT_FALSE(Storage.exists(sourcePath.c_str()));
    EXPECT_FALSE(Storage.exists((finalPath + ".nocover").c_str()));
    EXPECT_EQ(Storage.invalidOperationCount(), 0U);
  }
}

TEST_F(EpubSourceIdentityTest, FullModeDescendantRulesAreBoundedAndSurviveTheCssCache) {
  const std::string css = "p.lead { margin-left: 1em; } section.note p.lead { text-align: right; text-indent: 2em; }";
  Storage.setFile("/full-mode.css", std::vector<uint8_t>(css.begin(), css.end()));
  HalFile source;
  ASSERT_TRUE(Storage.openFileForRead("TEST", "/full-mode.css", source));

  CssParser parser(CACHE_PATH);
  ASSERT_TRUE(parser.loadFromStream(source));
  ASSERT_TRUE(source.close());

  const CssStyle balanced = parser.resolveStyle("p", "lead");
  EXPECT_TRUE(balanced.defined.marginLeft);
  EXPECT_FALSE(balanced.defined.textAlign);

  const std::vector<CssParser::AncestorEntry> matching = {{"section", "note"}};
  const CssStyle full = parser.resolveStyle("p", "lead", matching);
  EXPECT_TRUE(full.defined.textAlign);
  EXPECT_EQ(full.textAlign, CssTextAlign::Right);
  EXPECT_TRUE(full.defined.textIndent);

  const std::vector<CssParser::AncestorEntry> notMatching = {{"section", "warning"}};
  EXPECT_FALSE(parser.resolveStyle("p", "lead", notMatching).defined.textAlign);

  ASSERT_TRUE(parser.saveToCache());
  CssParser restored(CACHE_PATH);
  ASSERT_TRUE(restored.loadFromCache());
  const CssStyle cached = restored.resolveStyle("p", "lead", matching);
  EXPECT_TRUE(cached.defined.textAlign);
  EXPECT_EQ(cached.textAlign, CssTextAlign::Right);
  EXPECT_TRUE(cached.defined.textIndent);
}

TEST_F(EpubSourceIdentityTest, CssRuleGrowthStopsSafelyWhenHeapIsLow) {
  const std::string css = "p.first { text-align: right; } p.second { margin-left: 2em; }";
  Storage.setFile("/low-memory.css", std::vector<uint8_t>(css.begin(), css.end()));
  HalFile source;
  ASSERT_TRUE(Storage.openFileForRead("TEST", "/low-memory.css", source));

  ESP.setHeap(63U * 1024U, 32U * 1024U);
  CssParser parser(CACHE_PATH);
  EXPECT_TRUE(parser.loadFromStream(source));
  EXPECT_TRUE(parser.empty());
  EXPECT_TRUE(source.close());
}

TEST_F(EpubSourceIdentityTest, LowHeapStillMergesAnExistingCssRule) {
  const std::string initialCss = "p.note { text-align: left; }";
  Storage.setFile("/initial.css", std::vector<uint8_t>(initialCss.begin(), initialCss.end()));
  HalFile initial;
  ASSERT_TRUE(Storage.openFileForRead("TEST", "/initial.css", initial));

  CssParser parser(CACHE_PATH);
  ASSERT_TRUE(parser.loadFromStream(initial));
  ASSERT_TRUE(initial.close());
  ASSERT_EQ(parser.ruleCount(), 1U);

  const std::string updateCss = "p.note { text-align: right; } p.new { margin-left: 2em; }";
  Storage.setFile("/update.css", std::vector<uint8_t>(updateCss.begin(), updateCss.end()));
  HalFile update;
  ASSERT_TRUE(Storage.openFileForRead("TEST", "/update.css", update));
  ESP.setHeap(63U * 1024U, 32U * 1024U);
  EXPECT_TRUE(parser.loadFromStream(update));
  EXPECT_TRUE(update.close());

  EXPECT_EQ(parser.ruleCount(), 1U);
  EXPECT_EQ(parser.resolveStyle("p", "note").textAlign, CssTextAlign::Right);
}

TEST_F(EpubSourceIdentityTest, LowHeapRejectsCssCacheWithoutDeletingIt) {
  const std::string css = "p.note { text-align: right; margin-left: 2em; }";
  Storage.setFile("/cached.css", std::vector<uint8_t>(css.begin(), css.end()));
  HalFile source;
  ASSERT_TRUE(Storage.openFileForRead("TEST", "/cached.css", source));

  CssParser writer(CACHE_PATH);
  ASSERT_TRUE(writer.loadFromStream(source));
  ASSERT_TRUE(source.close());
  ASSERT_TRUE(writer.saveToCache());
  ASSERT_TRUE(writer.hasCache());

  ESP.setHeap(63U * 1024U, 32U * 1024U);
  CssParser constrained(CACHE_PATH);
  EXPECT_TRUE(constrained.validateCache());
  EXPECT_TRUE(constrained.empty());
  EXPECT_FALSE(constrained.loadFromCache());
  EXPECT_TRUE(constrained.empty());
  EXPECT_TRUE(constrained.hasCache());

  ESP.setHeap(1024U * 1024U, 1024U * 1024U);
  EXPECT_TRUE(constrained.loadFromCache());
  EXPECT_EQ(constrained.resolveStyle("p", "note").textAlign, CssTextAlign::Right);
}

TEST_F(EpubSourceIdentityTest, WarmBookValidatesCssCacheWithoutMaterializingRules) {
  const auto identity = identify(makeCssTestEpub());
  Epub firstLoad(EPUB_PATH, "/.crosspoint");
  const std::string cachePath = firstLoad.getCachePath();
  ASSERT_TRUE(Storage.mkdir(cachePath.c_str()));
  ASSERT_TRUE(firstLoad.bindCurrentSource());
  Storage.setFile(cachePath + "/book.bin", makeBookCache(identity));
  ASSERT_TRUE(firstLoad.load(false, true));
  ASSERT_TRUE(firstLoad.ensureCssCache());

  ESP.setHeap(63U * 1024U, 32U * 1024U);
  Epub warmLoad(EPUB_PATH, "/.crosspoint");
  ASSERT_TRUE(warmLoad.load(false, false));
  EXPECT_TRUE(warmLoad.getCssParser()->empty());
  EXPECT_TRUE(warmLoad.getCssParser()->hasCache());
}

TEST_F(EpubSourceIdentityTest, WarmBookPreparesCssRulesForTheFirstSectionBuild) {
  const auto identity = identify(makeCssTestEpub());
  Epub firstLoad(EPUB_PATH, "/.crosspoint");
  const std::string cachePath = firstLoad.getCachePath();
  ASSERT_TRUE(Storage.mkdir(cachePath.c_str()));
  ASSERT_TRUE(firstLoad.bindCurrentSource());
  Storage.setFile(cachePath + "/book.bin", makeBookCache(identity));
  ASSERT_TRUE(firstLoad.load(false, true));
  ASSERT_TRUE(firstLoad.ensureCssCache());

  Epub warmLoad(EPUB_PATH, "/.crosspoint");
  ASSERT_TRUE(warmLoad.load(false, false));
  ASSERT_TRUE(warmLoad.getCssParser()->hasMaterializedCache());
  EXPECT_EQ(warmLoad.getCssParser()->resolveStyle("p", "note").textAlign, CssTextAlign::Right);
}

TEST_F(EpubSourceIdentityTest, SmallCapsCssSurvivesParsingAndCacheRoundTrip) {
  const std::string css =
      "p.caps { font-variant: small-caps; } "
      "span.normal { font-variant-caps: normal; }";
  Storage.setFile("/small-caps.css", std::vector<uint8_t>(css.begin(), css.end()));
  HalFile source;
  ASSERT_TRUE(Storage.openFileForRead("TEST", "/small-caps.css", source));

  CssParser parser(CACHE_PATH);
  ASSERT_TRUE(parser.loadFromStream(source));
  ASSERT_TRUE(source.close());
  const CssStyle caps = parser.resolveStyle("p", "caps");
  EXPECT_TRUE(caps.hasFontVariantCaps());
  EXPECT_EQ(caps.fontVariantCaps, CssFontVariantCaps::SmallCaps);
  const CssStyle normal = parser.resolveStyle("span", "normal");
  EXPECT_TRUE(normal.hasFontVariantCaps());
  EXPECT_EQ(normal.fontVariantCaps, CssFontVariantCaps::Normal);

  ASSERT_TRUE(parser.saveToCache());
  CssParser restored(CACHE_PATH);
  ASSERT_TRUE(restored.loadFromCache());
  const CssStyle cached = restored.resolveStyle("p", "caps");
  EXPECT_TRUE(cached.hasFontVariantCaps());
  EXPECT_EQ(cached.fontVariantCaps, CssFontVariantCaps::SmallCaps);
}

TEST_F(EpubSourceIdentityTest, InvalidLengthKeywordsDoNotOverrideValidBookSpacing) {
  const std::string css =
      "p.note { text-indent: 2em; margin-left: 1em; } "
      "p.note { text-indent: inherit; margin: inherit; padding: auto; }";
  Storage.setFile("/invalid-length.css", std::vector<uint8_t>(css.begin(), css.end()));
  HalFile source;
  ASSERT_TRUE(Storage.openFileForRead("TEST", "/invalid-length.css", source));

  CssParser parser(CACHE_PATH);
  ASSERT_TRUE(parser.loadFromStream(source));
  ASSERT_TRUE(source.close());
  const CssStyle style = parser.resolveStyle("p", "note");
  ASSERT_TRUE(style.defined.textIndent);
  EXPECT_FLOAT_EQ(style.textIndent.value, 2.0F);
  ASSERT_TRUE(style.defined.marginLeft);
  EXPECT_FLOAT_EQ(style.marginLeft.value, 1.0F);
  EXPECT_FALSE(style.defined.marginTop);
  EXPECT_FALSE(style.defined.paddingTop);
}

TEST_F(EpubSourceIdentityTest, WarmCssDiscoveryDoesNotTouchManifestScratchFile) {
  const auto identity = identify(makeCssTestEpub());
  Epub epub(EPUB_PATH, "/.crosspoint");
  const std::string cachePath = epub.getCachePath();
  const std::string scratchPath = cachePath + "/.items.bin";
  ASSERT_TRUE(Storage.mkdir(cachePath.c_str()));
  ASSERT_TRUE(epub.bindCurrentSource());
  Storage.setFile(cachePath + "/book.bin", makeBookCache(identity));
  ASSERT_TRUE(epub.load(false, true));

  Storage.makeUnwritable(scratchPath);
  Storage.makeUnreadable(scratchPath);
  ASSERT_TRUE(epub.ensureCssCache());
  EXPECT_EQ(Storage.openWriteAttemptsFor(scratchPath), 0U);
  EXPECT_EQ(Storage.openReadAttemptsFor(scratchPath), 0U);
  EXPECT_EQ(Storage.invalidOperationCount(), 0U);
  EXPECT_FALSE(Storage.exists(scratchPath.c_str()));

  ASSERT_TRUE(epub.getCssParser()->loadFromCache());
  EXPECT_EQ(epub.getCssParser()->resolveStyle("p", "note").textAlign, CssTextAlign::Right);
}

TEST_F(EpubSourceIdentityTest, ContentOpfItemScratchIoFaultsFailClosedWithoutInvalidHandleAccess) {
  enum class Fault { OpenWrite, ShortWrite, CloseWriter, OpenRead, ShortRead };
  for (const Fault fault :
       {Fault::OpenWrite, Fault::ShortWrite, Fault::CloseWriter, Fault::OpenRead, Fault::ShortRead}) {
    SCOPED_TRACE(static_cast<int>(fault));
    Storage.reset();
    ASSERT_TRUE(Storage.mkdir("/.crosspoint"));
    ASSERT_TRUE(Storage.mkdir(CACHE_PATH));
    identify(makeCssTestEpub());

    Epub epub(EPUB_PATH, "/.crosspoint");
    const std::string cachePath = epub.getCachePath();
    const std::string scratchPath = cachePath + "/.items.bin";
    ASSERT_TRUE(epub.bindCurrentSource());
    switch (fault) {
      case Fault::OpenWrite:
        Storage.makeUnwritable(scratchPath);
        break;
      case Fault::ShortWrite:
        Storage.shortWriteFor(scratchPath);
        break;
      case Fault::CloseWriter:
        Storage.failCloseFor(scratchPath);
        break;
      case Fault::OpenRead:
        Storage.makeUnreadable(scratchPath);
        break;
      case Fault::ShortRead:
        Storage.shortReadFor(scratchPath);
        break;
    }

    EXPECT_FALSE(epub.load(true, true));
    EXPECT_FALSE(Storage.exists((cachePath + "/book.bin").c_str()));
    EXPECT_FALSE(Storage.exists(scratchPath.c_str()));
    EXPECT_EQ(Storage.invalidOperationCount(), 0U);
  }
}

TEST_F(EpubSourceIdentityTest, CssCachePublishFaultsLeaveNoCommittedOrTemporaryCache) {
  enum class Fault { OpenWrite, ShortWrite, Sync, Close, Rename, CorruptRename };
  for (const Fault fault :
       {Fault::OpenWrite, Fault::ShortWrite, Fault::Sync, Fault::Close, Fault::Rename, Fault::CorruptRename}) {
    SCOPED_TRACE(static_cast<int>(fault));
    Storage.reset();
    ASSERT_TRUE(Storage.mkdir("/.crosspoint"));
    ASSERT_TRUE(Storage.mkdir(CACHE_PATH));
    const auto identity = identify(makeCssTestEpub());
    Epub epub(EPUB_PATH, "/.crosspoint");
    const std::string cachePath = epub.getCachePath();
    const std::string canonicalPath = cachePath + "/css_rules.cache";
    const std::string temporaryPath = cachePath + "/css_rules.cache.tmp";
    ASSERT_TRUE(epub.bindCurrentSource());
    Storage.setFile(cachePath + "/book.bin", makeBookCache(identity));
    ASSERT_TRUE(epub.load(false, true));

    switch (fault) {
      case Fault::OpenWrite:
        Storage.makeUnwritable(temporaryPath);
        break;
      case Fault::ShortWrite:
        Storage.shortWriteFor(temporaryPath);
        break;
      case Fault::Sync:
        Storage.failSyncOnce();
        break;
      case Fault::Close:
        Storage.failCloseFor(temporaryPath);
        break;
      case Fault::Rename:
        Storage.failRenameOnce();
        break;
      case Fault::CorruptRename:
        Storage.corruptRenameOnce();
        break;
    }

    EXPECT_FALSE(epub.ensureCssCache());
    EXPECT_FALSE(Storage.exists(canonicalPath.c_str()));
    EXPECT_FALSE(Storage.exists(temporaryPath.c_str()));
    EXPECT_TRUE(epub.getCssParser()->empty());
    EXPECT_EQ(epub.getTitle(), "A safe title");
    EXPECT_EQ(Storage.invalidOperationCount(), 0U);
  }
}

TEST_F(EpubSourceIdentityTest, CssCacheRejectsBitFlipTruncationAndTrailingBytes) {
  const auto identity = identify(makeCssTestEpub());
  Epub epub(EPUB_PATH, "/.crosspoint");
  const std::string cachePath = epub.getCachePath();
  const std::string canonicalPath = cachePath + "/css_rules.cache";
  ASSERT_TRUE(Storage.mkdir(cachePath.c_str()));
  ASSERT_TRUE(epub.bindCurrentSource());
  Storage.setFile(cachePath + "/book.bin", makeBookCache(identity));
  ASSERT_TRUE(epub.load(false, true));
  ASSERT_TRUE(epub.ensureCssCache());
  const auto validCache = Storage.file(canonicalPath);
  ASSERT_GT(validCache.size(), 8U);

  for (int mutation = 0; mutation < 3; ++mutation) {
    SCOPED_TRACE(mutation);
    auto corrupted = validCache;
    if (mutation == 0) corrupted[corrupted.size() - sizeof(uint32_t) - 1U] ^= 0x40U;
    if (mutation == 1) corrupted.pop_back();
    if (mutation == 2) corrupted.push_back(0xA5U);
    Storage.setFile(canonicalPath, std::move(corrupted));
    EXPECT_FALSE(epub.getCssParser()->loadFromCache());
    EXPECT_TRUE(epub.getCssParser()->empty());
    EXPECT_FALSE(Storage.exists(canonicalPath.c_str()));
  }
}

TEST_F(EpubSourceIdentityTest, CssCacheRejectsSemanticGarbageWithValidCrc) {
  const auto identity = identify(makeCssTestEpub());
  Epub epub(EPUB_PATH, "/.crosspoint");
  const std::string cachePath = epub.getCachePath();
  const std::string canonicalPath = cachePath + "/css_rules.cache";
  ASSERT_TRUE(Storage.mkdir(cachePath.c_str()));
  ASSERT_TRUE(epub.bindCurrentSource());
  Storage.setFile(cachePath + "/book.bin", makeBookCache(identity));
  ASSERT_TRUE(epub.load(false, true));
  ASSERT_TRUE(epub.ensureCssCache());
  const auto validCache = Storage.file(canonicalPath);
  ASSERT_GT(validCache.size(), 70U);

  uint16_t selectorLength = 0;
  memcpy(&selectorLength, validCache.data() + 3, sizeof(selectorLength));
  const size_t styleOffset = 5U + selectorLength;
  constexpr size_t styleEnumBytes = 6U;
  constexpr size_t firstLengthUnitOffset = styleEnumBytes + sizeof(float);
  constexpr size_t definedBitsOffset = styleEnumBytes + 11U * (sizeof(float) + sizeof(uint8_t)) + 2U;
  ASSERT_LE(styleOffset + definedBitsOffset + sizeof(uint32_t), validCache.size());

  for (int mutation = 0; mutation < 4; ++mutation) {
    SCOPED_TRACE(mutation);
    auto corrupted = validCache;
    if (mutation == 0) corrupted[styleOffset] = 0xFFU;
    if (mutation == 1) {
      const float nan = std::numeric_limits<float>::quiet_NaN();
      memcpy(corrupted.data() + styleOffset + styleEnumBytes, &nan, sizeof(nan));
    }
    if (mutation == 2) corrupted[styleOffset + firstLengthUnitOffset] = 0xFFU;
    if (mutation == 3) {
      uint32_t definedBits = 0;
      memcpy(&definedBits, corrupted.data() + styleOffset + definedBitsOffset, sizeof(definedBits));
      definedBits |= 1U << 19U;
      memcpy(corrupted.data() + styleOffset + definedBitsOffset, &definedBits, sizeof(definedBits));
    }
    const uint32_t crc = SourceIdentityCodec::crc32(corrupted.data(), corrupted.size() - sizeof(uint32_t));
    memcpy(corrupted.data() + corrupted.size() - sizeof(uint32_t), &crc, sizeof(crc));
    Storage.setFile(canonicalPath, std::move(corrupted));
    EXPECT_FALSE(epub.getCssParser()->loadFromCache());
    EXPECT_TRUE(epub.getCssParser()->empty());
    EXPECT_FALSE(Storage.exists(canonicalPath.c_str()));
  }
}

TEST_F(EpubSourceIdentityTest, CssCacheTempOnlyRecoveryRebuildsVerifiedCache) {
  const auto identity = identify(makeCssTestEpub());
  Epub epub(EPUB_PATH, "/.crosspoint");
  const std::string cachePath = epub.getCachePath();
  const std::string canonicalPath = cachePath + "/css_rules.cache";
  const std::string temporaryPath = cachePath + "/css_rules.cache.tmp";
  ASSERT_TRUE(Storage.mkdir(cachePath.c_str()));
  ASSERT_TRUE(epub.bindCurrentSource());
  Storage.setFile(cachePath + "/book.bin", makeBookCache(identity));
  ASSERT_TRUE(epub.load(false, true));
  ASSERT_TRUE(epub.ensureCssCache());
  const auto validCache = Storage.file(canonicalPath);
  ASSERT_TRUE(Storage.remove(canonicalPath.c_str()));
  Storage.setFile(temporaryPath, validCache);

  ASSERT_TRUE(epub.ensureCssCache());
  EXPECT_TRUE(Storage.exists(canonicalPath.c_str()));
  EXPECT_FALSE(Storage.exists(temporaryPath.c_str()));
  ASSERT_TRUE(epub.getCssParser()->loadFromCache());
  EXPECT_EQ(epub.getCssParser()->resolveStyle("p", "note").textAlign, CssTextAlign::Right);
}

TEST_F(EpubSourceIdentityTest, CssLengthToPixelsInt16ClampsNonFiniteAndRange) {
  EXPECT_EQ(CssLength(std::numeric_limits<float>::quiet_NaN()).toPixelsInt16(1), 0);
  EXPECT_EQ(CssLength(std::numeric_limits<float>::infinity()).toPixelsInt16(1), std::numeric_limits<int16_t>::max());
  EXPECT_EQ(CssLength(-std::numeric_limits<float>::infinity()).toPixelsInt16(1), std::numeric_limits<int16_t>::min());
  EXPECT_EQ(CssLength(100000.0F).toPixelsInt16(1), std::numeric_limits<int16_t>::max());
  EXPECT_EQ(CssLength(-100000.0F).toPixelsInt16(1), std::numeric_limits<int16_t>::min());
}

TEST_F(EpubSourceIdentityTest, OversizedStylesheetCannotPublishAPartialCssCache) {
  constexpr size_t MAX_SUPPORTED_CSS_SIZE = 128U * 1024U;
  const auto identity = identify(makeCssTestEpub(std::string(MAX_SUPPORTED_CSS_SIZE + 1U, 'x')));
  Epub epub(EPUB_PATH, "/.crosspoint");
  const std::string cachePath = epub.getCachePath();
  ASSERT_TRUE(Storage.mkdir(cachePath.c_str()));
  ASSERT_TRUE(epub.bindCurrentSource());
  Storage.setFile(cachePath + "/book.bin", makeBookCache(identity));
  ASSERT_TRUE(epub.load(false, true));

  EXPECT_FALSE(epub.ensureCssCache());
  EXPECT_FALSE(Storage.exists((cachePath + "/css_rules.cache").c_str()));
  EXPECT_FALSE(Storage.exists((cachePath + "/.tmp.css").c_str()));
  EXPECT_EQ(epub.getTitle(), "A safe title");
}

TEST_F(EpubSourceIdentityTest, CssCacheReadBackFailureFailsClosedAndKeepsBookMetadataLoaded) {
  const auto identity = identify(makeCssTestEpub());
  Epub epub(EPUB_PATH, "/.crosspoint");
  const std::string cachePath = epub.getCachePath();
  ASSERT_TRUE(Storage.mkdir(cachePath.c_str()));
  ASSERT_TRUE(epub.bindCurrentSource());
  Storage.setFile(cachePath + "/book.bin", makeBookCache(identity));
  ASSERT_TRUE(epub.load(false, true));

  const std::string cssCachePath = cachePath + "/css_rules.cache";
  Storage.makeUnreadable(cssCachePath);
  EXPECT_FALSE(epub.ensureCssCache());
  EXPECT_FALSE(Storage.exists(cssCachePath.c_str()));
  EXPECT_EQ(epub.getTitle(), "A safe title");
}

TEST_F(EpubSourceIdentityTest, BookLoadDegradesSafelyWhenCssCacheCannotBeVerified) {
  const auto identity = identify(makeCssTestEpub());
  Epub epub(EPUB_PATH, "/.crosspoint");
  const std::string cachePath = epub.getCachePath();
  ASSERT_TRUE(Storage.mkdir(cachePath.c_str()));
  ASSERT_TRUE(epub.bindCurrentSource());
  Storage.setFile(cachePath + "/book.bin", makeBookCache(identity));
  Storage.makeUnreadable(cachePath + "/css_rules.cache");

  EXPECT_TRUE(epub.load(false, false));
  EXPECT_EQ(epub.getTitle(), "A safe title");
  EXPECT_TRUE(epub.isExternalCssUnavailable());
  EXPECT_FALSE(Storage.exists((cachePath + "/css_rules.cache").c_str()));
}

TEST_F(EpubSourceIdentityTest, BookLoadRejectsStaleSectionsWhenCssInvalidationFails) {
  const auto identity = identify(makeCssTestEpub());
  Epub epub(EPUB_PATH, "/.crosspoint");
  const std::string cachePath = epub.getCachePath();
  ASSERT_TRUE(Storage.mkdir(cachePath.c_str()));
  ASSERT_TRUE(epub.bindCurrentSource());
  Storage.setFile(cachePath + "/book.bin", makeBookCache(identity));
  const std::string sectionsPath = cachePath + "/sections";
  ASSERT_TRUE(Storage.mkdir(sectionsPath.c_str()));
  Storage.setFile(sectionsPath + "/section_0.bin", {0xCC});
  Storage.failRemoveDirOnce();

  EXPECT_FALSE(epub.load(false, false));
  EXPECT_TRUE(Storage.exists((sectionsPath + "/section_0.bin").c_str()));
}

TEST_F(EpubSourceIdentityTest, StorePublishesRecoversAndProtectsNewerSibling) {
  const auto first = identify(makeZip());
  const auto second = identify(makeZip(0x87654321U));
  ASSERT_EQ(SourceIdentityStore::save(CACHE_PATH, first), SourceIdentityStore::SaveStatus::Saved);
  ASSERT_EQ(SourceIdentityStore::save(CACHE_PATH, second), SourceIdentityStore::SaveStatus::Saved);

  ZipFile::SourceIdentity loaded;
  EXPECT_EQ(SourceIdentityStore::load(CACHE_PATH, loaded), SourceIdentityStore::LoadStatus::Primary);
  EXPECT_EQ(loaded, second);

  auto& primary = Storage.mutableFile(std::string(CACHE_PATH) + "/source_identity.bin");
  primary[SourceIdentityCodec::PAYLOAD_OFFSET] ^= 1U;
  EXPECT_EQ(SourceIdentityStore::load(CACHE_PATH, loaded), SourceIdentityStore::LoadStatus::Backup);
  EXPECT_EQ(loaded, first);

  auto newer = Storage.file(std::string(CACHE_PATH) + "/source_identity.bin.bak");
  newer[SourceIdentityCodec::VERSION_OFFSET] = SourceIdentityCodec::VERSION + 1;
  Storage.setFile(std::string(CACHE_PATH) + "/source_identity.bin.bak", std::move(newer));
  EXPECT_EQ(SourceIdentityStore::save(CACHE_PATH, second), SourceIdentityStore::SaveStatus::NewerVersion);
}

TEST_F(EpubSourceIdentityTest, StoreWriteFaultLeavesVerifiedFallback) {
  const auto first = identify(makeZip());
  const auto second = identify(makeZip(0x87654321U));
  ASSERT_EQ(SourceIdentityStore::save(CACHE_PATH, first), SourceIdentityStore::SaveStatus::Saved);
  Storage.failRenameOnce();
  EXPECT_EQ(SourceIdentityStore::save(CACHE_PATH, second), SourceIdentityStore::SaveStatus::IoError);

  ZipFile::SourceIdentity loaded;
  EXPECT_EQ(SourceIdentityStore::load(CACHE_PATH, loaded), SourceIdentityStore::LoadStatus::Primary);
  EXPECT_EQ(loaded, first);
}

TEST_F(EpubSourceIdentityTest, TrulyMissingSidecarCanBeAdoptedForLegacyMigration) {
  ZipFile::SourceIdentity loaded;
  EXPECT_EQ(SourceIdentityStore::load(CACHE_PATH, loaded), SourceIdentityStore::LoadStatus::Missing);
  const auto current = identify(makeZip());
  EXPECT_EQ(SourceIdentityStore::save(CACHE_PATH, current), SourceIdentityStore::SaveStatus::Saved);
  EXPECT_EQ(SourceIdentityStore::load(CACHE_PATH, loaded), SourceIdentityStore::LoadStatus::Primary);
  EXPECT_EQ(loaded, current);
}

TEST_F(EpubSourceIdentityTest, PreparedReplacementRetainsOldIdentityAndCancelsAfterReboot) {
  const auto oldIdentity = identify(makeZip());
  ASSERT_EQ(SourceIdentityStore::prepareReplacement(CACHE_PATH, &oldIdentity),
            SourceIdentityStore::PrepareReplacementStatus::Prepared);

  ZipFile::SourceIdentity loaded;
  ASSERT_EQ(SourceIdentityStore::load(CACHE_PATH, loaded), SourceIdentityStore::LoadStatus::Primary);
  EXPECT_TRUE(SourceIdentityStore::isReplacementBarrier(loaded));

  EXPECT_EQ(SourceIdentityStore::recoverReplacement(CACHE_PATH, oldIdentity),
            SourceIdentityStore::RecoverReplacementStatus::RestoredCurrentSource);
  EXPECT_EQ(SourceIdentityStore::load(CACHE_PATH, loaded), SourceIdentityStore::LoadStatus::Primary);
  EXPECT_EQ(loaded, oldIdentity);
}

TEST_F(EpubSourceIdentityTest, PreparedReplacementKeepsBarrierForPublishedNewSource) {
  const auto oldIdentity = identify(makeZip());
  const auto newIdentity = identify(makeZip(0x87654321U));
  ASSERT_EQ(SourceIdentityStore::save(CACHE_PATH, oldIdentity), SourceIdentityStore::SaveStatus::Saved);
  ASSERT_EQ(SourceIdentityStore::prepareReplacement(CACHE_PATH, &oldIdentity),
            SourceIdentityStore::PrepareReplacementStatus::Prepared);

  EXPECT_EQ(SourceIdentityStore::recoverReplacement(CACHE_PATH, newIdentity),
            SourceIdentityStore::RecoverReplacementStatus::ReplacementPublished);
  ZipFile::SourceIdentity loaded;
  ASSERT_EQ(SourceIdentityStore::load(CACHE_PATH, loaded), SourceIdentityStore::LoadStatus::Primary);
  EXPECT_TRUE(SourceIdentityStore::isReplacementBarrier(loaded));
}

TEST_F(EpubSourceIdentityTest, LegacyStateGetsRecoverableOldIdentityBeforeBarrier) {
  ZipFile::SourceIdentity missing;
  ASSERT_EQ(SourceIdentityStore::load(CACHE_PATH, missing), SourceIdentityStore::LoadStatus::Missing);
  const auto currentIdentity = identify(makeZip());

  ASSERT_EQ(SourceIdentityStore::prepareReplacement(CACHE_PATH, &currentIdentity),
            SourceIdentityStore::PrepareReplacementStatus::Prepared);
  EXPECT_EQ(SourceIdentityStore::recoverReplacement(CACHE_PATH, currentIdentity),
            SourceIdentityStore::RecoverReplacementStatus::RestoredCurrentSource);
}

TEST_F(EpubSourceIdentityTest, BarrierPreparationFaultLeavesOldIdentityLoadable) {
  const auto oldIdentity = identify(makeZip());
  ASSERT_EQ(SourceIdentityStore::save(CACHE_PATH, oldIdentity), SourceIdentityStore::SaveStatus::Saved);
  Storage.failRenameOnce();

  EXPECT_EQ(SourceIdentityStore::prepareReplacement(CACHE_PATH, &oldIdentity),
            SourceIdentityStore::PrepareReplacementStatus::IoError);
  ZipFile::SourceIdentity loaded;
  EXPECT_EQ(SourceIdentityStore::load(CACHE_PATH, loaded), SourceIdentityStore::LoadStatus::Primary);
  EXPECT_EQ(loaded, oldIdentity);
}

TEST_F(EpubSourceIdentityTest, FailedNewPathPublicationCancelsBarrierWithoutInventingIdentity) {
  ASSERT_EQ(SourceIdentityStore::prepareReplacement(CACHE_PATH, nullptr),
            SourceIdentityStore::PrepareReplacementStatus::Prepared);
  ASSERT_TRUE(SourceIdentityStore::cancelReplacement(CACHE_PATH));

  ZipFile::SourceIdentity loaded;
  EXPECT_EQ(SourceIdentityStore::load(CACHE_PATH, loaded), SourceIdentityStore::LoadStatus::Missing);
}

TEST_F(EpubSourceIdentityTest, SyncFaultDuringPreparationDoesNotHideOldIdentity) {
  const auto oldIdentity = identify(makeZip());
  ASSERT_EQ(SourceIdentityStore::save(CACHE_PATH, oldIdentity), SourceIdentityStore::SaveStatus::Saved);
  Storage.failSyncOnce();

  EXPECT_EQ(SourceIdentityStore::prepareReplacement(CACHE_PATH, &oldIdentity),
            SourceIdentityStore::PrepareReplacementStatus::IoError);
  ZipFile::SourceIdentity loaded;
  EXPECT_EQ(SourceIdentityStore::load(CACHE_PATH, loaded), SourceIdentityStore::LoadStatus::Primary);
  EXPECT_EQ(loaded, oldIdentity);
}

TEST_F(EpubSourceIdentityTest, RecoveryNeverDeletesNewerTemporarySidecar) {
  const auto oldIdentity = identify(makeZip());
  ASSERT_EQ(SourceIdentityStore::save(CACHE_PATH, oldIdentity), SourceIdentityStore::SaveStatus::Saved);
  ASSERT_EQ(SourceIdentityStore::prepareReplacement(CACHE_PATH, &oldIdentity),
            SourceIdentityStore::PrepareReplacementStatus::Prepared);

  const std::string tempPath = std::string(CACHE_PATH) + "/source_identity.bin.tmp";
  auto newer = Storage.file(std::string(CACHE_PATH) + "/source_identity.bin.bak");
  newer[SourceIdentityCodec::VERSION_OFFSET] = SourceIdentityCodec::VERSION + 1;
  Storage.setFile(tempPath, newer);

  EXPECT_EQ(SourceIdentityStore::recoverReplacement(CACHE_PATH, oldIdentity),
            SourceIdentityStore::RecoverReplacementStatus::NewerVersion);
  EXPECT_FALSE(SourceIdentityStore::cancelReplacement(CACHE_PATH));
  EXPECT_EQ(Storage.file(tempPath), newer);
}

TEST_F(EpubSourceIdentityTest, RecoveryProtectsSiblingsWhenBarrierPrimaryIsMissing) {
  const auto oldIdentity = identify(makeZip());
  const auto newIdentity = identify(makeZip(0x87654321U));
  ASSERT_EQ(SourceIdentityStore::prepareReplacement(CACHE_PATH, &oldIdentity),
            SourceIdentityStore::PrepareReplacementStatus::Prepared);

  const std::string primaryPath = std::string(CACHE_PATH) + "/source_identity.bin";
  const std::string tempPath = primaryPath + ".tmp";
  ASSERT_TRUE(Storage.rename(primaryPath.c_str(), tempPath.c_str()));

  EXPECT_EQ(SourceIdentityStore::recoverReplacement(CACHE_PATH, newIdentity),
            SourceIdentityStore::RecoverReplacementStatus::ReplacementPublished);
  EXPECT_TRUE(Storage.exists(tempPath.c_str()));

  EXPECT_EQ(SourceIdentityStore::recoverReplacement(CACHE_PATH, oldIdentity),
            SourceIdentityStore::RecoverReplacementStatus::RestoredCurrentSource);
  ZipFile::SourceIdentity loaded;
  EXPECT_EQ(SourceIdentityStore::load(CACHE_PATH, loaded), SourceIdentityStore::LoadStatus::Primary);
  EXPECT_EQ(loaded, oldIdentity);
}

TEST_F(EpubSourceIdentityTest, RecoveryProtectsNewerTempWhenPrimaryIsMissing) {
  const auto oldIdentity = identify(makeZip());
  ASSERT_EQ(SourceIdentityStore::save(CACHE_PATH, oldIdentity), SourceIdentityStore::SaveStatus::Saved);

  const std::string primaryPath = std::string(CACHE_PATH) + "/source_identity.bin";
  const std::string tempPath = primaryPath + ".tmp";
  ASSERT_TRUE(Storage.rename(primaryPath.c_str(), tempPath.c_str()));
  auto newer = Storage.file(tempPath);
  newer[SourceIdentityCodec::VERSION_OFFSET] = SourceIdentityCodec::VERSION + 1;
  Storage.setFile(tempPath, newer);

  EXPECT_EQ(SourceIdentityStore::recoverReplacement(CACHE_PATH, oldIdentity),
            SourceIdentityStore::RecoverReplacementStatus::NewerVersion);
  EXPECT_EQ(Storage.file(tempPath), newer);
}

}  // namespace
