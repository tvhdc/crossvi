add_executable(FontSystemTest
  BuiltinFontVietnameseCoverageTest.cpp
  CpFontValidationTest.cpp
  FontInflateTest.cpp
  GlyphCacheTest.cpp
  ReaderFontSizeMappingTest.cpp
  ${REPO_ROOT}/lib/EpdFont/FontDecompressor.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFontFamily.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFont.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFontRegistry.cpp
  ${REPO_ROOT}/lib/InflateReader/InflateReader.cpp
  ${REPO_ROOT}/lib/uzlib/src/tinflate.c
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(FontSystemTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/InflateReader
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/uzlib/src
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(FontSystemTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FontSystemTest)
