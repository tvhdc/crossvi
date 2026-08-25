add_executable(VocabularyDataTest
  VocabularyDataTest.cpp
  ${REPO_ROOT}/src/vocabulary/VocabularyData.cpp
  ${REPO_ROOT}/src/vocabulary/VocabularyReviewStore.cpp
  ${REPO_ROOT}/lib/Serialization/AtomicFile.cpp
  ${REPO_ROOT}/simulator/src/HalStorage.cpp
)

target_include_directories(VocabularyDataTest BEFORE PRIVATE
  ${REPO_ROOT}/simulator/src
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/vocabulary
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Logging
)

target_compile_definitions(VocabularyDataTest PRIVATE UNIT_TEST)

target_link_libraries(VocabularyDataTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(VocabularyDataTest)
