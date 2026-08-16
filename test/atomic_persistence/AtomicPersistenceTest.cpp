#include <AtomicFile.h>
#include <ClockCalendar.h>
#include <FontStorageUtils.h>
#include <HalStorage.h>
#include <LegacyStateCodec.h>
#include <StagedFileTransaction.h>
#include <TiltLifecyclePolicy.h>
#include <gtest/gtest.h>
#include <util/ClockSyncPolicy.h>
#include <vocabulary/VocabularyReviewStore.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr char PATH[] = "/state.json";

std::vector<uint8_t> bytes(const std::string& value) { return {value.begin(), value.end()}; }

bool objectValidator(const uint8_t* data, const size_t size, void*) {
  return size >= 2 && data[0] == '{' && data[size - 1] == '}';
}

AtomicFile::SaveStatus save(const std::string& value, const size_t maxSize = 64) {
  return AtomicFile::save(PATH, reinterpret_cast<const uint8_t*>(value.data()), value.size(), maxSize, objectValidator);
}

AtomicFile::LoadStatus load(std::string& value, const size_t maxSize = 64) {
  return AtomicFile::load(PATH, value, maxSize, objectValidator);
}

void expectRecoverable(const std::string& oldValue, const std::string& newValue) {
  std::string recovered;
  const auto status = load(recovered);
  EXPECT_TRUE(status == AtomicFile::LoadStatus::Primary || status == AtomicFile::LoadStatus::Backup ||
              status == AtomicFile::LoadStatus::Temp);
  EXPECT_TRUE(recovered == oldValue || recovered == newValue);
}

class AtomicPersistenceTest : public testing::Test {
 protected:
  void SetUp() override {
    Storage.reset();
    VOCABULARY_REVIEW.resetForTests();
  }
};

TEST_F(AtomicPersistenceTest, VocabularyReviewPersistsCompactWrongWordSet) {
  EXPECT_TRUE(VOCABULARY_REVIEW.load());
  EXPECT_EQ(VOCABULARY_REVIEW.count(), 0U);
  EXPECT_TRUE(VOCABULARY_REVIEW.markForReview(7));
  EXPECT_TRUE(VOCABULARY_REVIEW.markForReview(2999));
  EXPECT_TRUE(VOCABULARY_REVIEW.flush());
  EXPECT_EQ(Storage.file("/.crosspoint/vocabulary_review_v1.bin").size(), 387U);

  VOCABULARY_REVIEW.resetForTests();
  EXPECT_TRUE(VOCABULARY_REVIEW.load());
  EXPECT_TRUE(VOCABULARY_REVIEW.needsReview(7));
  EXPECT_TRUE(VOCABULARY_REVIEW.needsReview(2999));
  EXPECT_FALSE(VOCABULARY_REVIEW.needsReview(3000));
  EXPECT_EQ(VOCABULARY_REVIEW.count(), 2U);

  EXPECT_TRUE(VOCABULARY_REVIEW.markMastered(7));
  EXPECT_TRUE(VOCABULARY_REVIEW.flush());
  VOCABULARY_REVIEW.resetForTests();
  EXPECT_TRUE(VOCABULARY_REVIEW.load());
  EXPECT_FALSE(VOCABULARY_REVIEW.needsReview(7));
  EXPECT_TRUE(VOCABULARY_REVIEW.needsReview(2999));
}

TEST_F(AtomicPersistenceTest, VocabularyReviewRecoversVerifiedBackupAfterCorruption) {
  ASSERT_TRUE(VOCABULARY_REVIEW.markForReview(11));
  ASSERT_TRUE(VOCABULARY_REVIEW.flush());
  ASSERT_TRUE(VOCABULARY_REVIEW.markForReview(22));
  ASSERT_TRUE(VOCABULARY_REVIEW.flush());
  Storage.mutableFile("/.crosspoint/vocabulary_review_v1.bin").back() ^= 0x80U;

  VOCABULARY_REVIEW.resetForTests();
  EXPECT_TRUE(VOCABULARY_REVIEW.load());
  EXPECT_TRUE(VOCABULARY_REVIEW.needsReview(11));
  EXPECT_FALSE(VOCABULARY_REVIEW.needsReview(22));
  EXPECT_TRUE(VOCABULARY_REVIEW.flush());
}

TEST_F(AtomicPersistenceTest, VocabularyReviewWriteFailureKeepsPreviousPublishedData) {
  ASSERT_TRUE(VOCABULARY_REVIEW.markForReview(33));
  ASSERT_TRUE(VOCABULARY_REVIEW.flush());
  ASSERT_TRUE(VOCABULARY_REVIEW.markForReview(44));
  Storage.shortWriteFor("/.crosspoint/vocabulary_review_v1.bin.tmp");
  EXPECT_FALSE(VOCABULARY_REVIEW.flush());

  VOCABULARY_REVIEW.resetForTests();
  EXPECT_TRUE(VOCABULARY_REVIEW.load());
  EXPECT_TRUE(VOCABULARY_REVIEW.needsReview(33));
  EXPECT_FALSE(VOCABULARY_REVIEW.needsReview(44));
}

TEST_F(AtomicPersistenceTest, PublishesAndRotatesVerifiedJson) {
  ASSERT_EQ(save("{old}"), AtomicFile::SaveStatus::Saved);
  ASSERT_EQ(save("{new}"), AtomicFile::SaveStatus::Saved);
  std::string loaded;
  EXPECT_EQ(load(loaded), AtomicFile::LoadStatus::Primary);
  EXPECT_EQ(loaded, "{new}");
  EXPECT_EQ(std::string(Storage.file(PATH).begin(), Storage.file(PATH).end()), "{new}");
  EXPECT_EQ(std::string(Storage.file("/state.json.bak").begin(), Storage.file("/state.json.bak").end()), "{old}");
}

TEST_F(AtomicPersistenceTest, RejectsPayloadOverByteLimitBeforeOpeningFile) {
  EXPECT_EQ(save("{123}", 4), AtomicFile::SaveStatus::Oversize);
  EXPECT_FALSE(Storage.exists(PATH));
  EXPECT_EQ(save("{}", 2), AtomicFile::SaveStatus::Saved);
}

TEST_F(AtomicPersistenceTest, OpenFailureRetainsPrimary) {
  Storage.setFile(PATH, bytes("{old}"));
  Storage.makeUnwritable("/state.json.tmp");
  EXPECT_EQ(save("{new}"), AtomicFile::SaveStatus::IoError);
  expectRecoverable("{old}", "{new}");
}

TEST_F(AtomicPersistenceTest, ShortWriteRetainsPrimary) {
  Storage.setFile(PATH, bytes("{old}"));
  Storage.shortWriteFor("/state.json.tmp");
  EXPECT_EQ(save("{new}"), AtomicFile::SaveStatus::IoError);
  expectRecoverable("{old}", "{new}");
}

TEST_F(AtomicPersistenceTest, SyncFailureRetainsPrimary) {
  Storage.setFile(PATH, bytes("{old}"));
  Storage.failSyncOnce();
  EXPECT_EQ(save("{new}"), AtomicFile::SaveStatus::IoError);
  expectRecoverable("{old}", "{new}");
}

TEST_F(AtomicPersistenceTest, CloseFailureRetainsPrimary) {
  Storage.setFile(PATH, bytes("{old}"));
  Storage.failCloseFor("/state.json.tmp");
  EXPECT_EQ(save("{new}"), AtomicFile::SaveStatus::IoError);
  expectRecoverable("{old}", "{new}");
}

TEST_F(AtomicPersistenceTest, BackupRenameFailureRetainsPrimary) {
  Storage.setFile(PATH, bytes("{old}"));
  Storage.failRenameOnce();
  EXPECT_EQ(save("{new}"), AtomicFile::SaveStatus::IoError);
  expectRecoverable("{old}", "{new}");
}

TEST_F(AtomicPersistenceTest, PublishRenameFailureLeavesVerifiedBackupOrTemp) {
  Storage.failRenameOnce();
  EXPECT_EQ(save("{new}"), AtomicFile::SaveStatus::IoError);
  expectRecoverable("{old}", "{new}");
}

TEST_F(AtomicPersistenceTest, PublishVerificationFailureLeavesVerifiedBackup) {
  Storage.corruptRenameOnce();
  EXPECT_EQ(save("{new}"), AtomicFile::SaveStatus::IoError);
  expectRecoverable("{old}", "{new}");
}

TEST_F(AtomicPersistenceTest, RecoversTempWhenItIsOnlyValidCandidate) {
  Storage.setFile("/state.json.tmp", bytes("{new}"));
  std::string loaded;
  EXPECT_EQ(load(loaded), AtomicFile::LoadStatus::Temp);
  EXPECT_EQ(loaded, "{new}");
}

TEST_F(AtomicPersistenceTest, RecoversBackupAfterMalformedPrimary) {
  Storage.setFile(PATH, bytes("bad"));
  Storage.setFile("/state.json.bak", bytes("{old}"));
  std::string loaded;
  EXPECT_EQ(load(loaded), AtomicFile::LoadStatus::Backup);
  EXPECT_EQ(loaded, "{old}");
}

TEST_F(AtomicPersistenceTest, PrefersGoodPrimaryOverMalformedTemp) {
  Storage.setFile(PATH, bytes("{old}"));
  Storage.setFile("/state.json.tmp", bytes("bad"));
  std::string loaded;
  EXPECT_EQ(load(loaded), AtomicFile::LoadStatus::Primary);
  EXPECT_EQ(loaded, "{old}");
}

template <typename T>
void append(std::vector<uint8_t>& output, const T& value) {
  const auto* raw = reinterpret_cast<const uint8_t*>(&value);
  output.insert(output.end(), raw, raw + sizeof(value));
}

std::vector<uint8_t> legacyState(const uint8_t version, const std::string& path = "/book.epub") {
  std::vector<uint8_t> output;
  append(output, version);
  const uint32_t length = path.size();
  append(output, length);
  output.insert(output.end(), path.begin(), path.end());
  if (version >= 2) append(output, static_cast<uint8_t>(7));
  if (version >= 3) append(output, static_cast<uint8_t>(9));
  if (version >= 4) append(output, true);
  return output;
}

LegacyStateCodec::DecodeStatus decodeLegacy(const std::vector<uint8_t>& encoded, LegacyStateCodec::State& state) {
  Storage.setFile("/state.bin", encoded);
  HalFile file;
  EXPECT_TRUE(Storage.openFileForRead("TEST", "/state.bin", file));
  return LegacyStateCodec::decode(file, state);
}

TEST_F(AtomicPersistenceTest, ValidLegacyStateRemainsCompatible) {
  LegacyStateCodec::State state;
  EXPECT_EQ(decodeLegacy(legacyState(4), state), LegacyStateCodec::DecodeStatus::Ok);
  EXPECT_EQ(state.openBookPath, "/book.epub");
  EXPECT_EQ(state.lastSleepImage, 7);
  EXPECT_EQ(state.readerActivityLoadCount, 9);
  EXPECT_TRUE(state.lastSleepFromReader);
}

TEST_F(AtomicPersistenceTest, EveryTruncationIsRejectedWithoutPublishingPartialState) {
  const auto valid = legacyState(4);
  for (size_t size = 0; size < valid.size(); ++size) {
    LegacyStateCodec::State state;
    state.openBookPath = "sentinel";
    const std::vector<uint8_t> truncated(valid.begin(), valid.begin() + size);
    EXPECT_NE(decodeLegacy(truncated, state), LegacyStateCodec::DecodeStatus::Ok) << size;
    EXPECT_EQ(state.openBookPath, "sentinel") << size;
  }
}

TEST_F(AtomicPersistenceTest, UntrustedLegacyLengthsAreRejectedBeforeAllocation) {
  std::vector<uint8_t> encoded{4};
  append(encoded, UINT32_MAX);
  LegacyStateCodec::State state;
  EXPECT_EQ(decodeLegacy(encoded, state), LegacyStateCodec::DecodeStatus::Invalid);

  encoded = {4};
  append(encoded, static_cast<uint32_t>(100));
  encoded.push_back('x');
  EXPECT_EQ(decodeLegacy(encoded, state), LegacyStateCodec::DecodeStatus::Invalid);
}

TEST_F(AtomicPersistenceTest, FutureLegacyVersionIsRejected) {
  LegacyStateCodec::State state;
  EXPECT_EQ(decodeLegacy(legacyState(5), state), LegacyStateCodec::DecodeStatus::FutureVersion);
}

TEST_F(AtomicPersistenceTest, LegacyShortReadIsReportedAsIoError) {
  Storage.setFile("/state.bin", legacyState(4));
  Storage.shortReadFor("/state.bin");
  HalFile file;
  ASSERT_TRUE(Storage.openFileForRead("TEST", "/state.bin", file));
  LegacyStateCodec::State state;
  EXPECT_EQ(LegacyStateCodec::decode(file, state), LegacyStateCodec::DecodeStatus::IoError);
}

bool fontValidator(const char* path, void*) {
  if (!Storage.exists(path)) return false;
  const auto& data = Storage.file(path);
  return data.size() >= 8 && memcmp(data.data(), "CPFONT\0\0", 8) == 0;
}

bool readableFontValidator(const char* path, void*) {
  HalFile file;
  uint8_t magic[8]{};
  if (!Storage.openFileForRead("TEST", path, file) || file.fileSize64() < sizeof(magic)) return false;
  const bool valid = file.read(magic, sizeof(magic)) == static_cast<int>(sizeof(magic)) &&
                     memcmp(magic, "CPFONT\0\0", sizeof(magic)) == 0;
  return file.close() && valid;
}

constexpr char FONT[] = "/fonts/Regular.cpfont";
constexpr char FONT_TEMP[] = "/fonts/Regular.cpfont.upload.tmp";
constexpr char FONT_BACKUP[] = "/fonts/Regular.cpfont.upload.bak";

std::vector<uint8_t> fontBytes(const uint8_t marker) {
  std::vector<uint8_t> output{'C', 'P', 'F', 'O', 'N', 'T', 0, 0};
  output.push_back(marker);
  return output;
}

StagedFileTransaction::Digest streamedDigest(const std::vector<uint8_t>& data) {
  StagedFileTransaction::Digest digest;
  StagedFileTransaction::updateDigest(digest, data.data(), data.size());
  return digest;
}

TEST_F(AtomicPersistenceTest, FontPublishKeepsOldUntilNewFileVerifies) {
  Storage.setFile(FONT, fontBytes(1));
  Storage.setFile(FONT_TEMP, fontBytes(2));
  EXPECT_EQ(StagedFileTransaction::publish(FONT, FONT_TEMP, FONT_BACKUP, fontValidator),
            StagedFileTransaction::Status::Published);
  EXPECT_EQ(Storage.file(FONT).back(), 2);
  EXPECT_FALSE(Storage.exists(FONT_BACKUP));
}

TEST_F(AtomicPersistenceTest, InvalidFontStagingNeverTouchesOldFont) {
  Storage.setFile(FONT, fontBytes(1));
  Storage.setFile(FONT_TEMP, bytes("invalid"));
  EXPECT_EQ(StagedFileTransaction::publish(FONT, FONT_TEMP, FONT_BACKUP, fontValidator),
            StagedFileTransaction::Status::InvalidStaging);
  EXPECT_EQ(Storage.file(FONT).back(), 1);
}

TEST_F(AtomicPersistenceTest, FontPublishRenameFailureRestoresOldFont) {
  Storage.setFile(FONT, fontBytes(1));
  Storage.setFile(FONT_TEMP, fontBytes(2));
  Storage.failRenameTo(FONT);
  EXPECT_EQ(StagedFileTransaction::publish(FONT, FONT_TEMP, FONT_BACKUP, fontValidator),
            StagedFileTransaction::Status::IoError);
  EXPECT_TRUE(Storage.exists(FONT));
  EXPECT_EQ(Storage.file(FONT).back(), 1);
}

TEST_F(AtomicPersistenceTest, FontPostPublishVerifyFailureRestoresOldFont) {
  Storage.setFile(FONT, fontBytes(1));
  Storage.setFile(FONT_TEMP, fontBytes(2));
  Storage.corruptRenameTo(FONT);
  EXPECT_EQ(StagedFileTransaction::publish(FONT, FONT_TEMP, FONT_BACKUP, fontValidator),
            StagedFileTransaction::Status::IoError);
  EXPECT_TRUE(Storage.exists(FONT));
  EXPECT_EQ(Storage.file(FONT).back(), 1);
}

TEST_F(AtomicPersistenceTest, StreamVerifiedFontIsReadOnceAfterPublishAndKeepsAtomicRollback) {
  const auto uploaded = fontBytes(2);
  Storage.setFile(FONT, fontBytes(1));
  Storage.setFile(FONT_TEMP, uploaded);
  EXPECT_EQ(StagedFileTransaction::publishAndVerify(FONT, FONT_TEMP, FONT_BACKUP, streamedDigest(uploaded),
                                                    readableFontValidator),
            StagedFileTransaction::Status::Published);
  EXPECT_EQ(Storage.file(FONT), uploaded);
  EXPECT_FALSE(Storage.exists(FONT_BACKUP));

  Storage.setFile(FONT_TEMP, fontBytes(3));
  Storage.corruptRenameTo(FONT);
  EXPECT_EQ(StagedFileTransaction::publishAndVerify(FONT, FONT_TEMP, FONT_BACKUP, streamedDigest(fontBytes(3)),
                                                    readableFontValidator),
            StagedFileTransaction::Status::IoError);
  EXPECT_EQ(Storage.file(FONT), uploaded);
}

TEST_F(AtomicPersistenceTest, PendingPublishRetainsBackupUntilCooperativeVerificationCommits) {
  const auto uploaded = fontBytes(2);
  Storage.setFile(FONT, fontBytes(1));
  Storage.setFile(FONT_TEMP, uploaded);

  EXPECT_EQ(
      StagedFileTransaction::beginPendingPublish(FONT, FONT_TEMP, FONT_BACKUP, uploaded.size(), readableFontValidator),
      StagedFileTransaction::Status::Published);
  EXPECT_EQ(Storage.file(FONT), uploaded);
  ASSERT_TRUE(Storage.exists(FONT_BACKUP));
  EXPECT_EQ(Storage.file(FONT_BACKUP).back(), 1);

  EXPECT_TRUE(StagedFileTransaction::commitPendingPublish(FONT_BACKUP));
  EXPECT_FALSE(Storage.exists(FONT_BACKUP));
  EXPECT_EQ(Storage.file(FONT), uploaded);
}

TEST_F(AtomicPersistenceTest, CancelledPendingPublishRestoresPreviousFile) {
  Storage.setFile(FONT, fontBytes(1));
  Storage.setFile(FONT_TEMP, fontBytes(2));
  ASSERT_EQ(StagedFileTransaction::beginPendingPublish(FONT, FONT_TEMP, FONT_BACKUP, fontBytes(2).size(),
                                                       readableFontValidator),
            StagedFileTransaction::Status::Published);

  EXPECT_TRUE(StagedFileTransaction::rollbackPendingPublish(FONT, FONT_BACKUP));
  EXPECT_EQ(Storage.file(FONT).back(), 1);
  EXPECT_FALSE(Storage.exists(FONT_BACKUP));
}

TEST_F(AtomicPersistenceTest, InterruptedFontPublishRecoversBackup) {
  Storage.setFile(FONT_BACKUP, fontBytes(1));
  EXPECT_EQ(StagedFileTransaction::recover(FONT, FONT_BACKUP, fontValidator), StagedFileTransaction::Status::Recovered);
  EXPECT_TRUE(Storage.exists(FONT));
  EXPECT_EQ(Storage.file(FONT).back(), 1);
}

TEST_F(AtomicPersistenceTest, InvalidBackupCannotReplaceAmbiguousFinal) {
  Storage.setFile(FONT, fontBytes(1));
  Storage.setFile(FONT_BACKUP, bytes("invalid"));
  Storage.makeUnreadable(FONT);

  EXPECT_EQ(StagedFileTransaction::recover(FONT, FONT_BACKUP, readableFontValidator),
            StagedFileTransaction::Status::IoError);
  EXPECT_TRUE(Storage.exists(FONT));
  EXPECT_EQ(Storage.file(FONT).back(), 1);
  EXPECT_TRUE(Storage.exists(FONT_BACKUP));
}

TEST_F(AtomicPersistenceTest, ReadableInvalidBackupCannotBlockNewPublishWhenFinalIsMissing) {
  Storage.setFile(FONT_BACKUP, bytes("invalid"));

  EXPECT_EQ(StagedFileTransaction::recover(FONT, FONT_BACKUP, readableFontValidator),
            StagedFileTransaction::Status::NoRecoveryNeeded);
  EXPECT_FALSE(Storage.exists(FONT));
  EXPECT_FALSE(Storage.exists(FONT_BACKUP));
}

constexpr char FONT_FAMILY[] = "/.fonts/Family";
constexpr char FONT_FAMILY_TEMP[] = "/.fonts/.Family.download.tmp";
constexpr char FONT_FAMILY_BACKUP[] = "/.fonts/.Family.download.bak";

struct FamilyExpectation {
  uint8_t first;
  uint8_t second;
};

bool familyValidator(const char* directory, void* opaque) {
  const auto& expected = *static_cast<FamilyExpectation*>(opaque);
  const std::string firstPath = std::string(directory) + "/Family_12.cpfont";
  const std::string secondPath = std::string(directory) + "/Family_14.cpfont";
  return Storage.exists(firstPath.c_str()) && Storage.exists(secondPath.c_str()) &&
         Storage.file(firstPath).back() == expected.first && Storage.file(secondPath).back() == expected.second;
}

bool anyCompleteFamilyValidator(const char* directory, void*) {
  const std::string firstPath = std::string(directory) + "/Family_12.cpfont";
  const std::string secondPath = std::string(directory) + "/Family_14.cpfont";
  return Storage.exists(firstPath.c_str()) && Storage.exists(secondPath.c_str()) &&
         Storage.file(firstPath).size() >= 8 && Storage.file(secondPath).size() >= 8;
}

void setFamily(const char* directory, const uint8_t first, const uint8_t second) {
  Storage.setDirectory(directory);
  Storage.setFile(std::string(directory) + "/Family_12.cpfont", fontBytes(first));
  Storage.setFile(std::string(directory) + "/Family_14.cpfont", fontBytes(second));
}

void expectFamily(const char* directory, const uint8_t first, const uint8_t second) {
  FamilyExpectation expected{first, second};
  EXPECT_TRUE(familyValidator(directory, &expected));
}

TEST_F(AtomicPersistenceTest, NewFontFamilyPublishesOnlyWhenEveryFileIsValid) {
  setFamily(FONT_FAMILY_TEMP, 2, 2);
  FamilyExpectation expected{2, 2};
  EXPECT_EQ(FontStorageUtils::publishFamily(FONT_FAMILY, FONT_FAMILY_TEMP, FONT_FAMILY_BACKUP, familyValidator,
                                            &expected, anyCompleteFamilyValidator, nullptr),
            FontStorageUtils::FamilyTransactionStatus::Published);
  expectFamily(FONT_FAMILY, 2, 2);
  EXPECT_FALSE(Storage.exists(FONT_FAMILY_TEMP));
  EXPECT_FALSE(Storage.exists(FONT_FAMILY_BACKUP));
}

TEST_F(AtomicPersistenceTest, MultiFileFontFamilyUpdateSwapsTheWholeDirectory) {
  setFamily(FONT_FAMILY, 1, 1);
  setFamily(FONT_FAMILY_TEMP, 2, 2);
  FamilyExpectation expected{2, 2};
  EXPECT_EQ(FontStorageUtils::publishFamily(FONT_FAMILY, FONT_FAMILY_TEMP, FONT_FAMILY_BACKUP, familyValidator,
                                            &expected, anyCompleteFamilyValidator, nullptr),
            FontStorageUtils::FamilyTransactionStatus::Published);
  expectFamily(FONT_FAMILY, 2, 2);
  EXPECT_FALSE(Storage.exists(FONT_FAMILY_BACKUP));
}

TEST_F(AtomicPersistenceTest, FailedSecondDownloadNeverTouchesOldFamily) {
  setFamily(FONT_FAMILY, 1, 1);
  Storage.setDirectory(FONT_FAMILY_TEMP);
  Storage.setFile(std::string(FONT_FAMILY_TEMP) + "/Family_12.cpfont", fontBytes(2));
  FamilyExpectation expected{2, 2};
  EXPECT_EQ(FontStorageUtils::publishFamily(FONT_FAMILY, FONT_FAMILY_TEMP, FONT_FAMILY_BACKUP, familyValidator,
                                            &expected, anyCompleteFamilyValidator, nullptr),
            FontStorageUtils::FamilyTransactionStatus::InvalidStaging);
  ASSERT_TRUE(FontStorageUtils::discardStagingFamily(FONT_FAMILY_TEMP));
  expectFamily(FONT_FAMILY, 1, 1);
}

TEST_F(AtomicPersistenceTest, CancelledFamilyDownloadDiscardsOnlyStaging) {
  setFamily(FONT_FAMILY, 1, 1);
  Storage.setDirectory(FONT_FAMILY_TEMP);
  Storage.setFile(std::string(FONT_FAMILY_TEMP) + "/Family_12.cpfont", fontBytes(2));
  ASSERT_TRUE(FontStorageUtils::discardStagingFamily(FONT_FAMILY_TEMP));
  expectFamily(FONT_FAMILY, 1, 1);
  EXPECT_FALSE(Storage.exists(FONT_FAMILY_TEMP));
}

TEST_F(AtomicPersistenceTest, BadSecondFileChecksumNeverTouchesOldFamily) {
  setFamily(FONT_FAMILY, 1, 1);
  setFamily(FONT_FAMILY_TEMP, 2, 3);
  FamilyExpectation expected{2, 2};
  EXPECT_EQ(FontStorageUtils::publishFamily(FONT_FAMILY, FONT_FAMILY_TEMP, FONT_FAMILY_BACKUP, familyValidator,
                                            &expected, anyCompleteFamilyValidator, nullptr),
            FontStorageUtils::FamilyTransactionStatus::InvalidStaging);
  expectFamily(FONT_FAMILY, 1, 1);
}

TEST_F(AtomicPersistenceTest, SecondPhasePublishFailureRestoresCompleteOldFamily) {
  setFamily(FONT_FAMILY, 1, 1);
  setFamily(FONT_FAMILY_TEMP, 2, 2);
  FamilyExpectation expected{2, 2};
  Storage.failRenameTo(FONT_FAMILY);
  EXPECT_EQ(FontStorageUtils::publishFamily(FONT_FAMILY, FONT_FAMILY_TEMP, FONT_FAMILY_BACKUP, familyValidator,
                                            &expected, anyCompleteFamilyValidator, nullptr),
            FontStorageUtils::FamilyTransactionStatus::IoError);
  expectFamily(FONT_FAMILY, 1, 1);
}

TEST_F(AtomicPersistenceTest, RecoveryFromBackupThenLaterFailureKeepsRecoveredFamily) {
  setFamily(FONT_FAMILY_BACKUP, 1, 1);
  Storage.setDirectory(FONT_FAMILY_TEMP);
  Storage.setFile(std::string(FONT_FAMILY_TEMP) + "/Family_12.cpfont", fontBytes(2));
  FamilyExpectation expectedNew{2, 2};
  EXPECT_EQ(FontStorageUtils::recoverFamily(FONT_FAMILY, FONT_FAMILY_TEMP, FONT_FAMILY_BACKUP, familyValidator,
                                            &expectedNew, anyCompleteFamilyValidator, nullptr),
            FontStorageUtils::FamilyTransactionStatus::Recovered);
  expectFamily(FONT_FAMILY, 1, 1);
  EXPECT_FALSE(Storage.exists(FONT_FAMILY_BACKUP));
  EXPECT_FALSE(Storage.exists(FONT_FAMILY_TEMP));
}

TEST_F(AtomicPersistenceTest, RecoveryCommitsOnlyACompletePublishedFamily) {
  setFamily(FONT_FAMILY, 2, 2);
  setFamily(FONT_FAMILY_BACKUP, 1, 1);
  Storage.setDirectory(FONT_FAMILY_TEMP);
  FamilyExpectation expectedNew{2, 2};
  EXPECT_EQ(FontStorageUtils::recoverFamily(FONT_FAMILY, FONT_FAMILY_TEMP, FONT_FAMILY_BACKUP, familyValidator,
                                            &expectedNew, anyCompleteFamilyValidator, nullptr),
            FontStorageUtils::FamilyTransactionStatus::NoRecoveryNeeded);
  expectFamily(FONT_FAMILY, 2, 2);
  EXPECT_FALSE(Storage.exists(FONT_FAMILY_BACKUP));
  EXPECT_FALSE(Storage.exists(FONT_FAMILY_TEMP));
}

TEST_F(AtomicPersistenceTest, RecoveryNeverReplacesAFamilyWithAnIncompleteBackup) {
  Storage.setDirectory(FONT_FAMILY_BACKUP);
  Storage.setFile(std::string(FONT_FAMILY_BACKUP) + "/Family_12.cpfont", fontBytes(1));
  Storage.setDirectory(FONT_FAMILY_TEMP);
  FamilyExpectation expectedNew{2, 2};
  EXPECT_EQ(FontStorageUtils::recoverFamily(FONT_FAMILY, FONT_FAMILY_TEMP, FONT_FAMILY_BACKUP, familyValidator,
                                            &expectedNew, anyCompleteFamilyValidator, nullptr),
            FontStorageUtils::FamilyTransactionStatus::IoError);
  EXPECT_TRUE(Storage.exists(FONT_FAMILY_BACKUP));
  EXPECT_FALSE(Storage.exists(FONT_FAMILY));
}

TEST_F(AtomicPersistenceTest, FontCrcUpdateDetectionUsesSizeAndStreamingCrc) {
  const auto data = bytes("123456789");
  Storage.setFile("/font.cpfont", data);
  EXPECT_EQ(FontStorageUtils::fileMatches("/font.cpfont", data.size(), 0xCBF43926U),
            FontStorageUtils::FileMatch::Match);
  EXPECT_EQ(FontStorageUtils::fileMatches("/font.cpfont", data.size() + 1, 0xCBF43926U),
            FontStorageUtils::FileMatch::Different);
  EXPECT_EQ(FontStorageUtils::fileMatches("/font.cpfont", data.size(), 0xCBF43927U),
            FontStorageUtils::FileMatch::Different);
  EXPECT_LE(Storage.maxRead(), 512U);
}

TEST_F(AtomicPersistenceTest, UnreadableFontIsNeverReportedCurrent) {
  EXPECT_EQ(FontStorageUtils::fileMatches("/missing.cpfont", 0, 0), FontStorageUtils::FileMatch::IoError);
  Storage.setFile("/font.cpfont", bytes("same-size"));
  Storage.makeUnreadable("/font.cpfont");
  EXPECT_EQ(FontStorageUtils::fileMatches("/font.cpfont", 9, 0), FontStorageUtils::FileMatch::IoError);
}

TEST_F(AtomicPersistenceTest, FontPathContractRejectsTruncationAndAvoidsLongNameCollision) {
  std::string exactFamily(FontStorageUtils::MAX_FAMILY_NAME_BYTES, 'a');
  std::string tooLongFamily = exactFamily + 'b';
  std::string exactFile(FontStorageUtils::MAX_CPFONT_FILENAME_BYTES - 7, 'f');
  exactFile += ".cpfont";
  char path[FontStorageUtils::FONT_PATH_CAPACITY];
  EXPECT_TRUE(FontStorageUtils::isValidFamilyName(exactFamily.c_str()));
  EXPECT_FALSE(FontStorageUtils::isValidFamilyName(tooLongFamily.c_str()));
  EXPECT_TRUE(FontStorageUtils::isValidFamilyName("Family with spaces"));
  EXPECT_TRUE(FontStorageUtils::isValidFamilyName("Phông chữ Việt"));
  EXPECT_FALSE(FontStorageUtils::isValidFamilyName(" Family"));
  EXPECT_FALSE(FontStorageUtils::isValidFamilyName("Family "));
  EXPECT_FALSE(FontStorageUtils::isValidFamilyName("Family.name"));
  std::string invalidUtf8 = "Family";
  invalidUtf8.push_back(static_cast<char>(0xC3));
  EXPECT_FALSE(FontStorageUtils::isValidFamilyName(invalidUtf8.c_str()));
  EXPECT_TRUE(FontStorageUtils::isValidCpfontFilename(exactFile.c_str()));
  EXPECT_TRUE(FontStorageUtils::isValidCpfontFilename("Phông chữ Việt_18.cpfont"));
  EXPECT_FALSE(FontStorageUtils::isValidCpfontFilename("Phông chữ Việt_18 .cpfont"));
  EXPECT_FALSE(FontStorageUtils::isValidCpfontFilename(("x" + exactFile).c_str()));
  char persistedFamily[FontStorageUtils::MAX_FAMILY_NAME_BYTES + 1];
  EXPECT_TRUE(FontStorageUtils::copyPersistedFamilyName(exactFamily.c_str(), persistedFamily, sizeof(persistedFamily)));
  EXPECT_EQ(exactFamily, persistedFamily);
  EXPECT_FALSE(
      FontStorageUtils::copyPersistedFamilyName(tooLongFamily.c_str(), persistedFamily, sizeof(persistedFamily)));
  EXPECT_STREQ(persistedFamily, "");
  EXPECT_TRUE(FontStorageUtils::buildFontPath("/.fonts", exactFamily.c_str(), exactFile.c_str(), path, sizeof(path)));
  const std::string firstPath = path;
  EXPECT_FALSE(
      FontStorageUtils::buildFontPath("/.fonts", tooLongFamily.c_str(), exactFile.c_str(), path, sizeof(path)));
  ASSERT_TRUE(
      FontStorageUtils::buildFontPath("/.fonts", "Phông chữ Việt", "Phông chữ Việt_18.cpfont", path, sizeof(path)));
  EXPECT_STREQ(path, "/.fonts/Phông chữ Việt/Phông chữ Việt_18.cpfont");
  std::string otherFamily = exactFamily;
  otherFamily.back() = 'z';
  ASSERT_TRUE(FontStorageUtils::buildFontPath("/.fonts", otherFamily.c_str(), exactFile.c_str(), path, sizeof(path)));
  EXPECT_NE(firstPath, path);

  const std::string shortFile = "A.cpfont";
  const std::string exactDirectory(FontStorageUtils::FONT_PATH_CAPACITY - shortFile.size() - 2, 'd');
  EXPECT_TRUE(FontStorageUtils::buildFilePath(exactDirectory.c_str(), shortFile.c_str(), path, sizeof(path)));
  EXPECT_EQ(strlen(path), sizeof(path) - 1);
  char truncated[FontStorageUtils::FONT_PATH_CAPACITY - 1];
  EXPECT_FALSE(
      FontStorageUtils::buildFilePath(exactDirectory.c_str(), shortFile.c_str(), truncated, sizeof(truncated)));
  EXPECT_STREQ(truncated, "");
}

TEST_F(AtomicPersistenceTest, ClockCalendarConvertsRtcUtcWithoutLocalOffset) {
  const ClockCalendar::DateTime rtc{2026, 7, 22, 14, 35, 10, 3};
  time_t epoch = 0;
  ASSERT_TRUE(ClockCalendar::toEpoch(rtc, epoch));
  ClockCalendar::DateTime restored;
  ASSERT_TRUE(ClockCalendar::fromEpoch(epoch, restored));
  EXPECT_EQ(restored.year, 2026);
  EXPECT_EQ(restored.month, 7);
  EXPECT_EQ(restored.day, 22);
  EXPECT_EQ(restored.hour, 14);
  EXPECT_EQ(restored.minute, 35);
}

TEST_F(AtomicPersistenceTest, ClockCalendarRejectsInvalidRtcAndHandlesMidnight) {
  time_t epoch = 0;
  EXPECT_FALSE(ClockCalendar::toEpoch({2025, 2, 29, 0, 0, 0, 0}, epoch));
  ASSERT_TRUE(ClockCalendar::toEpoch({2024, 2, 29, 23, 59, 59, 4}, epoch));
  ClockCalendar::DateTime next;
  ASSERT_TRUE(ClockCalendar::fromEpoch(epoch + 1, next));
  EXPECT_EQ(next.year, 2024);
  EXPECT_EQ(next.month, 3);
  EXPECT_EQ(next.day, 1);
  EXPECT_EQ(next.hour, 0);
}

TEST_F(AtomicPersistenceTest, NtpPolicyDoesNotTrustPersistedFlagWhenCurrentClockIsInvalid) {
  EXPECT_TRUE(ClockSyncPolicy::shouldSyncFromNetwork(true, false));
  EXPECT_TRUE(ClockSyncPolicy::shouldSyncFromNetwork(false, true));
  EXPECT_FALSE(ClockSyncPolicy::shouldSyncFromNetwork(true, true));
}

TEST_F(AtomicPersistenceTest, TiltSensorOnlyStaysAwakeForVisibleReader) {
  EXPECT_TRUE(TiltLifecyclePolicy::shouldBeAwake(1, true));
  EXPECT_TRUE(TiltLifecyclePolicy::shouldBeAwake(2, true));
  EXPECT_FALSE(TiltLifecyclePolicy::shouldBeAwake(1, false));
  EXPECT_FALSE(TiltLifecyclePolicy::shouldBeAwake(0, true));
}

}  // namespace
