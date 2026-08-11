add_executable(VocabularyDataTest
  VocabularyDataTest.cpp
  ${REPO_ROOT}/src/vocabulary/VocabularyData.cpp
)

target_include_directories(VocabularyDataTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/vocabulary
)

target_link_libraries(VocabularyDataTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(VocabularyDataTest)
