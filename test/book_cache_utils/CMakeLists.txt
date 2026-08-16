add_executable(BookCacheUtilsTest
  BookCacheUtilsTest.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/src/util/BookCacheUtils.cpp
  ${REPO_ROOT}/src/util/BookmarkUtil.cpp
)

target_include_directories(BookCacheUtilsTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/util
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(BookCacheUtilsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookCacheUtilsTest)

add_executable(BookFilePublishTest
  BookFilePublishTest.cpp
  HttpTransportPolicyTest.cpp
  HttpStringAppendTest.cpp
  UploadPathGuardTest.cpp
  ${REPO_ROOT}/lib/HttpTransportPolicy/HttpTransportPolicy.cpp
  ${REPO_ROOT}/src/network/UploadPathGuard.cpp
  ${REPO_ROOT}/src/util/BookPathMoveUtils.cpp
  ${REPO_ROOT}/src/util/BookCacheUtils.cpp
  ${REPO_ROOT}/src/util/BookmarkUtil.cpp
  ${REPO_ROOT}/src/platform/http_utils_oom.c
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(BookFilePublishTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/network
  ${REPO_ROOT}/src/util
  ${REPO_ROOT}/lib/HttpTransportPolicy
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(BookFilePublishTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)
target_link_options(BookFilePublishTest PRIVATE -Wl,--wrap=realloc)

gtest_discover_tests(BookFilePublishTest)
