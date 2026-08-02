add_executable(KOReaderXPathTest
  KOReaderXPathTest.cpp
  ${REPO_ROOT}/lib/KOReaderSync/ChapterXPathResolver.cpp
)
target_include_directories(KOReaderXPathTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/KOReaderSync
)
target_link_libraries(KOReaderXPathTest PRIVATE crosspoint_test_common GTest::gtest_main expat)
gtest_discover_tests(KOReaderXPathTest)
