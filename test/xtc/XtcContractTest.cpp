#include <Bitmap.h>
#include <HalStorage.h>
#include <Xtc.h>
#include <Xtc/XtcPageLayout.h>
#include <Xtc/XtcParser.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr char BOOK_PATH[] = "/book.xtc";

void writeU16(std::vector<uint8_t>& bytes, const size_t offset, const uint16_t value) {
  bytes[offset] = static_cast<uint8_t>(value);
  bytes[offset + 1] = static_cast<uint8_t>(value >> 8U);
}

void writeU32(std::vector<uint8_t>& bytes, const size_t offset, const uint32_t value) {
  for (uint8_t index = 0; index < 4; ++index) bytes[offset + index] = static_cast<uint8_t>(value >> (index * 8U));
}

void writeU64(std::vector<uint8_t>& bytes, const size_t offset, const uint64_t value) {
  for (uint8_t index = 0; index < 8; ++index) bytes[offset + index] = static_cast<uint8_t>(value >> (index * 8U));
}

std::vector<uint8_t> makeBook(const uint8_t bitDepth = 1, const uint16_t width = 480, const uint16_t height = 800,
                              const bool chapters = false) {
  xtc::PageLayout layout;
  EXPECT_TRUE(xtc::calculatePageLayout(width, height, bitDepth, layout));
  const size_t chapterBytes = chapters ? xtc::XTC_CHAPTER_SIZE : 0;
  const size_t metadataOffset = xtc::XTC_HEADER_SIZE;
  const size_t chapterOffset = metadataOffset + xtc::XTC_METADATA_SIZE;
  const size_t tableOffset = chapterOffset + chapterBytes;
  const size_t pageOffset = tableOffset + sizeof(xtc::PageTableEntry);
  const size_t pageBytes = sizeof(xtc::XtgPageHeader) + layout.payloadBytes;
  std::vector<uint8_t> bytes(pageOffset + pageBytes, 0);

  writeU32(bytes, 0, bitDepth == 2 ? xtc::XTCH_MAGIC : xtc::XTC_MAGIC);
  bytes[4] = 1;
  bytes[5] = 0;
  writeU16(bytes, 6, 1);
  bytes[9] = 1;
  bytes[11] = chapters ? 1 : 0;
  writeU32(bytes, 12, 1);
  writeU64(bytes, 16, metadataOffset);
  writeU64(bytes, 24, tableOffset);
  writeU64(bytes, 32, pageOffset);
  writeU64(bytes, 48, chapters ? chapterOffset : 0);

  std::memcpy(bytes.data() + metadataOffset, "CrossVi fixture", 16);
  std::memcpy(bytes.data() + metadataOffset + 128, "CrossVi tests", 13);
  if (chapters) {
    writeU16(bytes, metadataOffset + 196, 1);
    std::memcpy(bytes.data() + chapterOffset, "Chapter 1", 10);
    writeU16(bytes, chapterOffset + 0x50, 1);
    writeU16(bytes, chapterOffset + 0x52, 1);
  }

  writeU64(bytes, tableOffset, pageOffset);
  writeU32(bytes, tableOffset + 8, static_cast<uint32_t>(pageBytes));
  writeU16(bytes, tableOffset + 12, width);
  writeU16(bytes, tableOffset + 14, height);

  writeU32(bytes, pageOffset, bitDepth == 2 ? xtc::XTH_MAGIC : xtc::XTG_MAGIC);
  writeU16(bytes, pageOffset + 4, width);
  writeU16(bytes, pageOffset + 6, height);
  writeU32(bytes, pageOffset + 10, static_cast<uint32_t>(layout.payloadBytes));
  std::fill(bytes.begin() + static_cast<std::ptrdiff_t>(pageOffset + sizeof(xtc::XtgPageHeader)), bytes.end(),
            bitDepth == 1 ? 0xFFU : 0x00U);
  return bytes;
}

std::vector<uint8_t> makeBookWithPages(const uint16_t pageCount) {
  xtc::PageLayout layout;
  EXPECT_TRUE(xtc::calculatePageLayout(480, 800, 1, layout));
  const size_t metadataOffset = xtc::XTC_HEADER_SIZE;
  const size_t tableOffset = metadataOffset + xtc::XTC_METADATA_SIZE;
  const size_t pageBytes = sizeof(xtc::XtgPageHeader) + layout.payloadBytes;
  const size_t dataOffset = tableOffset + static_cast<size_t>(pageCount) * sizeof(xtc::PageTableEntry);
  std::vector<uint8_t> bytes(dataOffset + static_cast<size_t>(pageCount) * pageBytes, 0);

  writeU32(bytes, 0, xtc::XTC_MAGIC);
  bytes[4] = 1;
  writeU16(bytes, 6, pageCount);
  bytes[9] = 1;
  writeU32(bytes, 12, 1);
  writeU64(bytes, 16, metadataOffset);
  writeU64(bytes, 24, tableOffset);
  writeU64(bytes, 32, dataOffset);
  std::memcpy(bytes.data() + metadataOffset, "CrossVi fixture", 16);
  std::memcpy(bytes.data() + metadataOffset + 128, "CrossVi tests", 13);

  for (uint16_t page = 0; page < pageCount; ++page) {
    const size_t entryOffset = tableOffset + static_cast<size_t>(page) * sizeof(xtc::PageTableEntry);
    const size_t pageOffset = dataOffset + static_cast<size_t>(page) * pageBytes;
    writeU64(bytes, entryOffset, pageOffset);
    writeU32(bytes, entryOffset + 8, static_cast<uint32_t>(pageBytes));
    writeU16(bytes, entryOffset + 12, 480);
    writeU16(bytes, entryOffset + 14, 800);
    writeU32(bytes, pageOffset, xtc::XTG_MAGIC);
    writeU16(bytes, pageOffset + 4, 480);
    writeU16(bytes, pageOffset + 6, 800);
    writeU32(bytes, pageOffset + 10, static_cast<uint32_t>(layout.payloadBytes));
    std::fill(bytes.begin() + static_cast<std::ptrdiff_t>(pageOffset + sizeof(xtc::XtgPageHeader)),
              bytes.begin() + static_cast<std::ptrdiff_t>(pageOffset + pageBytes), 0xFFU);
  }
  return bytes;
}

std::vector<uint8_t> makeValidBmp(const uint8_t pixel = 0x80U) {
  constexpr uint32_t pixelOffset = 14U + 40U + 8U;
  constexpr uint32_t fileSize = pixelOffset + 4U;
  std::vector<uint8_t> bytes(fileSize, 0);
  writeU16(bytes, 0, 0x4D42U);
  writeU32(bytes, 2, fileSize);
  writeU32(bytes, 10, pixelOffset);
  writeU32(bytes, 14, 40);
  writeU32(bytes, 18, 1);
  writeU32(bytes, 22, 1);
  writeU16(bytes, 26, 1);
  writeU16(bytes, 28, 1);
  writeU32(bytes, 34, 4);
  writeU32(bytes, 46, 2);
  bytes[58] = 0xFFU;
  bytes[59] = 0xFFU;
  bytes[60] = 0xFFU;
  bytes[pixelOffset] = pixel;
  return bytes;
}

uint64_t tableOffset(const std::vector<uint8_t>& bytes) {
  uint64_t value = 0;
  std::memcpy(&value, bytes.data() + 24, sizeof(value));
  return value;
}

uint64_t pageOffset(const std::vector<uint8_t>& bytes) {
  uint64_t value = 0;
  std::memcpy(&value, bytes.data() + 32, sizeof(value));
  return value;
}

xtc::XtcError finishOpen(xtc::XtcParser& parser) {
  const xtc::XtcError begin = parser.beginOpen(BOOK_PATH);
  if (begin != xtc::XtcError::OK) return begin;
  while (true) {
    const auto result = parser.stepOpen(16, 64U * 1024U);
    if (result == xtc::XtcParser::OpenStepResult::Opened) return xtc::XtcError::OK;
    if (result == xtc::XtcParser::OpenStepResult::Error) return parser.getLastError();
  }
}

xtc::XtcError openBook(std::vector<uint8_t> bytes, xtc::XtcParser* parser = nullptr) {
  Storage.setFile(BOOK_PATH, std::move(bytes));
  xtc::XtcParser local;
  return finishOpen(parser ? *parser : local);
}

Xtc::ThumbnailPreparationStatus finishThumbnailPreparation(Xtc& book, const int width = 273, const int height = 456) {
  Xtc::ThumbnailPreparationStatus status = book.beginThumbnailPreparation(width, height);
  while (status == Xtc::ThumbnailPreparationStatus::InProgress) {
    status = book.stepThumbnailPreparation(1024, 8);
  }
  return status;
}

std::vector<uint8_t> readFixture(const char* name) {
  std::ifstream file(std::string(XTC_CONVERTER_FIXTURE_DIR) + "/" + name, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

class XtcContractTest : public testing::Test {
 protected:
  void SetUp() override { Storage.reset(); }
};

TEST_F(XtcContractTest, CheckedSizesMatchRowAndColumnStorage) {
  xtc::PageLayout xtg;
  ASSERT_TRUE(xtc::calculatePageLayout(480, 800, 1, xtg));
  EXPECT_EQ(xtg.rowBytes, 60U);
  EXPECT_EQ(xtg.payloadBytes, 48000U);

  xtc::PageLayout xth;
  ASSERT_TRUE(xtc::calculatePageLayout(480, 799, 2, xth));
  EXPECT_EQ(xth.columnBytes, 100U);
  EXPECT_EQ(xth.planeBytes, 48000U);
  EXPECT_EQ(xth.payloadBytes, 96000U);
  EXPECT_FALSE(xtc::calculatePageLayout(0, 800, 1, xtg));
  EXPECT_FALSE(xtc::calculatePageLayout(480, 0, 2, xth));
  size_t ignored = 0;
  EXPECT_FALSE(xtc::checkedMultiply(std::numeric_limits<size_t>::max(), 2, ignored));
}

TEST_F(XtcContractTest, OpensSupportedXtcAndXtch) {
  EXPECT_EQ(openBook(makeBook(1)), xtc::XtcError::OK);
  EXPECT_EQ(openBook(makeBook(2)), xtc::XtcError::OK);
}

TEST_F(XtcContractTest, ValidatesPageTableInBoundedSequentialBlocks) {
  Storage.setFile(BOOK_PATH, makeBookWithPages(16));
  xtc::XtcParser parser;

  ASSERT_EQ(finishOpen(parser), xtc::XtcError::OK);
  EXPECT_LE(Storage.seekCalls(), 5U);
  EXPECT_LE(Storage.maxRead(), 2048U);
}

TEST_F(XtcContractTest, DefersPageHeaderValidationUntilThePageIsRead) {
  auto bytes = makeBookWithPages(2);
  const uint64_t firstPageOffset = pageOffset(bytes);
  xtc::PageLayout layout;
  ASSERT_TRUE(xtc::calculatePageLayout(480, 800, 1, layout));
  const size_t secondPageOffset =
      static_cast<size_t>(firstPageOffset) + sizeof(xtc::XtgPageHeader) + layout.payloadBytes;
  writeU32(bytes, secondPageOffset, xtc::XTH_MAGIC);

  xtc::XtcParser parser;
  ASSERT_EQ(openBook(std::move(bytes), &parser), xtc::XtcError::OK);
  size_t firstPageBytes = 0;
  EXPECT_EQ(parser.loadPageStreaming(
                0, [&](const uint8_t*, const size_t size, size_t) { firstPageBytes += size; }, 1024),
            xtc::XtcError::OK);
  EXPECT_EQ(firstPageBytes, layout.payloadBytes);
  EXPECT_EQ(parser.loadPageStreaming(1, [](const uint8_t*, size_t, size_t) {}, 1024), xtc::XtcError::INVALID_MAGIC);
}

TEST_F(XtcContractTest, CooperativeOpenBoundsValidationAndSupportsCancellation) {
  Storage.setFile(BOOK_PATH, makeBookWithPages(16));
  xtc::XtcParser parser;

  ASSERT_EQ(parser.beginOpen(BOOK_PATH), xtc::XtcError::OK);
  EXPECT_EQ(parser.stepOpen(2, 1024), xtc::XtcParser::OpenStepResult::InProgress);
  EXPECT_FALSE(parser.isOpen());
  parser.cancelOpen();
  EXPECT_FALSE(parser.isOpen());

  ASSERT_EQ(parser.beginOpen(BOOK_PATH), xtc::XtcError::OK);
  xtc::XtcParser::OpenStepResult result = xtc::XtcParser::OpenStepResult::InProgress;
  size_t steps = 0;
  while (result == xtc::XtcParser::OpenStepResult::InProgress) {
    result = parser.stepOpen(2, 1024);
    ++steps;
  }
  EXPECT_EQ(result, xtc::XtcParser::OpenStepResult::Opened);
  EXPECT_TRUE(parser.isOpen());
  EXPECT_GT(steps, 16U);
  EXPECT_LE(Storage.maxRead(), 1024U);
}

TEST_F(XtcContractTest, PreparedIdentitySkipsASecondWholeFileFingerprint) {
  Storage.setFile(BOOK_PATH, makeBookWithPages(16));
  Xtc prepared(BOOK_PATH, "/.crosspoint");
  ASSERT_TRUE(prepared.load());
  const size_t fullReadCalls = Storage.readCalls();
  RawSourceIdentityHandoff handoff;
  ASSERT_TRUE(prepared.getSourceIdentityHandoff(handoff));

  Storage.resetIoCounters();
  Xtc reopened(BOOK_PATH, "/.crosspoint");
  ASSERT_TRUE(reopened.beginLoad(&handoff));
  while (reopened.stepLoad(2, 1024) == Xtc::LoadStepResult::InProgress) {
  }
  ASSERT_TRUE(reopened.isLoaded());
  EXPECT_LT(Storage.readCalls(), fullReadCalls);

  ZipFile::SourceIdentity identity;
  ASSERT_TRUE(reopened.getSourceIdentity(identity));
  EXPECT_EQ(identity, handoff.identity);
}

TEST_F(XtcContractTest, PreparedIdentityFallsBackAfterSameSizeSourceReplacement) {
  auto original = makeBook();
  Storage.setFile(BOOK_PATH, original);
  Xtc prepared(BOOK_PATH, "/.crosspoint");
  ASSERT_TRUE(prepared.load());
  RawSourceIdentityHandoff handoff;
  ASSERT_TRUE(prepared.getSourceIdentityHandoff(handoff));

  original.back() ^= 0x01U;
  Storage.setFile(BOOK_PATH, original);
  Storage.resetIoCounters();
  Xtc reopened(BOOK_PATH, "/.crosspoint");
  ASSERT_TRUE(reopened.beginLoad(&handoff));
  while (reopened.stepLoad(2, 1024) == Xtc::LoadStepResult::InProgress) {
  }
  ASSERT_TRUE(reopened.isLoaded());
  EXPECT_GT(Storage.readCalls(), 10U);

  ZipFile::SourceIdentity replacementIdentity;
  ASSERT_TRUE(reopened.getSourceIdentity(replacementIdentity));
  EXPECT_NE(replacementIdentity, handoff.identity);
  EXPECT_EQ(replacementIdentity.fileSize, handoff.identity.fileSize);
}

TEST_F(XtcContractTest, CoreMetadataProbeSkipsPageValidationAndWholeFileFingerprint) {
  auto bytes = makeBook();
  bytes[pageOffset(bytes)] ^= 0xFFU;
  Storage.setFile(BOOK_PATH, std::move(bytes));
  Xtc book(BOOK_PATH, "/.crosspoint");
  std::string title;
  std::string author;

  EXPECT_TRUE(book.readCoreMetadata(title, author));
  EXPECT_EQ(title, "CrossVi fixture");
  EXPECT_EQ(author, "CrossVi tests");
  EXPECT_EQ(Storage.openReadAttemptsFor(BOOK_PATH), 1U);
  EXPECT_LE(Storage.maxRead(), xtc::XTC_METADATA_SIZE);
  EXPECT_FALSE(book.isLoaded());
}

TEST_F(XtcContractTest, ValidatesChapterRecordsWithoutChangingLazyLoadBehavior) {
  xtc::XtcParser parser;
  ASSERT_EQ(openBook(makeBook(1, 480, 800, true), &parser), xtc::XtcError::OK);
  ASSERT_TRUE(parser.hasChapters());

  const auto& chapters = parser.getChapters();
  ASSERT_EQ(chapters.size(), 1U);
  EXPECT_EQ(chapters[0].name, "Chapter 1");
  EXPECT_EQ(chapters[0].startPage, 0U);
  EXPECT_EQ(chapters[0].endPage, 0U);
}

TEST_F(XtcContractTest, ThumbnailUsesVerifiedBackupRecoveryBeforeReadingTheBook) {
  Xtc book(BOOK_PATH, "/.crosspoint");
  const std::string sharedPath = book.getThumbBmpPath(Xtc::SHARED_THUMB_HEIGHT);
  const std::string carouselPath = book.getThumbBmpPath(456);
  const auto backup = makeValidBmp();
  Storage.setFile(sharedPath + ".bak", backup);
  Storage.setFile(carouselPath + ".bak", backup);

  EXPECT_EQ(book.beginThumbnailPreparation(273, 456), Xtc::ThumbnailPreparationStatus::Ready);
  EXPECT_EQ(Storage.file(sharedPath), backup);
  EXPECT_EQ(Storage.file(carouselPath), backup);
  EXPECT_FALSE(Storage.exists((sharedPath + ".bak").c_str()));
  EXPECT_FALSE(Storage.exists((carouselPath + ".bak").c_str()));
}

TEST_F(XtcContractTest, CarouselThumbnailFitsBeforeOneBitDithering) {
  Storage.setFile(BOOK_PATH, makeBook());
  Xtc book(BOOK_PATH, "/.crosspoint");
  ASSERT_TRUE(book.load());
  ASSERT_EQ(finishThumbnailPreparation(book), Xtc::ThumbnailPreparationStatus::Ready);

  HalFile file;
  ASSERT_TRUE(Storage.openFileForRead("TEST", book.getThumbBmpPath(456), file));
  Bitmap bitmap(file);
  ASSERT_EQ(bitmap.parseHeaders(), BmpReaderError::Ok);
  EXPECT_TRUE(bitmap.is1Bit());
  EXPECT_EQ(bitmap.getWidth(), 273);
  EXPECT_LE(bitmap.getHeight(), 456);
  EXPECT_TRUE(file.close());
}

TEST_F(XtcContractTest, PairedThumbnailsShareOneFirstPageReadForXtcAndXtch) {
  for (const uint8_t bitDepth : {1U, 2U}) {
    SCOPED_TRACE(bitDepth);
    Storage.reset();
    Storage.setFile(BOOK_PATH, makeBook(bitDepth));
    Xtc book(BOOK_PATH, "/.crosspoint");
    ASSERT_TRUE(book.load());
    Storage.resetIoCounters();

    ASSERT_EQ(finishThumbnailPreparation(book), Xtc::ThumbnailPreparationStatus::Ready);
    EXPECT_LE(Storage.openReadAttemptsFor(BOOK_PATH), static_cast<size_t>(bitDepth) + 1U);
    EXPECT_LE(Storage.maxRead(), 1024U);

    HalFile shared;
    ASSERT_TRUE(Storage.openFileForRead("TEST", book.getThumbBmpPath(Xtc::SHARED_THUMB_HEIGHT), shared));
    Bitmap sharedBitmap(shared);
    ASSERT_EQ(sharedBitmap.parseHeaders(), BmpReaderError::Ok);
    EXPECT_TRUE(sharedBitmap.is1Bit());
    EXPECT_EQ(sharedBitmap.getWidth(), Xtc::SHARED_THUMB_WIDTH);
    EXPECT_EQ(sharedBitmap.getHeight(), Xtc::SHARED_THUMB_HEIGHT);
    EXPECT_TRUE(shared.close());

    HalFile carousel;
    ASSERT_TRUE(Storage.openFileForRead("TEST", book.getThumbBmpPath(456), carousel));
    Bitmap carouselBitmap(carousel);
    ASSERT_EQ(carouselBitmap.parseHeaders(), BmpReaderError::Ok);
    EXPECT_TRUE(carouselBitmap.is1Bit());
    EXPECT_EQ(carouselBitmap.getWidth(), 273);
    EXPECT_LE(carouselBitmap.getHeight(), 456);
    EXPECT_TRUE(carousel.close());
  }
}

TEST_F(XtcContractTest, ThumbnailPairPreparationReadsAtMostOneBoundedChunkPerPlanePerStep) {
  for (const uint8_t bitDepth : {1U, 2U}) {
    SCOPED_TRACE(bitDepth);
    Storage.reset();
    Storage.setFile(BOOK_PATH, makeBook(bitDepth));
    Xtc book(BOOK_PATH, "/.crosspoint");
    ASSERT_TRUE(book.load());
    Storage.resetIoCounters();

    ASSERT_EQ(book.beginThumbnailPreparation(273, 456), Xtc::ThumbnailPreparationStatus::InProgress);
    const size_t readsBeforeStep = Storage.readCalls();
    Xtc::ThumbnailPreparationStatus status = book.stepThumbnailPreparation(1024, 8);
    EXPECT_EQ(status, Xtc::ThumbnailPreparationStatus::InProgress);
    EXPECT_LE(Storage.readCalls() - readsBeforeStep, bitDepth);
    EXPECT_LE(Storage.maxRead(), 1024U);
    EXPECT_FALSE(Storage.exists(book.getThumbBmpPath(Xtc::SHARED_THUMB_HEIGHT).c_str()));
    EXPECT_FALSE(Storage.exists(book.getThumbBmpPath(456).c_str()));

    size_t steps = 1;
    while (status == Xtc::ThumbnailPreparationStatus::InProgress && steps < 1000) {
      status = book.stepThumbnailPreparation(1024, 8);
      ++steps;
    }
    EXPECT_EQ(status, Xtc::ThumbnailPreparationStatus::Ready);
    EXPECT_GT(steps, 20U);
    EXPECT_FALSE(book.thumbnailPreparationActive());
    EXPECT_TRUE(Storage.exists(book.getThumbBmpPath(Xtc::SHARED_THUMB_HEIGHT).c_str()));
    EXPECT_TRUE(Storage.exists(book.getThumbBmpPath(456).c_str()));
  }
}

TEST_F(XtcContractTest, CancellingThumbnailPairPreparationRemovesOnlyStagingOutput) {
  Storage.setFile(BOOK_PATH, makeBook());
  Xtc book(BOOK_PATH, "/.crosspoint");
  ASSERT_TRUE(book.load());
  ASSERT_EQ(book.beginThumbnailPreparation(273, 456), Xtc::ThumbnailPreparationStatus::InProgress);

  const std::string sharedPath = book.getThumbBmpPath(Xtc::SHARED_THUMB_HEIGHT);
  const std::string carouselPath = book.getThumbBmpPath(456);
  size_t steps = 0;
  while (!Storage.exists((sharedPath + ".tmp").c_str()) && steps < 1000) {
    ASSERT_EQ(book.stepThumbnailPreparation(1024, 8), Xtc::ThumbnailPreparationStatus::InProgress);
    ++steps;
  }
  ASSERT_TRUE(Storage.exists((sharedPath + ".tmp").c_str()));
  const size_t stagingBytesBeforeStep = Storage.file(sharedPath + ".tmp").size();
  ASSERT_EQ(book.stepThumbnailPreparation(1024, 3), Xtc::ThumbnailPreparationStatus::InProgress);
  EXPECT_LE(Storage.file(sharedPath + ".tmp").size() - stagingBytesBeforeStep, 3U * 20U);

  book.cancelThumbnailPreparation();
  EXPECT_FALSE(book.thumbnailPreparationActive());
  EXPECT_FALSE(Storage.exists(sharedPath.c_str()));
  EXPECT_FALSE(Storage.exists((sharedPath + ".tmp").c_str()));
  EXPECT_FALSE(Storage.exists(carouselPath.c_str()));
  EXPECT_FALSE(Storage.exists((carouselPath + ".tmp").c_str()));
}

TEST_F(XtcContractTest, ThumbnailPairPreparationRejectsSourceReplacementBeforePublishing) {
  auto source = makeBook();
  Storage.setFile(BOOK_PATH, source);
  Xtc book(BOOK_PATH, "/.crosspoint");
  ASSERT_TRUE(book.load());
  ASSERT_EQ(book.beginThumbnailPreparation(273, 456), Xtc::ThumbnailPreparationStatus::InProgress);

  const std::string carouselStagingPath = book.getThumbBmpPath(456) + ".tmp";
  size_t steps = 0;
  while (!Storage.exists(carouselStagingPath.c_str()) && steps < 1000) {
    ASSERT_EQ(book.stepThumbnailPreparation(1024, 8), Xtc::ThumbnailPreparationStatus::InProgress);
    ++steps;
  }
  ASSERT_TRUE(Storage.exists(carouselStagingPath.c_str()));

  source.back() ^= 0x01U;
  Storage.setFile(BOOK_PATH, std::move(source));
  Xtc::ThumbnailPreparationStatus status = Xtc::ThumbnailPreparationStatus::InProgress;
  steps = 0;
  while (status == Xtc::ThumbnailPreparationStatus::InProgress && steps < 1000) {
    status = book.stepThumbnailPreparation(1024, 8);
    ++steps;
  }

  EXPECT_EQ(status, Xtc::ThumbnailPreparationStatus::Error);
  EXPECT_FALSE(Storage.exists(book.getThumbBmpPath(Xtc::SHARED_THUMB_HEIGHT).c_str()));
  EXPECT_FALSE(Storage.exists(book.getThumbBmpPath(456).c_str()));
  EXPECT_FALSE(Storage.exists((book.getThumbBmpPath(Xtc::SHARED_THUMB_HEIGHT) + ".tmp").c_str()));
  EXPECT_FALSE(Storage.exists((book.getThumbBmpPath(456) + ".tmp").c_str()));
}

TEST_F(XtcContractTest, PairedThumbnailsPreserveExistingSiblingWhenCarouselPublishFails) {
  Storage.setFile(BOOK_PATH, makeBook());
  Xtc book(BOOK_PATH, "/.crosspoint");
  ASSERT_TRUE(book.load());
  ASSERT_EQ(finishThumbnailPreparation(book), Xtc::ThumbnailPreparationStatus::Ready);
  const std::string sharedPath = book.getThumbBmpPath(Xtc::SHARED_THUMB_HEIGHT);
  const std::vector<uint8_t> sharedBefore = Storage.file(sharedPath);
  const std::string carouselPath = book.getThumbBmpPath(456);
  ASSERT_TRUE(Storage.remove(carouselPath.c_str()));
  Storage.failRenameTo(carouselPath);

  Xtc retry(BOOK_PATH, "/.crosspoint");
  ASSERT_TRUE(retry.load());
  EXPECT_EQ(finishThumbnailPreparation(retry), Xtc::ThumbnailPreparationStatus::Error);
  EXPECT_EQ(Storage.file(sharedPath), sharedBefore);
  EXPECT_FALSE(Storage.exists(carouselPath.c_str()));
  EXPECT_FALSE(Storage.exists((carouselPath + ".tmp").c_str()));
}

TEST_F(XtcContractTest, ThumbnailPublishFaultsLeaveNoCommittedOrTemporaryOutput) {
  enum class Fault { ShortWrite, Sync, Close, Rename };
  for (const Fault fault : {Fault::ShortWrite, Fault::Sync, Fault::Close, Fault::Rename}) {
    SCOPED_TRACE(static_cast<int>(fault));
    Storage.reset();
    Storage.setFile(BOOK_PATH, makeBook());
    Xtc book(BOOK_PATH, "/.crosspoint");
    ASSERT_TRUE(book.load());
    const std::string finalPath = book.getThumbBmpPath(Xtc::SHARED_THUMB_HEIGHT);
    const std::string stagingPath = finalPath + ".tmp";
    switch (fault) {
      case Fault::ShortWrite:
        Storage.shortWriteFor(stagingPath);
        break;
      case Fault::Sync:
        Storage.failSyncOnce();
        break;
      case Fault::Close:
        Storage.failCloseFor(stagingPath);
        break;
      case Fault::Rename:
        Storage.failRenameTo(finalPath);
        break;
    }

    EXPECT_EQ(finishThumbnailPreparation(book), Xtc::ThumbnailPreparationStatus::Error);
    EXPECT_FALSE(Storage.exists(finalPath.c_str()));
    EXPECT_FALSE(Storage.exists(stagingPath.c_str()));
    EXPECT_EQ(Storage.invalidOperationCount(), 0U);
  }
}

TEST_F(XtcContractTest, RejectsUnsupportedDimensionsAndVersion) {
  EXPECT_EQ(openBook(makeBook(1, 480, 799)), xtc::XtcError::UNSUPPORTED_DIMENSIONS);
  auto zeroWidth = makeBook();
  writeU16(zeroWidth, tableOffset(zeroWidth) + 12, 0);
  EXPECT_EQ(openBook(std::move(zeroWidth)), xtc::XtcError::UNSUPPORTED_DIMENSIONS);
  auto swappedVersion = makeBook();
  swappedVersion[4] = 0;
  swappedVersion[5] = 1;
  EXPECT_EQ(openBook(std::move(swappedVersion)), xtc::XtcError::INVALID_VERSION);

  auto extremeHeaderDimensions = makeBook();
  writeU16(extremeHeaderDimensions, pageOffset(extremeHeaderDimensions) + 4, std::numeric_limits<uint16_t>::max());
  xtc::XtcParser extremeParser;
  ASSERT_EQ(openBook(std::move(extremeHeaderDimensions), &extremeParser), xtc::XtcError::OK);
  xtc::PageInfo extremeInfo;
  EXPECT_FALSE(extremeParser.getPageInfo(0, extremeInfo));
  EXPECT_EQ(extremeParser.getLastError(), xtc::XtcError::SIZE_MISMATCH);
}

TEST_F(XtcContractTest, RejectsInvalidHeaderCountsFlagsAndTableRange) {
  auto zeroPages = makeBook();
  writeU16(zeroPages, 6, 0);
  EXPECT_EQ(openBook(std::move(zeroPages)), xtc::XtcError::CORRUPTED_HEADER);

  auto impossibleCurrentPage = makeBook();
  writeU32(impossibleCurrentPage, 12, 2);
  EXPECT_EQ(openBook(std::move(impossibleCurrentPage)), xtc::XtcError::CORRUPTED_HEADER);

  auto invalidFlag = makeBook();
  invalidFlag[9] = 2;
  EXPECT_EQ(openBook(std::move(invalidFlag)), xtc::XtcError::CORRUPTED_HEADER);

  auto missingTableTail = makeBook();
  writeU16(missingTableTail, 6, 2);
  writeU64(missingTableTail, 24, missingTableTail.size() - sizeof(xtc::PageTableEntry));
  EXPECT_EQ(openBook(std::move(missingTableTail)), xtc::XtcError::OFFSET_OUT_OF_RANGE);

  auto wrappedTable = makeBook();
  writeU64(wrappedTable, 24, std::numeric_limits<uint64_t>::max() - 4U);
  EXPECT_EQ(openBook(std::move(wrappedTable)), xtc::XtcError::OFFSET_OUT_OF_RANGE);
}

TEST_F(XtcContractTest, RejectsWrongContainerAndPageMagic) {
  auto wrongContainer = makeBook();
  writeU32(wrongContainer, 0, 0x12345678U);
  EXPECT_EQ(openBook(std::move(wrongContainer)), xtc::XtcError::INVALID_MAGIC);

  auto mismatchedPage = makeBook(1);
  writeU32(mismatchedPage, pageOffset(mismatchedPage), xtc::XTH_MAGIC);
  xtc::XtcParser mismatchedParser;
  ASSERT_EQ(openBook(std::move(mismatchedPage), &mismatchedParser), xtc::XtcError::OK);
  xtc::PageInfo info;
  EXPECT_FALSE(mismatchedParser.getPageInfo(0, info));
  EXPECT_EQ(mismatchedParser.getLastError(), xtc::XtcError::INVALID_MAGIC);
  auto mismatchedGrayPage = makeBook(2);
  writeU32(mismatchedGrayPage, pageOffset(mismatchedGrayPage), xtc::XTG_MAGIC);
  xtc::XtcParser mismatchedGrayParser;
  ASSERT_EQ(openBook(std::move(mismatchedGrayPage), &mismatchedGrayParser), xtc::XtcError::OK);
  EXPECT_FALSE(mismatchedGrayParser.getPageInfo(0, info));
  EXPECT_EQ(mismatchedGrayParser.getLastError(), xtc::XtcError::INVALID_MAGIC);
}

TEST_F(XtcContractTest, RejectsSizeOffsetCompressionAndTruncationDamage) {
  auto tableMismatch = makeBook();
  writeU32(tableMismatch, tableOffset(tableMismatch) + 8, 1234);
  EXPECT_EQ(openBook(std::move(tableMismatch)), xtc::XtcError::SIZE_MISMATCH);

  auto headerMismatch = makeBook();
  writeU32(headerMismatch, pageOffset(headerMismatch) + 10, 1234);
  xtc::XtcParser headerMismatchParser;
  ASSERT_EQ(openBook(std::move(headerMismatch), &headerMismatchParser), xtc::XtcError::OK);
  xtc::PageInfo info;
  EXPECT_FALSE(headerMismatchParser.getPageInfo(0, info));
  EXPECT_EQ(headerMismatchParser.getLastError(), xtc::XtcError::SIZE_MISMATCH);

  auto compressed = makeBook();
  compressed[pageOffset(compressed) + 9] = 1;
  xtc::XtcParser compressedParser;
  ASSERT_EQ(openBook(std::move(compressed), &compressedParser), xtc::XtcError::OK);
  EXPECT_FALSE(compressedParser.getPageInfo(0, info));
  EXPECT_EQ(compressedParser.getLastError(), xtc::XtcError::UNSUPPORTED_COMPRESSION);

  auto outside = makeBook();
  writeU64(outside, tableOffset(outside), std::numeric_limits<uint64_t>::max() - 4U);
  EXPECT_EQ(openBook(std::move(outside)), xtc::XtcError::OFFSET_OUT_OF_RANGE);

  auto truncated = makeBook();
  truncated.pop_back();
  EXPECT_EQ(openBook(std::move(truncated)), xtc::XtcError::OFFSET_OUT_OF_RANGE);
}

TEST_F(XtcContractTest, RejectsMalformedMetadataAndChapters) {
  auto noTitleTerminator = makeBook();
  std::fill(noTitleTerminator.begin() + xtc::XTC_HEADER_SIZE, noTitleTerminator.begin() + xtc::XTC_HEADER_SIZE + 128,
            'A');
  EXPECT_EQ(openBook(std::move(noTitleTerminator)), xtc::XtcError::INVALID_METADATA);

  auto noAuthorTerminator = makeBook();
  std::fill(noAuthorTerminator.begin() + xtc::XTC_HEADER_SIZE + 128,
            noAuthorTerminator.begin() + xtc::XTC_HEADER_SIZE + 192, 'A');
  EXPECT_EQ(openBook(std::move(noAuthorTerminator)), xtc::XtcError::INVALID_METADATA);

  auto truncatedMetadata = makeBook();
  truncatedMetadata.resize(xtc::XTC_HEADER_SIZE + xtc::XTC_METADATA_SIZE - 1U);
  writeU64(truncatedMetadata, 24, truncatedMetadata.size());
  writeU64(truncatedMetadata, 32, truncatedMetadata.size());
  EXPECT_EQ(openBook(std::move(truncatedMetadata)), xtc::XtcError::INVALID_METADATA);

  auto badChapterOffset = makeBook(1, 480, 800, true);
  writeU64(badChapterOffset, 48, std::numeric_limits<uint64_t>::max());
  EXPECT_EQ(openBook(std::move(badChapterOffset)), xtc::XtcError::INVALID_CHAPTERS);

  auto tooManyChapters = makeBook(1, 480, 800, true);
  writeU16(tooManyChapters, xtc::XTC_HEADER_SIZE + 196, xtc::XTC_MAX_CHAPTERS + 1U);
  EXPECT_EQ(openBook(std::move(tooManyChapters)), xtc::XtcError::INVALID_CHAPTERS);

  auto pageOutside = makeBook(1, 480, 800, true);
  writeU16(pageOutside, xtc::XTC_HEADER_SIZE + xtc::XTC_METADATA_SIZE + 0x50, 2);
  writeU16(pageOutside, xtc::XTC_HEADER_SIZE + xtc::XTC_METADATA_SIZE + 0x52, 2);
  EXPECT_EQ(openBook(std::move(pageOutside)), xtc::XtcError::INVALID_CHAPTERS);

  auto invertedRange = makeBook(1, 480, 800, true);
  writeU16(invertedRange, xtc::XTC_HEADER_SIZE + xtc::XTC_METADATA_SIZE + 0x50, 1);
  writeU16(invertedRange, xtc::XTC_HEADER_SIZE + xtc::XTC_METADATA_SIZE + 0x52, 0);
  EXPECT_EQ(openBook(std::move(invertedRange)), xtc::XtcError::INVALID_CHAPTERS);

  auto unterminatedChapter = makeBook(1, 480, 800, true);
  std::fill(unterminatedChapter.begin() + xtc::XTC_HEADER_SIZE + xtc::XTC_METADATA_SIZE,
            unterminatedChapter.begin() + xtc::XTC_HEADER_SIZE + xtc::XTC_METADATA_SIZE + 80, 'C');
  EXPECT_EQ(openBook(std::move(unterminatedChapter)), xtc::XtcError::INVALID_CHAPTERS);

  auto chapterPointsIntoPayload = makeBook(1, 480, 800, true);
  writeU64(chapterPointsIntoPayload, 48, pageOffset(chapterPointsIntoPayload) + sizeof(xtc::XtgPageHeader));
  EXPECT_EQ(openBook(std::move(chapterPointsIntoPayload)), xtc::XtcError::INVALID_CHAPTERS);
}

TEST_F(XtcContractTest, FailedOpenClearsPreviouslyLoadedState) {
  xtc::XtcParser parser;
  ASSERT_EQ(openBook(makeBook(1, 480, 800, true), &parser), xtc::XtcError::OK);
  ASSERT_TRUE(parser.isOpen());
  ASSERT_TRUE(parser.hasChapters());
  ASSERT_FALSE(parser.getTitle().empty());

  auto broken = makeBook();
  writeU32(broken, 0, 0);
  EXPECT_EQ(openBook(std::move(broken), &parser), xtc::XtcError::INVALID_MAGIC);
  EXPECT_FALSE(parser.isOpen());
  EXPECT_FALSE(parser.hasChapters());
  EXPECT_TRUE(parser.getTitle().empty());
  ZipFile::SourceIdentity identity;
  EXPECT_FALSE(parser.getSourceIdentity(identity));
}

TEST_F(XtcContractTest, PageLoadingRequiresExactBoundsAndKeepsStreamingChunksBounded) {
  xtc::XtcParser parser;
  ASSERT_EQ(openBook(makeBook(), &parser), xtc::XtcError::OK);
  xtc::PageLayout layout;
  ASSERT_TRUE(xtc::calculatePageLayout(480, 800, 1, layout));

  std::vector<uint8_t> tooSmall(layout.payloadBytes - 1U);
  EXPECT_EQ(parser.loadPage(0, tooSmall.data(), tooSmall.size()), 0U);
  EXPECT_EQ(parser.getLastError(), xtc::XtcError::MEMORY_ERROR);

  std::vector<uint8_t> exact(layout.payloadBytes);
  EXPECT_EQ(parser.loadPage(0, exact.data(), exact.size()), layout.payloadBytes);
  EXPECT_TRUE(std::all_of(exact.begin(), exact.end(), [](const uint8_t value) { return value == 0xFFU; }));

  size_t streamed = 0;
  size_t largestChunk = 0;
  EXPECT_EQ(parser.loadPageStreaming(
                0,
                [&](const uint8_t*, const size_t size, const size_t offset) {
                  EXPECT_EQ(offset, streamed);
                  streamed += size;
                  largestChunk = std::max(largestChunk, size);
                },
                std::numeric_limits<size_t>::max()),
            xtc::XtcError::OK);
  EXPECT_EQ(streamed, layout.payloadBytes);
  EXPECT_LE(largestChunk, 1024U);
  EXPECT_EQ(parser.loadPageStreaming(0, {}, 1024), xtc::XtcError::INVALID_ARGUMENT);
  EXPECT_EQ(parser.loadPageStreaming(1, [](const uint8_t*, size_t, size_t) {}, 1024), xtc::XtcError::PAGE_OUT_OF_RANGE);
}

TEST_F(XtcContractTest, ReadFailuresAndSourceMutationFailClosed) {
  Storage.setFile(BOOK_PATH, makeBook());
  Storage.shortReadFor(BOOK_PATH);
  xtc::XtcParser shortReadParser;
  EXPECT_EQ(finishOpen(shortReadParser), xtc::XtcError::READ_ERROR);
  EXPECT_FALSE(shortReadParser.isOpen());

  Storage.reset();
  Storage.setFile(BOOK_PATH, makeBook());
  // Header, metadata and the page-table block are the first three reads;
  // mutate the file as the streaming identity pass begins.
  Storage.growOnReadCall(4);
  xtc::XtcParser changingParser;
  EXPECT_EQ(finishOpen(changingParser), xtc::XtcError::READ_ERROR);
  EXPECT_FALSE(changingParser.isOpen());
}

TEST_F(XtcContractTest, XthPlaneOrderPreservesAllFourConverterLevels) {
  xtc::PageLayout layout;
  ASSERT_TRUE(xtc::calculatePageLayout(480, 800, 2, layout));
  std::vector<uint8_t> payload(layout.payloadBytes, 0);
  const auto setLevel = [&](const uint16_t x, const uint16_t y, const uint8_t level) {
    const size_t offset = static_cast<size_t>(480 - 1 - x) * layout.columnBytes + y / 8U;
    const uint8_t bit = static_cast<uint8_t>(7U - y % 8U);
    if ((level & 1U) != 0) payload[offset] |= static_cast<uint8_t>(1U << bit);
    if ((level & 2U) != 0) payload[layout.planeBytes + offset] |= static_cast<uint8_t>(1U << bit);
  };
  for (uint8_t level = 0; level < 4; ++level) setLevel(level, 0, level);
  for (uint8_t level = 0; level < 4; ++level) {
    EXPECT_EQ(xtc::readXthPixel(payload.data(), layout, 480, level, 0), level);
  }
}

TEST_F(XtcContractTest, NativeX4PlaneCompositionMatchesTheFourConverterLevels) {
  for (uint16_t first = 0; first <= 0xFFU; ++first) {
    for (uint16_t second = 0; second <= 0xFFU; ++second) {
      const uint8_t bit0 = static_cast<uint8_t>(first);
      const uint8_t bit1 = static_cast<uint8_t>(second);
      uint8_t base = 0;
      uint8_t lsb = 0;
      uint8_t msb = 0;
      xtc::composeNativeXthPlaneBytes(&bit0, &bit1, 1, &base, &lsb, &msb);
      EXPECT_EQ(base, static_cast<uint8_t>(~(bit0 | bit1)));
      EXPECT_EQ(lsb, static_cast<uint8_t>(bit0 & ~bit1));
      EXPECT_EQ(msb, static_cast<uint8_t>(bit0 ^ bit1));
    }
  }

  uint8_t aliasedBit0 = 0x5AU;
  uint8_t aliasedBit1 = 0x3CU;
  xtc::composeNativeXthPlaneBytes(&aliasedBit0, &aliasedBit1, 1, nullptr, &aliasedBit0, &aliasedBit1);
  EXPECT_EQ(aliasedBit0, static_cast<uint8_t>(0x5AU & ~0x3CU));
  EXPECT_EQ(aliasedBit1, static_cast<uint8_t>(0x5AU ^ 0x3CU));
}

TEST_F(XtcContractTest, StreamsMatchingXthPlaneChunksWithBoundedScratch) {
  auto bytes = makeBook(2);
  xtc::PageLayout layout;
  ASSERT_TRUE(xtc::calculatePageLayout(480, 800, 2, layout));
  const size_t payloadStart = pageOffset(bytes) + sizeof(xtc::XtgPageHeader);
  std::vector<uint8_t> expectedBit0(layout.planeBytes);
  std::vector<uint8_t> expectedBit1(layout.planeBytes);
  for (size_t index = 0; index < layout.planeBytes; ++index) {
    expectedBit0[index] = static_cast<uint8_t>(index * 17U + 3U);
    expectedBit1[index] = static_cast<uint8_t>(index * 29U + 11U);
  }
  std::copy(expectedBit0.begin(), expectedBit0.end(), bytes.begin() + static_cast<std::ptrdiff_t>(payloadStart));
  std::copy(expectedBit1.begin(), expectedBit1.end(),
            bytes.begin() + static_cast<std::ptrdiff_t>(payloadStart + layout.planeBytes));

  xtc::XtcParser parser;
  ASSERT_EQ(openBook(std::move(bytes), &parser), xtc::XtcError::OK);
  std::vector<uint8_t> streamedBit0;
  std::vector<uint8_t> streamedBit1;
  size_t expectedOffset = 0;
  size_t largestChunk = 0;
  EXPECT_EQ(parser.loadXthPlanePairs(
                0,
                [&](uint8_t* bit0, uint8_t* bit1, const size_t size, const size_t offset) {
                  EXPECT_EQ(offset, expectedOffset);
                  EXPECT_EQ(size % layout.columnBytes, 0U);
                  expectedOffset += size;
                  largestChunk = std::max(largestChunk, size);
                  streamedBit0.insert(streamedBit0.end(), bit0, bit0 + size);
                  streamedBit1.insert(streamedBit1.end(), bit1, bit1 + size);
                },
                217),
            xtc::XtcError::OK);
  EXPECT_EQ(streamedBit0, expectedBit0);
  EXPECT_EQ(streamedBit1, expectedBit1);
  EXPECT_EQ(expectedOffset, layout.planeBytes);
  EXPECT_LE(largestChunk, 217U);

  EXPECT_EQ(parser.loadXthPlanePairs(0, {}, 1024), xtc::XtcError::INVALID_ARGUMENT);
  xtc::XtcParser oneBitParser;
  ASSERT_EQ(openBook(makeBook(1), &oneBitParser), xtc::XtcError::OK);
  EXPECT_EQ(oneBitParser.loadXthPlanePairs(0, [](uint8_t*, uint8_t*, size_t, size_t) {}),
            xtc::XtcError::INVALID_ARGUMENT);
}

TEST_F(XtcContractTest, X3FitsAndX4MapsEveryEdge) {
  xtc::Viewport x4;
  ASSERT_TRUE(xtc::calculateFitViewport(480, 800, 480, 800, x4));
  EXPECT_EQ(x4.x, 0);
  EXPECT_EQ(x4.y, 0);
  EXPECT_EQ(x4.width, 480);
  EXPECT_EQ(x4.height, 800);

  xtc::Viewport x3;
  ASSERT_TRUE(xtc::calculateFitViewport(480, 800, 528, 792, x3));
  EXPECT_EQ(x3.x, 26);
  EXPECT_EQ(x3.y, 0);
  EXPECT_EQ(x3.width, 475);
  EXPECT_EQ(x3.height, 792);
  EXPECT_EQ(xtc::mapViewportCoordinate(0, x3.width, 480), 0);
  EXPECT_EQ(xtc::mapViewportCoordinate(x3.width - 1, x3.width, 480), 479);
  EXPECT_EQ(xtc::mapViewportCoordinate(0, x3.height, 800), 0);
  EXPECT_EQ(xtc::mapViewportCoordinate(x3.height - 1, x3.height, 800), 799);
}

TEST_F(XtcContractTest, NativeX4RotationPreservesEveryPackedPixelAcrossStreamedStrips) {
  constexpr uint16_t width = 16;
  constexpr uint16_t height = 16;
  xtc::PageLayout layout;
  ASSERT_TRUE(xtc::calculatePageLayout(width, height, 1, layout));
  std::vector<uint8_t> source(layout.payloadBytes);
  for (size_t index = 0; index < source.size(); ++index) {
    source[index] = static_cast<uint8_t>(0x31U + index * 37U);
  }
  std::vector<uint8_t> target(layout.payloadBytes, 0xA5U);
  const size_t stripBytes = layout.rowBytes * 8U;
  ASSERT_TRUE(xtc::rotateXtgPortraitRowsToNativeLandscape(source.data(), stripBytes, 0, width, height, target.data(),
                                                          target.size()));
  ASSERT_TRUE(xtc::rotateXtgPortraitRowsToNativeLandscape(source.data() + stripBytes, stripBytes, stripBytes, width,
                                                          height, target.data(), target.size()));

  const size_t targetRowBytes = height / 8U;
  for (uint16_t sourceY = 0; sourceY < height; ++sourceY) {
    for (uint16_t sourceX = 0; sourceX < width; ++sourceX) {
      const bool sourceWhite = ((source[sourceY * layout.rowBytes + sourceX / 8U] >> (7U - sourceX % 8U)) & 1U) != 0;
      const uint16_t targetX = sourceY;
      const uint16_t targetY = width - 1U - sourceX;
      const bool targetWhite = ((target[targetY * targetRowBytes + targetX / 8U] >> (7U - targetX % 8U)) & 1U) != 0;
      EXPECT_EQ(targetWhite, sourceWhite) << "source pixel " << sourceX << "," << sourceY;
    }
  }
}

TEST_F(XtcContractTest, NativeX4RotationRejectsPartialOrMisalignedRows) {
  std::array<uint8_t, 16> source{};
  std::array<uint8_t, 32> target{};
  EXPECT_FALSE(xtc::rotateXtgPortraitRowsToNativeLandscape(source.data(), 1, 0, 16, 16, target.data(), target.size()));
  EXPECT_FALSE(xtc::rotateXtgPortraitRowsToNativeLandscape(source.data(), source.size(), 1, 16, 16, target.data(),
                                                           target.size()));
}

TEST_F(XtcContractTest, StreamCoordinatesPreservePlaneOrderAndViewportSampling) {
  xtc::PageLayout layout;
  ASSERT_TRUE(xtc::calculatePageLayout(480, 799, 2, layout));

  bool secondPlane = false;
  uint16_t x = 0;
  uint16_t yBase = 0;
  ASSERT_TRUE(xtc::locateXthStreamByte(layout, 480, 799, 0, secondPlane, x, yBase));
  EXPECT_FALSE(secondPlane);
  EXPECT_EQ(x, 479);
  EXPECT_EQ(yBase, 0);
  ASSERT_TRUE(xtc::locateXthStreamByte(layout, 480, 799, layout.planeBytes, secondPlane, x, yBase));
  EXPECT_TRUE(secondPlane);
  EXPECT_EQ(x, 479);
  EXPECT_EQ(yBase, 0);
  EXPECT_FALSE(xtc::locateXthStreamByte(layout, 480, 799, layout.payloadBytes, secondPlane, x, yBase));

  for (uint16_t destination = 0; destination < 475; ++destination) {
    const uint16_t source = xtc::mapViewportCoordinate(destination, 475, 480);
    const xtc::CoordinateRange range = xtc::mapSourceCoordinateRange(source, 480, 475);
    EXPECT_LE(range.begin, destination);
    EXPECT_GT(range.end, destination);
  }
  size_t covered = 0;
  for (uint16_t source = 0; source < 480; ++source) {
    const xtc::CoordinateRange range = xtc::mapSourceCoordinateRange(source, 480, 475);
    covered += range.end - range.begin;
  }
  EXPECT_EQ(covered, 475U);

  for (uint16_t destination = 0; destination < 792; ++destination) {
    const uint16_t source = xtc::mapViewportCoordinate(destination, 792, 800);
    const xtc::CoordinateRange range = xtc::mapSourceCoordinateRange(source, 800, 792);
    EXPECT_LE(range.begin, destination);
    EXPECT_GT(range.end, destination);
  }
  covered = 0;
  for (uint16_t source = 0; source < 800; ++source) {
    const xtc::CoordinateRange range = xtc::mapSourceCoordinateRange(source, 800, 792);
    covered += range.end - range.begin;
  }
  EXPECT_EQ(covered, 792U);
}

TEST_F(XtcContractTest, ScaledPortraitPlanePairsMatchReferenceWithoutAFullFrameScratch) {
  constexpr uint16_t sourceWidth = 16;
  constexpr uint16_t sourceHeight = 25;
  constexpr uint16_t screenWidth = 18;
  constexpr uint16_t screenHeight = 24;
  constexpr uint16_t panelWidth = screenHeight;
  constexpr uint16_t panelHeight = screenWidth;
  constexpr size_t panelRowBytes = panelWidth / 8U;

  xtc::PageLayout layout;
  ASSERT_TRUE(xtc::calculatePageLayout(sourceWidth, sourceHeight, 2, layout));
  xtc::Viewport viewport;
  ASSERT_TRUE(xtc::calculateFitViewport(sourceWidth, sourceHeight, screenWidth, screenHeight, viewport));
  ASSERT_EQ(viewport.x, 1);
  ASSERT_EQ(viewport.y, 0);
  ASSERT_EQ(viewport.width, 15);
  ASSERT_EQ(viewport.height, 24);

  std::vector<uint8_t> bit0(layout.planeBytes, 0);
  std::vector<uint8_t> bit1(layout.planeBytes, 0);
  for (uint16_t x = 0; x < sourceWidth; ++x) {
    for (uint16_t y = 0; y < sourceHeight; ++y) {
      const uint8_t level = static_cast<uint8_t>((x * 3U + y * 5U) & 3U);
      const size_t column = sourceWidth - 1U - x;
      const size_t offset = column * layout.columnBytes + y / 8U;
      const uint8_t mask = static_cast<uint8_t>(1U << (7U - y % 8U));
      if ((level & 1U) != 0) bit0[offset] |= mask;
      if ((level & 2U) != 0) bit1[offset] |= mask;
    }
  }

  std::vector<uint8_t> base(static_cast<size_t>(panelHeight) * panelRowBytes, 0xFFU);
  std::vector<uint8_t> lsb(static_cast<size_t>(panelHeight) * panelRowBytes, 0);
  std::vector<uint8_t> msb(static_cast<size_t>(panelHeight) * panelRowBytes, 0);
  constexpr size_t columnsPerChunk = 3;
  std::array<uint8_t, columnsPerChunk * panelRowBytes> firstScratch{};
  std::array<uint8_t, columnsPerChunk * panelRowBytes> secondScratch{};
  size_t mappedRows = 0;
  uint16_t nextPhysicalRow = static_cast<uint16_t>(panelHeight - viewport.x - viewport.width);

  for (size_t planeOffset = 0; planeOffset < layout.planeBytes;) {
    const size_t size = std::min(columnsPerChunk * layout.columnBytes, layout.planeBytes - planeOffset);
    xtc::XthPortraitRows rows;
    ASSERT_TRUE(xtc::composeScaledXthPortraitRows(
        bit0.data() + planeOffset, bit1.data() + planeOffset, size, planeOffset, layout, sourceWidth, sourceHeight,
        viewport, panelWidth, panelHeight, false, firstScratch.data(), nullptr, nullptr, firstScratch.size(), rows));
    if (rows.count > 0) {
      EXPECT_EQ(rows.yStart, nextPhysicalRow);
      std::memcpy(base.data() + static_cast<size_t>(rows.yStart) * panelRowBytes, firstScratch.data(),
                  static_cast<size_t>(rows.count) * panelRowBytes);
      nextPhysicalRow = static_cast<uint16_t>(rows.yStart + rows.count);
      mappedRows += rows.count;
    }

    ASSERT_TRUE(xtc::composeScaledXthPortraitRows(bit0.data() + planeOffset, bit1.data() + planeOffset, size,
                                                  planeOffset, layout, sourceWidth, sourceHeight, viewport, panelWidth,
                                                  panelHeight, false, nullptr, firstScratch.data(),
                                                  secondScratch.data(), firstScratch.size(), rows));
    if (rows.count > 0) {
      std::memcpy(lsb.data() + static_cast<size_t>(rows.yStart) * panelRowBytes, firstScratch.data(),
                  static_cast<size_t>(rows.count) * panelRowBytes);
      std::memcpy(msb.data() + static_cast<size_t>(rows.yStart) * panelRowBytes, secondScratch.data(),
                  static_cast<size_t>(rows.count) * panelRowBytes);
    }
    planeOffset += size;
  }
  EXPECT_EQ(mappedRows, viewport.width);

  const auto packedBit = [panelRowBytes](const std::vector<uint8_t>& plane, const uint16_t x, const uint16_t y) {
    return (plane[static_cast<size_t>(y) * panelRowBytes + x / 8U] >> (7U - x % 8U)) & 1U;
  };
  for (uint16_t logicalX = 0; logicalX < screenWidth; ++logicalX) {
    for (uint16_t logicalY = 0; logicalY < screenHeight; ++logicalY) {
      uint8_t level = 0;
      if (logicalX >= viewport.x && logicalX < viewport.x + viewport.width && logicalY >= viewport.y &&
          logicalY < viewport.y + viewport.height) {
        const uint16_t sourceX =
            xtc::mapViewportCoordinate(static_cast<uint16_t>(logicalX - viewport.x), viewport.width, sourceWidth);
        const uint16_t sourceY =
            xtc::mapViewportCoordinate(static_cast<uint16_t>(logicalY - viewport.y), viewport.height, sourceHeight);
        level = static_cast<uint8_t>((sourceX * 3U + sourceY * 5U) & 3U);
      }
      const uint16_t physicalX = logicalY;
      const uint16_t physicalY = static_cast<uint16_t>(panelHeight - 1U - logicalX);
      EXPECT_EQ(packedBit(base, physicalX, physicalY), level == 0 ? 1U : 0U);
      EXPECT_EQ(packedBit(lsb, physicalX, physicalY), level == 1 ? 1U : 0U);
      EXPECT_EQ(packedBit(msb, physicalX, physicalY), level == 1 || level == 2 ? 1U : 0U);
    }
  }

  xtc::XthPortraitRows rows;
  EXPECT_FALSE(xtc::composeScaledXthPortraitRows(bit0.data(), bit1.data(), layout.columnBytes - 1U, 0, layout,
                                                 sourceWidth, sourceHeight, viewport, panelWidth, panelHeight, false,
                                                 firstScratch.data(), nullptr, nullptr, firstScratch.size(), rows));
}

TEST_F(XtcContractTest, InvertedPortraitPlanePairsMatchReference) {
  constexpr uint16_t sourceWidth = 8;
  constexpr uint16_t sourceHeight = 16;
  constexpr uint16_t panelWidth = 16;
  constexpr uint16_t panelHeight = 8;
  constexpr size_t panelRowBytes = panelWidth / 8U;

  xtc::PageLayout layout;
  ASSERT_TRUE(xtc::calculatePageLayout(sourceWidth, sourceHeight, 2, layout));
  const xtc::Viewport viewport{0, 0, sourceWidth, sourceHeight};
  std::vector<uint8_t> bit0(layout.planeBytes, 0);
  std::vector<uint8_t> bit1(layout.planeBytes, 0);
  for (uint16_t x = 0; x < sourceWidth; ++x) {
    for (uint16_t y = 0; y < sourceHeight; ++y) {
      const uint8_t level = static_cast<uint8_t>((x + y) & 3U);
      const size_t offset = static_cast<size_t>(sourceWidth - 1U - x) * layout.columnBytes + y / 8U;
      const uint8_t mask = static_cast<uint8_t>(1U << (7U - y % 8U));
      if ((level & 1U) != 0) bit0[offset] |= mask;
      if ((level & 2U) != 0) bit1[offset] |= mask;
    }
  }

  std::array<uint8_t, panelHeight * panelRowBytes> base{};
  std::array<uint8_t, panelHeight * panelRowBytes> lsb{};
  std::array<uint8_t, panelHeight * panelRowBytes> msb{};
  xtc::XthPortraitRows rows;
  ASSERT_TRUE(xtc::composeScaledXthPortraitRows(bit0.data(), bit1.data(), layout.planeBytes, 0, layout, sourceWidth,
                                                sourceHeight, viewport, panelWidth, panelHeight, true, base.data(),
                                                lsb.data(), msb.data(), base.size(), rows));
  EXPECT_EQ(rows.yStart, 0);
  EXPECT_EQ(rows.count, panelHeight);

  const auto packedBit = [](const auto& plane, const uint16_t x, const uint16_t y) {
    return (plane[static_cast<size_t>(y) * panelRowBytes + x / 8U] >> (7U - x % 8U)) & 1U;
  };
  for (uint16_t logicalX = 0; logicalX < sourceWidth; ++logicalX) {
    for (uint16_t logicalY = 0; logicalY < sourceHeight; ++logicalY) {
      const uint8_t level = static_cast<uint8_t>((logicalX + logicalY) & 3U);
      const uint16_t physicalX = static_cast<uint16_t>(panelWidth - 1U - logicalY);
      const uint16_t physicalY = logicalX;
      EXPECT_EQ(packedBit(base, physicalX, physicalY), level == 0 ? 1U : 0U);
      EXPECT_EQ(packedBit(lsb, physicalX, physicalY), level == 1 ? 1U : 0U);
      EXPECT_EQ(packedBit(msb, physicalX, physicalY), level == 1 || level == 2 ? 1U : 0U);
    }
  }
}

TEST_F(XtcContractTest, X3ScaledPlanePairChunksCoverOnlyTheFittedRows) {
  constexpr uint16_t sourceWidth = 480;
  constexpr uint16_t sourceHeight = 800;
  constexpr uint16_t screenWidth = 528;
  constexpr uint16_t screenHeight = 792;
  constexpr uint16_t panelWidth = screenHeight;
  constexpr uint16_t panelHeight = screenWidth;
  constexpr size_t chunkBytes = 1000;

  xtc::PageLayout layout;
  ASSERT_TRUE(xtc::calculatePageLayout(sourceWidth, sourceHeight, 2, layout));
  ASSERT_EQ(layout.columnBytes, 100U);
  xtc::Viewport viewport;
  ASSERT_TRUE(xtc::calculateFitViewport(sourceWidth, sourceHeight, screenWidth, screenHeight, viewport));
  ASSERT_EQ(viewport.x, 26);
  ASSERT_EQ(viewport.y, 0);
  ASSERT_EQ(viewport.width, 475);
  ASSERT_EQ(viewport.height, 792);

  constexpr size_t panelRowBytes = panelWidth / 8U;
  constexpr size_t rowsPerChunk = chunkBytes / (sourceHeight / 8U);
  std::array<uint8_t, chunkBytes> bit0{};
  std::array<uint8_t, chunkBytes> bit1{};
  std::array<uint8_t, rowsPerChunk * panelRowBytes> scratch{};
  size_t coveredRows = 0;
  uint16_t nextPhysicalRow = static_cast<uint16_t>(panelHeight - viewport.x - viewport.width);

  for (size_t planeOffset = 0; planeOffset < layout.planeBytes; planeOffset += chunkBytes) {
    const size_t size = std::min(chunkBytes, layout.planeBytes - planeOffset);
    xtc::XthPortraitRows rows;
    ASSERT_TRUE(xtc::composeScaledXthPortraitRows(bit0.data(), bit1.data(), size, planeOffset, layout, sourceWidth,
                                                  sourceHeight, viewport, panelWidth, panelHeight, false,
                                                  scratch.data(), nullptr, nullptr, scratch.size(), rows));
    EXPECT_EQ(rows.yStart, nextPhysicalRow);
    EXPECT_LE(rows.count, rowsPerChunk);
    nextPhysicalRow = static_cast<uint16_t>(rows.yStart + rows.count);
    coveredRows += rows.count;
  }

  EXPECT_EQ(coveredRows, viewport.width);
  EXPECT_EQ(nextPhysicalRow, panelHeight - viewport.x);
  EXPECT_EQ(scratch.size() * 2U, 1980U);
}

TEST_F(XtcContractTest, SameSizePixelReplacementChangesStreamingIdentity) {
  auto first = makeBook();
  auto second = first;
  second.back() ^= 0x01U;
  xtc::XtcParser firstParser;
  ASSERT_EQ(openBook(std::move(first), &firstParser), xtc::XtcError::OK);
  ZipFile::SourceIdentity firstIdentity;
  ASSERT_TRUE(firstParser.getSourceIdentity(firstIdentity));
  xtc::XtcParser secondParser;
  ASSERT_EQ(openBook(std::move(second), &secondParser), xtc::XtcError::OK);
  ZipFile::SourceIdentity secondIdentity;
  ASSERT_TRUE(secondParser.getSourceIdentity(secondIdentity));
  EXPECT_EQ(firstIdentity.fileSize, secondIdentity.fileSize);
  EXPECT_NE(firstIdentity, secondIdentity);
  EXPECT_LE(Storage.maxRead(), 2048U);
}

TEST_F(XtcContractTest, OpensFrozenFixturesFromTheRecommendedConverter) {
  const auto xtcFixture = readFixture("crossvi-converter-480x800.xtc");
  const auto xtchFixture = readFixture("crossvi-converter-480x800.xtch");
  ASSERT_FALSE(xtcFixture.empty());
  ASSERT_FALSE(xtchFixture.empty());
  EXPECT_EQ(openBook(xtcFixture), xtc::XtcError::OK);
  EXPECT_EQ(openBook(xtchFixture), xtc::XtcError::OK);
}

}  // namespace
