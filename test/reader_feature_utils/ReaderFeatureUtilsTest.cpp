#include <ClockDateFormat.h>
#include <FsHelpers.h>
#include <ReaderWordSpacing.h>
#include <SmallCaps.h>
#include <gtest/gtest.h>

#include <string>

#include "BookSearchUtils.h"
#include "DictionaryQuery.h"
#include "EpubSearchTraversal.h"
#include "HomeShortcuts.h"
#include "LazyStoreState.h"
#include "MemoryBudget.h"
#include "PowerButtonGesture.h"
#include "QrCapacity.h"
#include "activities/boot_sleep/SleepImagePlacement.h"
#include "UrlUtils.h"
#include "Utf8.h"
#include "VietnameseTelex.h"
#include "components/LibraryGridModel.h"
#include "util/WifiNetworkSelection.h"

namespace {
std::string typeTelex(const std::string& keys, const size_t maxBytes = 0) {
  std::string text;
  size_t cursor = 0;
  for (const char key : keys) {
    EXPECT_TRUE(VietnameseTelex::applyKey(text, cursor, key, maxBytes));
  }
  return text;
}
}  // namespace

TEST(HomeShortcuts, DefaultsAreBoundedUniqueAndStable) {
  const HomeShortcutList shortcuts;
  EXPECT_EQ(shortcuts.count, 6);
  EXPECT_EQ(shortcuts.at(0), HomeShortcutId::Appearance);
  EXPECT_EQ(shortcuts.at(1), HomeShortcutId::TextSettings);
  EXPECT_EQ(shortcuts.at(2), HomeShortcutId::QuickResume);
  EXPECT_EQ(shortcuts.at(3), HomeShortcutId::SleepScreen);
  EXPECT_EQ(shortcuts.at(4), HomeShortcutId::StatusBar);
  EXPECT_EQ(shortcuts.at(5), HomeShortcutId::VocabularyLearning);
  for (uint8_t index = 0; index < shortcuts.count; ++index) {
    EXPECT_TRUE(isValidHomeShortcutId(shortcuts.items[index]));
    for (uint8_t other = index + 1; other < shortcuts.count; ++other) {
      EXPECT_NE(shortcuts.items[index], shortcuts.items[other]);
    }
  }
}

TEST(HomeShortcuts, AddReplaceRemoveAndMoveKeepACompactUniqueList) {
  HomeShortcutList shortcuts;
  shortcuts.clear();
  EXPECT_TRUE(shortcuts.add(HomeShortcutId::Appearance));
  EXPECT_TRUE(shortcuts.add(HomeShortcutId::TextSettings));
  EXPECT_TRUE(shortcuts.add(HomeShortcutId::StatusBar));
  EXPECT_FALSE(shortcuts.add(HomeShortcutId::TextSettings));

  EXPECT_TRUE(shortcuts.move(2, 0));
  EXPECT_EQ(shortcuts.at(0), HomeShortcutId::StatusBar);
  EXPECT_TRUE(shortcuts.replace(1, HomeShortcutId::QuickResume));
  EXPECT_FALSE(shortcuts.replace(1, HomeShortcutId::StatusBar));
  EXPECT_TRUE(shortcuts.remove(0));
  ASSERT_EQ(shortcuts.count, 2);
  EXPECT_EQ(shortcuts.at(0), HomeShortcutId::QuickResume);
  EXPECT_EQ(shortcuts.at(1), HomeShortcutId::TextSettings);
}

TEST(HomeShortcuts, RejectsEntriesBeyondTheFixedCapacity) {
  HomeShortcutList shortcuts;
  shortcuts.clear();
  for (uint8_t raw = 0; raw < HomeShortcutList::CAPACITY; ++raw) {
    EXPECT_TRUE(shortcuts.add(static_cast<HomeShortcutId>(raw)));
  }
  EXPECT_FALSE(shortcuts.add(HomeShortcutId::OutsideReaderClock));
  EXPECT_EQ(shortcuts.count, HomeShortcutList::CAPACITY);
}

TEST(MemoryBudget, RequiresBothTotalAndContiguousHeadroom) {
  EXPECT_EQ(MemoryBudget::SD_FONT_LOAD.maxAllocHeap, 32U * 1024U);
  EXPECT_TRUE(MemoryBudget::hasHeadroom(72U * 1024U, 32U * 1024U, MemoryBudget::SD_FONT_LOAD));
  EXPECT_FALSE(MemoryBudget::hasHeadroom(71U * 1024U, 80U * 1024U, MemoryBudget::SD_FONT_LOAD));
  EXPECT_FALSE(MemoryBudget::hasHeadroom(96U * 1024U, 32U * 1024U - 1U, MemoryBudget::SD_FONT_LOAD));
  EXPECT_TRUE(MemoryBudget::hasHeadroom(60U * 1024U, 48U * 1024U, MemoryBudget::PNG_DECODE));
}

TEST(MemoryBudget, UsesMeasuredKoReaderTlsThresholds) {
  EXPECT_TRUE(MemoryBudget::hasHeadroom(50000U, 20000U, MemoryBudget::KOREADER_TLS));
  EXPECT_TRUE(MemoryBudget::hasHeadroom(51900U, 42000U, MemoryBudget::KOREADER_TLS));
  EXPECT_FALSE(MemoryBudget::hasHeadroom(49999U, 42000U, MemoryBudget::KOREADER_TLS));
  EXPECT_FALSE(MemoryBudget::hasHeadroom(50000U, 19999U, MemoryBudget::KOREADER_TLS));
}

TEST(MemoryBudget, ProtectsCssGrowthAndDynamicContiguousReservations) {
  EXPECT_TRUE(MemoryBudget::hasHeadroom(64U * 1024U, 8U * 1024U, MemoryBudget::CSS_RULE_GROWTH));
  EXPECT_FALSE(MemoryBudget::hasHeadroom(63U * 1024U, 32U * 1024U, MemoryBudget::CSS_RULE_GROWTH));
  EXPECT_FALSE(MemoryBudget::hasHeadroom(96U * 1024U, 7U * 1024U, MemoryBudget::CSS_RULE_GROWTH));

  EXPECT_TRUE(MemoryBudget::hasContiguousHeadroom(32U * 1024U, 16U * 1024U, 16U * 1024U));
  EXPECT_FALSE(MemoryBudget::hasContiguousHeadroom(32U * 1024U - 1U, 16U * 1024U, 16U * 1024U));
  EXPECT_FALSE(MemoryBudget::hasContiguousHeadroom(UINT32_MAX, UINT32_MAX, 1U));
}

TEST(SleepImagePlacement, FitsAroundTheCenter) {
  const SleepImagePlacement fit = calculateSleepImagePlacement(480, 800, 600, 600, 100, 0, 0);
  EXPECT_EQ(fit.width, 480);
  EXPECT_EQ(fit.height, 480);
  EXPECT_EQ(fit.x, 0);
  EXPECT_EQ(fit.y, 160);
}

TEST(SleepImagePlacement, AppliesZoomAndPixelOffsetWithoutChangingAspectRatio) {
  const SleepImagePlacement placement = calculateSleepImagePlacement(480, 800, 600, 600, 150, 40, -30);
  EXPECT_EQ(placement.width, 720);
  EXPECT_EQ(placement.height, 720);
  EXPECT_EQ(placement.x, -80);
  EXPECT_EQ(placement.y, 10);
}

TEST(SleepImagePlacement, MovesAnImageThatAlreadyMatchesTheScreen) {
  const SleepImagePlacement placement = calculateSleepImagePlacement(480, 800, 480, 800, 100, 40, 30);
  EXPECT_EQ(placement.x, 40);
  EXPECT_EQ(placement.y, 30);
}

TEST(SleepImagePlacement, DoesNotUpscaleFitAtOneHundredPercentAndClampsSettings) {
  const SleepImagePlacement normal = calculateSleepImagePlacement(528, 792, 100, 100, 100, 0, 0);
  EXPECT_EQ(normal.width, 100);
  EXPECT_EQ(normal.height, 100);
  EXPECT_EQ(normal.x, 214);
  EXPECT_EQ(normal.y, 346);

  const SleepImagePlacement enlarged = calculateSleepImagePlacement(528, 792, 100, 100, 200, 0, 0);
  EXPECT_EQ(enlarged.width, 200);
  EXPECT_EQ(enlarged.height, 200);
  EXPECT_EQ(enlarged.x, 164);
  EXPECT_EQ(enlarged.y, 296);

  const SleepImagePlacement clamped = calculateSleepImagePlacement(480, 800, 600, 600, 0, 9999, -9999);
  EXPECT_EQ(clamped.width, 240);
  EXPECT_EQ(clamped.height, 240);
  EXPECT_EQ(clamped.x, 360);
  EXPECT_EQ(clamped.y, -120);
}

TEST(SleepImagePlacement, HeldMovementUsesTheFastStepAfterTheThreshold) {
  EXPECT_EQ(sleepImageMoveStep(0), SLEEP_IMAGE_MOVE_STEP);
  EXPECT_EQ(sleepImageMoveStep(SLEEP_IMAGE_FAST_MOVE_HOLD_MS - 1), SLEEP_IMAGE_MOVE_STEP);
  EXPECT_EQ(sleepImageMoveStep(SLEEP_IMAGE_FAST_MOVE_HOLD_MS), SLEEP_IMAGE_FAST_MOVE_STEP);
  EXPECT_EQ(SLEEP_IMAGE_FRONT_ZOOM_STEP, 1);
  EXPECT_EQ(SLEEP_IMAGE_SIDE_ZOOM_STEP, 5);
  EXPECT_FALSE(shouldResetSleepImageTransform(SLEEP_IMAGE_RESET_HOLD_MS - 1));
  EXPECT_TRUE(shouldResetSleepImageTransform(SLEEP_IMAGE_RESET_HOLD_MS));
}

TEST(WifiNetworkSelection, KeepsStrongestResultForDuplicateSsid) {
  std::vector<WifiNetworkInfo> networks;
  WifiNetworkInfo weak{.ssid = "mesh", .rssi = -70, .isEncrypted = true, .hasSavedPassword = true};
  WifiNetworkInfo strong{.ssid = "mesh", .rssi = -42, .isEncrypted = true, .hasSavedPassword = true};
  mergeWifiScanResult(networks, std::move(weak));
  mergeWifiScanResult(networks, std::move(strong));
  ASSERT_EQ(networks.size(), 1U);
  EXPECT_EQ(networks[0].rssi, -42);
}

TEST(WifiNetworkSelection, SortsSavedNetworksBeforeUnsavedThenBySignal) {
  std::vector<WifiNetworkInfo> networks = {
      {.ssid = "strong-unsaved", .rssi = -30},
      {.ssid = "weak-saved", .rssi = -75, .hasSavedPassword = true},
      {.ssid = "strong-saved", .rssi = -40, .hasSavedPassword = true},
  };
  sortWifiNetworks(networks);
  EXPECT_EQ(networks[0].ssid, "strong-saved");
  EXPECT_EQ(networks[1].ssid, "weak-saved");
  EXPECT_EQ(networks[2].ssid, "strong-unsaved");
}

TEST(LazyStoreState, LoadsOnceAndDoesNotRetryFailedInputImplicitly) {
  LazyStoreState state;
  EXPECT_TRUE(state.beginLoad());
  EXPECT_TRUE(state.usable());
  EXPECT_FALSE(state.beginLoad());
  state.finish(false);
  EXPECT_TRUE(state.failed());
  EXPECT_FALSE(state.usable());
  EXPECT_FALSE(state.beginLoad());
}

TEST(LazyStoreState, BecomesUsableAfterSuccessfulOrRecoveryLoad) {
  LazyStoreState state;
  ASSERT_TRUE(state.beginLoad());
  state.finish(true);
  EXPECT_TRUE(state.loaded());
  EXPECT_TRUE(state.usable());

  LazyStoreState recovery;
  recovery.markLoaded();
  EXPECT_TRUE(recovery.loaded());
}

TEST(QrCapacity, UsesSafeByteModeBoundaries) {
  EXPECT_EQ(QrCapacity::select(0).version, 4);
  EXPECT_EQ(QrCapacity::select(78).version, 4);
  EXPECT_EQ(QrCapacity::select(79).version, 10);
  EXPECT_EQ(QrCapacity::select(271).version, 10);
  EXPECT_EQ(QrCapacity::select(272).version, 20);
  EXPECT_EQ(QrCapacity::select(858).version, 20);
  EXPECT_EQ(QrCapacity::select(859).version, 30);
  EXPECT_EQ(QrCapacity::select(1732).version, 30);
  EXPECT_EQ(QrCapacity::select(1733).version, 40);
  EXPECT_EQ(QrCapacity::select(2953).version, 40);
  EXPECT_EQ(QrCapacity::maxBytes(), 2953U);
}

TEST(FsHelpers, NormalisePathCollapsesCurrentDirectoryComponents) {
  EXPECT_EQ(FsHelpers::normalisePath("OEBPS/./ch1.html"), "OEBPS/ch1.html");
  EXPECT_EQ(FsHelpers::normalisePath("./OEBPS/Text/../ch1.html"), "OEBPS/ch1.html");
}

TEST(UrlUtils, ResolvesRelativeOpdsLinksAgainstTheFeedDirectory) {
  EXPECT_EQ(UrlUtils::buildUrl("http://host/opds/root.xml", "sub.xml"), "http://host/opds/sub.xml");
  EXPECT_EQ(UrlUtils::buildUrl("http://host/opds/root.xml?page=2", "book.epub"), "http://host/opds/book.epub");
  EXPECT_EQ(UrlUtils::buildUrl("http://host/opds/", "sub.xml"), "http://host/opds/sub.xml");
  EXPECT_EQ(UrlUtils::buildUrl("http://host", "sub.xml"), "http://host/sub.xml");
  EXPECT_EQ(UrlUtils::buildUrl("https://host/opds/catalog?page=1", "?page=2"), "https://host/opds/catalog?page=2");
  EXPECT_EQ(UrlUtils::buildUrl("https://host/opds/catalog?page=1", "#entry"), "https://host/opds/catalog?page=1#entry");
  EXPECT_EQ(UrlUtils::buildUrl("https://host/opds/catalog", "//cdn.example/book.epub"),
            "https://cdn.example/book.epub");
  EXPECT_EQ(UrlUtils::buildUrl("https://host?token=x", "/opds"), "https://host/opds");
  EXPECT_EQ(UrlUtils::buildUrl("https://host/opds/a/root.xml", "../book.epub"), "https://host/opds/book.epub");
  EXPECT_EQ(UrlUtils::buildUrl(UrlUtils::buildUrl("https://host/opds/root.xml", "sub/catalog.xml"), "search?q=reader"),
            "https://host/opds/sub/search?q=reader");
}

TEST(EpubSearchTraversal, ExtendsPartialCachesAndClampsInvalidStartPages) {
  EXPECT_TRUE(EpubSearchTraversal::needsBuild(false, false));
  EXPECT_TRUE(EpubSearchTraversal::needsBuild(true, true));
  EXPECT_FALSE(EpubSearchTraversal::needsBuild(true, false));
  EXPECT_EQ(EpubSearchTraversal::clampPage(-1, 10), 0);
  EXPECT_EQ(EpubSearchTraversal::clampPage(4, 10), 4);
  EXPECT_EQ(EpubSearchTraversal::clampPage(99, 10), 9);
  EXPECT_EQ(EpubSearchTraversal::clampPage(99, 0), 0);
}

TEST(ReaderWordSpacing, AddsOnlyToNaturalWordGapsAndClampsTheLevel) {
  EXPECT_EQ(readerWordSpacingExtra(0, 4), 0);
  EXPECT_EQ(readerWordSpacingExtra(-3, 4), 0);
  EXPECT_EQ(readerWordSpacingExtra(6, 0), 0);
  EXPECT_EQ(readerWordSpacingExtra(6, 1), 10);
  EXPECT_EQ(readerWordSpacingExtra(6, 4), 40);
  EXPECT_EQ(readerWordSpacingExtra(6, 255), 40);
}

TEST(SmallCaps, MapsSupportedLowercaseScriptsWithoutGuessingOtherCharacters) {
  EXPECT_TRUE(isSyntheticSmallCapsLowercase('z'));
  EXPECT_EQ(syntheticSmallCapsUppercase('z'), static_cast<uint32_t>('Z'));
  EXPECT_TRUE(isSyntheticSmallCapsLowercase(0x1EC7));       // ệ
  EXPECT_EQ(syntheticSmallCapsUppercase(0x1EC7), 0x1EC6U);  // Ệ
  EXPECT_TRUE(isSyntheticSmallCapsLowercase(0x03C2));
  EXPECT_EQ(syntheticSmallCapsUppercase(0x03C2), 0x03A3U);
  EXPECT_FALSE(isSyntheticSmallCapsLowercase(0x00DF));  // ß has no one-codepoint uppercase pair
  EXPECT_EQ(syntheticSmallCapsUppercase(0x00DF), 0x00DFU);
}

TEST(ClockDateFormat, FormatsEveryOrderAndSeparatorWithoutLocaleState) {
  char value[20]{};
  EXPECT_TRUE(ClockDateFormat::format(2026, 7, 21, ClockDateFormat::MonthDayYearLong, '/', value, sizeof(value)));
  EXPECT_STREQ(value, "Jul 21, 2026");
  EXPECT_TRUE(ClockDateFormat::format(2026, 7, 21, ClockDateFormat::DayMonthYearLong, '/', value, sizeof(value)));
  EXPECT_STREQ(value, "21 Jul 2026");
  EXPECT_TRUE(ClockDateFormat::format(2026, 7, 21, ClockDateFormat::MonthDayYearNumeric, '.', value, sizeof(value)));
  EXPECT_STREQ(value, "07.21.2026");
  EXPECT_TRUE(ClockDateFormat::format(2026, 7, 21, ClockDateFormat::DayMonthYearNumeric, '-', value, sizeof(value)));
  EXPECT_STREQ(value, "21-07-2026");
  EXPECT_TRUE(ClockDateFormat::format(2026, 7, 21, ClockDateFormat::YearMonthDayNumeric, '/', value, sizeof(value)));
  EXPECT_STREQ(value, "2026/07/21");
  EXPECT_TRUE(ClockDateFormat::format(2026, 7, 21, ClockDateFormat::MonthDayNumeric, '/', value, sizeof(value)));
  EXPECT_STREQ(value, "07/21");
  EXPECT_TRUE(ClockDateFormat::format(2026, 7, 21, ClockDateFormat::DayMonthNumeric, '/', value, sizeof(value)));
  EXPECT_STREQ(value, "21/07");
  EXPECT_TRUE(ClockDateFormat::format(2026, 9, 1, ClockDateFormat::MonthDayLong, '/', value, sizeof(value)));
  EXPECT_STREQ(value, "September 01");
  EXPECT_TRUE(ClockDateFormat::format(2026, 9, 1, ClockDateFormat::DayMonthLong, '/', value, sizeof(value)));
  EXPECT_STREQ(value, "01 September");
}

TEST(ClockDateFormat, RejectsInvalidInputAndTruncatedOutput) {
  char value[8]{};
  EXPECT_FALSE(ClockDateFormat::format(2026, 0, 1, ClockDateFormat::MonthDayNumeric, '/', value, sizeof(value)));
  EXPECT_FALSE(ClockDateFormat::format(2026, 1, 1, ClockDateFormat::MonthDayYearLong, '/', value, sizeof(value)));
  EXPECT_EQ(ClockDateFormat::separatorChar(ClockDateFormat::Period), '.');
  EXPECT_EQ(ClockDateFormat::separatorChar(ClockDateFormat::Hyphen), '-');
  EXPECT_EQ(ClockDateFormat::separatorChar(ClockDateFormat::Slash), '/');
  EXPECT_EQ(ClockDateFormat::separatorChar(99), '/');
}

TEST(ClockDateFormat, LocalizesVietnameseMonthNamesWithoutChangingNumericFormats) {
  char value[24]{};
  EXPECT_TRUE(ClockDateFormat::format(2026, 8, 1, ClockDateFormat::MonthDayYearLong, '/', value, sizeof(value), true));
  EXPECT_STREQ(value, "Thg 8 01, 2026");
  EXPECT_TRUE(ClockDateFormat::format(2026, 8, 1, ClockDateFormat::DayMonthLong, '/', value, sizeof(value), true));
  EXPECT_STREQ(value, "01 Tháng 8");
  EXPECT_TRUE(
      ClockDateFormat::format(2026, 8, 1, ClockDateFormat::DayMonthYearNumeric, '-', value, sizeof(value), true));
  EXPECT_STREQ(value, "01-08-2026");
}

TEST(ClockDateFormat, ExposesUnambiguousFormatPatterns) {
  EXPECT_STREQ(ClockDateFormat::formatPattern(ClockDateFormat::MonthDayYearLong), "MMM dd, yyyy");
  EXPECT_STREQ(ClockDateFormat::formatPattern(ClockDateFormat::DayMonthYearNumeric), "dd/MM/yyyy");
  EXPECT_STREQ(ClockDateFormat::formatPattern(ClockDateFormat::YearMonthDayNumeric), "yyyy/MM/dd");
  EXPECT_STREQ(ClockDateFormat::formatPattern(255), "MMM dd, yyyy");
}

TEST(Utf8Safety, DropsAnIncompleteSequenceStartingAtTheFirstByte) {
  EXPECT_EQ(utf8SafeTruncateBuffer("\xC3", 1), 0);
  EXPECT_EQ(utf8SafeTruncateBuffer("\xC3\xA9", 2), 2);
}

TEST(PowerButtonGesture, SingleIsImmediateWhenDoubleClickIsDisabled) {
  PowerButtonGesture gesture;
  EXPECT_EQ(gesture.update(10, true, false, true, 0, false), PowerButtonGesture::Event::None);
  EXPECT_EQ(gesture.update(100, false, true, false, 90, false), PowerButtonGesture::Event::Single);
}

TEST(PowerButtonGesture, ResolvesDoubleClickAtTheWindowBoundary) {
  PowerButtonGesture gesture;
  EXPECT_EQ(gesture.update(100, false, true, false, 50, true), PowerButtonGesture::Event::None);
  EXPECT_EQ(gesture.update(449, true, false, true, 0, true), PowerButtonGesture::Event::None);
  EXPECT_EQ(gesture.update(460, false, true, false, 11, true), PowerButtonGesture::Event::Double);

  PowerButtonGesture boundary;
  EXPECT_EQ(boundary.update(100, false, true, false, 50, true), PowerButtonGesture::Event::None);
  EXPECT_EQ(boundary.update(450, true, false, true, 0, true), PowerButtonGesture::Event::None);
  EXPECT_EQ(boundary.update(451, false, true, false, 1, true), PowerButtonGesture::Event::Double);
}

TEST(PowerButtonGesture, DeliversDelayedSingleAndCancelsPendingClickOnHold) {
  PowerButtonGesture gesture;
  EXPECT_EQ(gesture.update(100, false, true, false, 50, true), PowerButtonGesture::Event::None);
  EXPECT_EQ(gesture.update(449, false, false, false, 50, true), PowerButtonGesture::Event::None);
  EXPECT_EQ(gesture.update(450, false, false, false, 50, true), PowerButtonGesture::Event::Single);

  EXPECT_EQ(gesture.update(1000, true, false, true, 0, true), PowerButtonGesture::Event::None);
  EXPECT_EQ(gesture.update(1499, false, false, true, 499, true), PowerButtonGesture::Event::None);
  EXPECT_EQ(gesture.update(1500, false, false, true, 500, true), PowerButtonGesture::Event::Hold);
  EXPECT_EQ(gesture.update(1510, false, true, false, 510, true), PowerButtonGesture::Event::None);
}

TEST(PowerButtonGesture, CancelAndMillisWrapDoNotLeakClicks) {
  PowerButtonGesture gesture;
  EXPECT_EQ(gesture.update(UINT32_MAX - 100, false, true, false, 10, true), PowerButtonGesture::Event::None);
  EXPECT_EQ(gesture.update(249, false, false, false, 10, true), PowerButtonGesture::Event::Single);

  EXPECT_EQ(gesture.update(1000, false, true, false, 10, true), PowerButtonGesture::Event::None);
  gesture.cancel();
  EXPECT_EQ(gesture.update(2000, false, false, false, 0, true), PowerButtonGesture::Event::None);
}

TEST(DictionaryQuery, BuildsVietnamesePhraseAndComposesNfc) {
  const char* words[] = {"C\xC3\xB4ng", "ngh\xE1\xBB\x87", "th\xC3\xB4ng", "tin"};
  std::string query;
  ASSERT_TRUE(DictionaryQuery::buildPhrase(words, 4, query));
  EXPECT_EQ(query, "c\xC3\xB4ng ngh\xE1\xBB\x87 th\xC3\xB4ng tin");

  const std::string nfd = std::string("xa\xCC\x83");
  const char* nfdWords[] = {nfd.c_str(), "h\xE1\xBB\x99i"};
  ASSERT_TRUE(DictionaryQuery::buildPhrase(nfdWords, 2, query));
  EXPECT_EQ(query, "x\xC3\xA3 h\xE1\xBB\x99i");

  const char* uppercaseWords[] = {"\xC4\x90\xE1\xBB\x9CI", "S\xE1\xBB\x90NG"};
  ASSERT_TRUE(DictionaryQuery::buildPhrase(uppercaseWords, 2, query));
  EXPECT_EQ(query, "\xC4\x91\xE1\xBB\x9Di s\xE1\xBB\x91ng");
}

TEST(DictionaryQuery, TrimsPunctuationAndEnforcesBounds) {
  const char* words[] = {"(x\xC3\xA3", "h\xE1\xBB\x99i),"};
  std::string query;
  ASSERT_TRUE(DictionaryQuery::buildPhrase(words, 2, query));
  EXPECT_EQ(query, "x\xC3\xA3 h\xE1\xBB\x99i");

  const std::string tooLong(256, 'a');
  const char* longWord[] = {tooLong.c_str()};
  EXPECT_FALSE(DictionaryQuery::buildPhrase(longWord, 1, query));
  EXPECT_TRUE(query.empty());

  const char* five[] = {"a", "b", "c", "d", "e"};
  EXPECT_FALSE(DictionaryQuery::buildPhrase(five, 5, query));

  const char* quoted[] = {"“Xã”,", "hội—"};
  ASSERT_TRUE(DictionaryQuery::buildPhrase(quoted, 2, query));
  EXPECT_EQ(query, "xã hội");
}

TEST(DictionaryQuery, KeepsUnicodeLookupCharactersAndCjkTokenSpacing) {
  EXPECT_EQ(DictionaryQuery::clean("（中文）"), "中文");
  EXPECT_EQ(DictionaryQuery::clean("『日本語』"), "日本語");

  const char* cjkWords[] = {"中", "文", "词", "典"};
  const bool joined[] = {false, true, true, true};
  std::string query;
  ASSERT_TRUE(DictionaryQuery::buildPhrase(cjkWords, 4, query, joined));
  EXPECT_EQ(query, "中文词典");

  ASSERT_TRUE(DictionaryQuery::buildPhrase(cjkWords, 4, query));
  EXPECT_EQ(query, "中 文 词 典");
}

TEST(BookSearch, MatchesVietnameseWithoutDiacriticsAndRanksExactFirst) {
  const BookSearchQuery titleQuery = makeBookSearchQuery("mat biec");
  EXPECT_EQ(matchBookSearch(titleQuery,
                            "M\xE1\xBA\xAF"
                            "t bi\xE1\xBA\xBF"
                            "c.epub"),
            BookSearchMatch::Folded);
  EXPECT_EQ(matchBookSearch(titleQuery, "Mat biec - ban dep.epub"), BookSearchMatch::Exact);

  const BookSearchQuery authorQuery = makeBookSearchQuery("nguyen nhat anh");
  EXPECT_EQ(matchBookSearch(authorQuery, "Nguy\xE1\xBB\x85n Nh\xE1\xBA\xADt \xC3\x81nh"), BookSearchMatch::Folded);
  EXPECT_EQ(matchBookSearch(makeBookSearchQuery("dac nhan tam"),
                            "\xC4\x90\xE1\xBA\xAF"
                            "c nh\xC3\xA2n t\xC3\xA2m"),
            BookSearchMatch::Folded);
}

TEST(BookSearch, HandlesNfdSeparatorsAndBounds) {
  const std::string nfd = std::string(
      "Ma\xCC\x86\xCC\x81"
      "t_bie\xCC\x82\xCC\x81"
      "c");
  EXPECT_EQ(matchBookSearch(makeBookSearchQuery("mat biec"), nfd), BookSearchMatch::Folded);
  EXPECT_TRUE(makeBookSearchQuery(std::string(100, 'a')).exact.size() <= BOOK_SEARCH_QUERY_BYTES);
  EXPECT_EQ(makeFoldedBookSearchKey(nfd), makeBookSearchQuery(nfd).folded);
  EXPECT_EQ(matchBookSearch(makeBookSearchQuery("missing"),
                            "M\xE1\xBA\xAF"
                            "t bi\xE1\xBA\xBF"
                            "c"),
            BookSearchMatch::None);
}

TEST(BookSearch, KeepsPageCapacityBestResultsAndReportsOverflow) {
  constexpr size_t pageCapacity = 9;
  std::vector<size_t> results;
  size_t exactCount = 0;
  bool truncated = false;
  EXPECT_TRUE(results.empty());

  for (size_t i = 0; i < pageCapacity; ++i) {
    addRankedBookSearchResult(results, exactCount, truncated, i, BookSearchMatch::Folded, pageCapacity);
    if (i == 0) {
      EXPECT_EQ(results.size(), 1u);
    }
  }
  EXPECT_EQ(results.size(), pageCapacity);
  EXPECT_FALSE(truncated);

  addRankedBookSearchResult(results, exactCount, truncated, pageCapacity, BookSearchMatch::Exact, pageCapacity);
  ASSERT_EQ(results.size(), pageCapacity);
  EXPECT_TRUE(truncated);
  EXPECT_EQ(results.front(), pageCapacity);
  EXPECT_EQ(exactCount, 1u);

  for (size_t i = pageCapacity + 1; i < 40; ++i) {
    addRankedBookSearchResult(results, exactCount, truncated, i, BookSearchMatch::Exact, pageCapacity);
  }
  EXPECT_EQ(results.size(), pageCapacity);
  EXPECT_EQ(exactCount, pageCapacity);
  EXPECT_EQ(results.front(), pageCapacity);
  EXPECT_EQ(results.back(), pageCapacity * 2 - 1);
}

TEST(BookSearch, HonorsGridAndHardLimitCapacities) {
  for (const size_t capacity :
       {size_t{0}, size_t{1}, size_t{6}, size_t{7}, size_t{12}, BOOK_SEARCH_RESULT_HARD_LIMIT}) {
    std::vector<size_t> results;
    size_t exactCount = 0;
    bool truncated = false;
    for (size_t i = 0; i <= capacity; ++i) {
      addRankedBookSearchResult(results, exactCount, truncated, i, BookSearchMatch::Folded, capacity);
    }
    EXPECT_EQ(results.size(), capacity);
    EXPECT_TRUE(truncated);
  }
}

TEST(BookSearch, IgnoresNonMatchesWithoutMarkingOverflow) {
  std::vector<size_t> results;
  size_t exactCount = 0;
  bool truncated = false;
  addRankedBookSearchResult(results, exactCount, truncated, 0, BookSearchMatch::None, 0);
  EXPECT_TRUE(results.empty());
  EXPECT_EQ(exactCount, 0u);
  EXPECT_FALSE(truncated);
}

TEST(BookSearch, ChunkedWindowOffsetsRetainOverlapAndDetectDuplicateRanges) {
  EXPECT_EQ(chunkedSearchWindowBase(2048, 256), 1792U);
  EXPECT_EQ(chunkedSearchWindowBase(128, 256), 0U);
  EXPECT_TRUE(bookSearchRangesOverlap(1536, 512, 1792, 512));
  EXPECT_TRUE(bookSearchRangesOverlap(1792, 512, 1536, 512));
  EXPECT_FALSE(bookSearchRangesOverlap(1536, 256, 1792, 512));
}

TEST(LibraryGridModel, UsesExactBoundedPageSizes) {
  EXPECT_EQ(LibraryGridModel::shape(0).columns, 3);
  EXPECT_EQ(LibraryGridModel::shape(0).rows, 2);
  EXPECT_EQ(LibraryGridModel::pageSize(0), 6u);
  EXPECT_EQ(LibraryGridModel::pageSize(1), 6u);
  EXPECT_EQ(LibraryGridModel::pageSize(255), 6u);
  EXPECT_TRUE(LibraryGridModel::usesPerCoverTitles(0));
  EXPECT_TRUE(LibraryGridModel::usesPerCoverTitles(1));
  EXPECT_EQ(LibraryGridModel::canonicalSetting(0), 0);
  EXPECT_EQ(LibraryGridModel::canonicalSetting(1), 0);
  EXPECT_EQ(LibraryGridModel::canonicalSetting(2), 0);
  EXPECT_EQ(LibraryGridModel::canonicalSetting(256), 0);
  EXPECT_EQ(LibraryGridModel::canonicalSetting(-1), 0);
  EXPECT_EQ(LibraryGridModel::migrateLegacySetting(0), 0);
  EXPECT_EQ(LibraryGridModel::migrateLegacySetting(1), 0);
  EXPECT_EQ(LibraryGridModel::migrateLegacySetting(2), 0);
  EXPECT_EQ(LibraryGridModel::migrateLegacySetting(255), 0);
  EXPECT_EQ(LibraryGridModel::productionThumbnailWidth(168), 100);
  EXPECT_EQ(LibraryGridModel::productionThumbnailWidth(255), 153);
}

TEST(LibraryGridModel, PrioritizesSelectedCoverWithoutRepeatingPageRecords) {
  constexpr std::array<size_t, 6> expected = {2, 0, 1, 3, 4, 5};
  for (size_t cursor = 0; cursor < expected.size(); ++cursor) {
    EXPECT_EQ(LibraryGridModel::coverQueueOffset(cursor, 2), expected[cursor]);
  }
}

TEST(LibraryGridModel, DefersCoverWorkUntilTheInputIdleWindowExpires) {
  EXPECT_FALSE(LibraryGridModel::coverWorkIdle(1999, 0, 2000));
  EXPECT_TRUE(LibraryGridModel::coverWorkIdle(2000, 0, 2000));
  EXPECT_TRUE(LibraryGridModel::coverWorkIdle(1000, UINT32_MAX - 1499, 2000));
}

TEST(LibraryGridModel, CalculatesPaginationForEveryGridWithoutInvalidPages) {
  for (const size_t capacity : {size_t{6}}) {
    EXPECT_EQ(LibraryGridModel::pageCount(0, capacity), 0u);
    EXPECT_EQ(LibraryGridModel::pageCount(1, capacity), 1u);
    EXPECT_EQ(LibraryGridModel::pageCount(capacity, capacity), 1u);
    EXPECT_EQ(LibraryGridModel::pageCount(capacity + 1, capacity), 2u);
    EXPECT_EQ(LibraryGridModel::pageCount(capacity * 2, capacity), 2u);
    EXPECT_EQ(LibraryGridModel::pageNumber(0, 1, capacity), 1u);
    EXPECT_EQ(LibraryGridModel::pageNumber(capacity - 1, capacity, capacity), 1u);
    EXPECT_EQ(LibraryGridModel::pageNumber(capacity, capacity + 1, capacity), 2u);
    EXPECT_EQ(LibraryGridModel::pageNumber(99, capacity + 1, capacity), 2u);
    EXPECT_EQ(LibraryGridModel::pageStart(capacity, capacity + 1, capacity), capacity);
    EXPECT_EQ(LibraryGridModel::pageStart(99, capacity + 1, capacity), capacity);
    EXPECT_EQ(LibraryGridModel::lastIndexOnPage(99, capacity + 1, capacity), capacity);
  }
}

TEST(LibraryGridModel, UsesNineRowsPerLibraryListPage) {
  constexpr size_t pageSize = LibraryGridModel::LIST_PAGE_SIZE;
  EXPECT_EQ(pageSize, 9u);
  EXPECT_EQ(LibraryGridModel::pageCount(0, pageSize), 0u);
  EXPECT_EQ(LibraryGridModel::pageCount(1, pageSize), 1u);
  EXPECT_EQ(LibraryGridModel::pageCount(9, pageSize), 1u);
  EXPECT_EQ(LibraryGridModel::pageCount(10, pageSize), 2u);
  EXPECT_EQ(LibraryGridModel::pageNumber(8, 10, pageSize), 1u);
  EXPECT_EQ(LibraryGridModel::pageNumber(9, 10, pageSize), 2u);
  EXPECT_EQ(LibraryGridModel::pageStart(17, 20, pageSize), 9u);
}

TEST(LibraryGridModel, CollectsPinnedThenSortedPageSourcesInOnePass) {
  constexpr std::array<size_t, 8> sorted = {7, 6, 5, 4, 3, 2, 1, 0};
  constexpr std::array<size_t, 2> pinned = {2, 6};
  std::array<size_t, 4> firstPage{};
  ASSERT_TRUE(LibraryGridModel::collectVisiblePageSources(sorted, pinned, 8, 0, firstPage));
  EXPECT_EQ(firstPage, (std::array<size_t, 4>{2, 6, 7, 5}));

  std::array<size_t, 3> secondPage{};
  ASSERT_TRUE(LibraryGridModel::collectVisiblePageSources(sorted, pinned, 8, 4, secondPage));
  EXPECT_EQ(secondPage, (std::array<size_t, 3>{4, 3, 1}));
}

TEST(LibraryGridModel, RejectsInvalidOrIncompleteVisiblePageSources) {
  constexpr std::array<size_t, 3> sorted = {0, 1, 2};
  constexpr std::array<size_t, 1> invalidPinned = {4};
  std::array<size_t, 1> output{};
  EXPECT_FALSE(LibraryGridModel::collectVisiblePageSources(sorted, invalidPinned, 3, 0, output));
  EXPECT_FALSE(LibraryGridModel::collectVisiblePageSources(sorted, std::span<const size_t>{}, 3, 3, output));
}

TEST(LibraryGridModel, EntersTheRememberedPageFromTabFocus) {
  constexpr size_t capacity = LibraryGridModel::LIST_PAGE_SIZE;
  EXPECT_EQ(LibraryGridModel::pageStart(17, 20, capacity), 9U);
  EXPECT_EQ(LibraryGridModel::lastIndexOnPage(17, 20, capacity), 17U);
  EXPECT_EQ(LibraryGridModel::pageStart(19, 20, capacity), 18U);
  EXPECT_EQ(LibraryGridModel::lastIndexOnPage(19, 20, capacity), 19U);
  EXPECT_EQ(LibraryGridModel::lastIndexOnPage(0, 0, capacity), 0U);
}

TEST(LibraryGridModel, MovesSequentiallyAcrossPagesWithoutWrapping) {
  EXPECT_EQ(LibraryGridModel::previousIndex(9, 1), 0u);
  EXPECT_EQ(LibraryGridModel::nextIndex(9, 1), 0u);
  EXPECT_EQ(LibraryGridModel::previousIndex(0, 32), 0u);
  EXPECT_EQ(LibraryGridModel::nextIndex(0, 32), 1u);
  EXPECT_EQ(LibraryGridModel::nextIndex(5, 32), 6u);
  EXPECT_EQ(LibraryGridModel::previousIndex(6, 32), 5u);
  EXPECT_EQ(LibraryGridModel::nextIndex(11, 32), 12u);
  EXPECT_EQ(LibraryGridModel::previousIndex(12, 32), 11u);
  EXPECT_EQ(LibraryGridModel::nextIndex(31, 32), 31u);
  EXPECT_EQ(LibraryGridModel::clampIndex(31, 17), 16u);
}

TEST(LibraryGridModel, RestoresIndependentTabSelectionsAndClampsAfterRefresh) {
  const std::vector<std::string> recent = {"a", "b", "c"};
  const std::vector<std::string> all = {"w", "x", "y", "z"};
  const size_t recentIndex =
      LibraryGridModel::restoreIndex(2, recent.size(), [&recent](const size_t index) { return recent[index] == "c"; });
  const size_t allIndex =
      LibraryGridModel::restoreIndex(1, all.size(), [&all](const size_t index) { return all[index] == "x"; });
  EXPECT_EQ(recentIndex, 2u);
  EXPECT_EQ(allIndex, 1u);

  const std::vector<std::string> refreshed = {"c", "a"};
  EXPECT_EQ(LibraryGridModel::restoreIndex(2, refreshed.size(),
                                           [&refreshed](const size_t index) { return refreshed[index] == "c"; }),
            0u);
  EXPECT_EQ(LibraryGridModel::restoreIndex(9, refreshed.size(), [](const size_t) { return false; }), 1u);
}

TEST(VietnameseTelex, ComposesVietnameseVowelsAndTonesAsNfc) {
  EXPECT_EQ(typeTelex("aas"), "ấ");
  EXPECT_EQ(typeTelex("awj"), "ặ");
  EXPECT_EQ(typeTelex("oox"), "ỗ");
  EXPECT_EQ(typeTelex("owr"), "ở");
  EXPECT_EQ(typeTelex("uwj"), "ự");
  EXPECT_EQ(typeTelex("dd"), "đ");
  EXPECT_EQ(typeTelex("w"), "ư");
  EXPECT_EQ(typeTelex("uow"), "ươ");
}

TEST(VietnameseTelex, TypesCommonVietnameseWordsAndPhrases) {
  EXPECT_EQ(typeTelex("tieengs Vieetj"), "tiếng Việt");
  EXPECT_EQ(typeTelex("dduwowngf"), "đường");
  EXPECT_EQ(typeTelex("hoangf"), "hoàng");
  EXPECT_EQ(typeTelex("hoaf"), "hòa");
  EXPECT_EQ(typeTelex("khoer"), "khỏe");
  EXPECT_EQ(typeTelex("thuyr"), "thủy");
  EXPECT_EQ(typeTelex("quas"), "quá");
  EXPECT_EQ(typeTelex("giaf"), "già");
  EXPECT_EQ(typeTelex("ngoaif"), "ngoài");
}

TEST(VietnameseTelex, RepositionsAndReplacesToneMarks) {
  EXPECT_EQ(typeTelex("toans"), "toán");
  EXPECT_EQ(typeTelex("toasn"), "toán");
  EXPECT_EQ(typeTelex("toanfs"), "toán");

  std::string afterDelete = typeTelex("toans");
  afterDelete.pop_back();
  size_t cursor = afterDelete.size();
  ASSERT_TRUE(VietnameseTelex::normalizeAtCursor(afterDelete, cursor));
  EXPECT_EQ(afterDelete, "tóa");
}

TEST(VietnameseTelex, PreservesCaseAndSupportsEscapeKeys) {
  EXPECT_EQ(typeTelex("AAS"), "Ấ");
  EXPECT_EQ(typeTelex("Dd"), "Đ");
  EXPECT_EQ(typeTelex("Tieengs"), "Tiếng");
  EXPECT_EQ(typeTelex("aaa"), "aa");
  EXPECT_EQ(typeTelex("aww"), "aw");
  EXPECT_EQ(typeTelex("ddd"), "dd");
  EXPECT_EQ(typeTelex("herr"), "her");
  EXPECT_EQ(typeTelex("az"), "az");
  EXPECT_EQ(typeTelex("toansz"), "toan");
}

TEST(VietnameseTelex, DoesNotShapeVowelsSeparatedByConsonants) {
  EXPECT_EQ(typeTelex("banana"), "banana");
  EXPECT_EQ(typeTelex("camera"), "camera");
  EXPECT_EQ(typeTelex("internet"), "internet");
  EXPECT_EQ(typeTelex("facebook"), "facebook");
  EXPECT_EQ(typeTelex("kaka"), "kaka");
}

TEST(VietnameseTelex, HandlesCursorEditsAndByteLimitsAtomically) {
  std::string text = "tn";
  size_t cursor = 1;
  for (const char key : std::string("oas")) {
    ASSERT_TRUE(VietnameseTelex::applyKey(text, cursor, key));
  }
  EXPECT_EQ(text, "toán");
  EXPECT_EQ(cursor, std::string("toá").size());

  text = "a";
  cursor = 1;
  EXPECT_FALSE(VietnameseTelex::applyKey(text, cursor, 'w', 1));
  EXPECT_EQ(text, "a");
  EXPECT_EQ(cursor, 1u);

  text = "á";
  cursor = 1;
  EXPECT_FALSE(VietnameseTelex::applyKey(text, cursor, 'n'));
  EXPECT_EQ(text, "á");
  EXPECT_EQ(cursor, 1u);
}

TEST(VietnameseTelex, BoundsWorkToTheCurrentToken) {
  std::string text(17, 'b');
  size_t cursor = text.size();
  ASSERT_TRUE(VietnameseTelex::applyKey(text, cursor, 's'));
  EXPECT_EQ(text, std::string(17, 'b') + "s");
  EXPECT_EQ(cursor, 18u);
}
