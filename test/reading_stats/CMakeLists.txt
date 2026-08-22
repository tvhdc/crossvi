add_executable(ReadingStatsTest
  DailyBookReadingHistoryTest.cpp
  DailyReadingHistoryTest.cpp
  ReadingCalendarModelTest.cpp
  ReadingAchievementsTest.cpp
  ReadingStatsTest.cpp
  ReadingStatsPresentationTest.cpp
  VCodexStatsImporterTest.cpp
  ${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
  ${REPO_ROOT}/src/activities/network/NearbyStatsStorage.cpp
  ${REPO_ROOT}/src/activities/reader/BookReadingStats.cpp
  ${REPO_ROOT}/src/activities/reader/DailyBookReadingHistory.cpp
  ${REPO_ROOT}/src/activities/reader/DailyReadingHistory.cpp
  ${REPO_ROOT}/src/activities/reader/GlobalReadingStats.cpp
  ${REPO_ROOT}/src/activities/reader/ReadingStatsCodec.cpp
  ${REPO_ROOT}/src/activities/reader/ReadingStatsCompletionTransaction.cpp
  ${REPO_ROOT}/src/activities/reader/ReadingStatsEnvelope.cpp
  ${REPO_ROOT}/src/activities/reader/ReadingStatsModels.cpp
  ${REPO_ROOT}/src/activities/reader/ReadingCalendarModel.cpp
  ${REPO_ROOT}/src/activities/reader/ReadingAchievements.cpp
  ${REPO_ROOT}/src/activities/reader/ReadingStatsPresentation.cpp
  ${REPO_ROOT}/src/activities/reader/ReadingStatsStorage.cpp
  ${REPO_ROOT}/src/activities/reader/ReadingStatsVersionGuard.cpp
  ${REPO_ROOT}/src/activities/reader/ReadingStatsUtils.cpp
  ${REPO_ROOT}/src/activities/reader/VCodexStatsImporter.cpp
)

target_include_directories(ReadingStatsTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/activities/reader
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/JsonParser
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/simulator/src
)

target_link_libraries(ReadingStatsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReadingStatsTest)
