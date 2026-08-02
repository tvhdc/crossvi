add_executable(BookmarkJsonIOTest
  BookmarkJsonIOTest.cpp
  ${REPO_ROOT}/lib/Serialization/AtomicFile.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/src/BookmarkJsonIO.cpp
  ${REPO_ROOT}/src/clippings/ClippingCodec.cpp
  ${REPO_ROOT}/src/util/BookmarkUtil.cpp
)

target_include_directories(BookmarkJsonIOTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/test/clippings_core/stubs
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/simulator/src
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/clippings
  ${REPO_ROOT}/src/util
)

target_compile_options(BookmarkJsonIOTest PRIVATE
  -include ${REPO_ROOT}/simulator/src/WString.h
)

target_link_libraries(BookmarkJsonIOTest PRIVATE
  ArduinoJson
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookmarkJsonIOTest)
