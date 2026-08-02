add_executable(ClippingsCoreTest
  ClippingsCoreTest.cpp
  ${REPO_ROOT}/src/clippings/ClippingCodec.cpp
  ${REPO_ROOT}/src/clippings/ClipTextBuilder.cpp
)

target_include_directories(ClippingsCoreTest PRIVATE
  ${REPO_ROOT}/src/clippings
)

target_link_libraries(ClippingsCoreTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ClippingsCoreTest)

add_executable(ClippingStorePersistenceTest
  ClippingStorePersistenceTest.cpp
  ${REPO_ROOT}/src/clippings/ClippingCodec.cpp
  ${REPO_ROOT}/src/clippings/ClippingStore.cpp
)

target_include_directories(ClippingStorePersistenceTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src/clippings
)

target_link_libraries(ClippingStorePersistenceTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ClippingStorePersistenceTest)

add_executable(ClippingPageToolsTest
  ClippingPageToolsTest.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/src/clippings/ClippingCodec.cpp
  ${REPO_ROOT}/src/clippings/ClippingPageTools.cpp
)

target_include_directories(ClippingPageToolsTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/src/clippings
)

target_link_libraries(ClippingPageToolsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ClippingPageToolsTest)

add_executable(SavedClippingsModelTest
  SavedClippingsModelTest.cpp
)

target_include_directories(SavedClippingsModelTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(SavedClippingsModelTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SavedClippingsModelTest)

add_executable(BookSavedItemsModelTest
  BookSavedItemsModelTest.cpp
  ${REPO_ROOT}/src/clippings/ClippingCodec.cpp
  ${REPO_ROOT}/src/clippings/ClippingStore.cpp
)

target_include_directories(BookSavedItemsModelTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/clippings
)

target_link_libraries(BookSavedItemsModelTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookSavedItemsModelTest)

add_executable(BookmarkCatalogTest
  BookmarkCatalogTest.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/src/activities/reader/BookmarkCatalog.cpp
  ${REPO_ROOT}/src/util/BookmarkUtil.cpp
)

target_include_directories(BookmarkCatalogTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/src
)

target_link_libraries(BookmarkCatalogTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookmarkCatalogTest)

add_executable(ClippingJumpValidationTest
  ClippingJumpValidationTest.cpp
  ${REPO_ROOT}/src/clippings/ClippingCodec.cpp
)

target_include_directories(ClippingJumpValidationTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(ClippingJumpValidationTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ClippingJumpValidationTest)
