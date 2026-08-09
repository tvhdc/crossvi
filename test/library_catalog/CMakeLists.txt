add_executable(LibraryCatalogTest
  LibraryCatalogTest.cpp
  stubs/RecentBooksStoreStub.cpp
  ${REPO_ROOT}/src/LibraryCatalogStore.cpp
  ${REPO_ROOT}/src/util/BookSearchUtils.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/lib/Serialization/StagedFileTransaction.cpp
  ${REPO_ROOT}/simulator/src/HalStorage.cpp
)

target_include_directories(LibraryCatalogTest BEFORE PRIVATE
  ${REPO_ROOT}/lib/Utf8
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/util
  ${REPO_ROOT}/simulator/src
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/EpdFont
)

target_link_libraries(LibraryCatalogTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
  ArduinoJson
)

gtest_discover_tests(LibraryCatalogTest)
