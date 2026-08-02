add_executable(PixelCacheIntegrationTest
  PixelCacheIntegrationTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/blocks/ImageBlock.cpp
)

target_include_directories(PixelCacheIntegrationTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${CMAKE_CURRENT_SOURCE_DIR}/../epub_source_identity/stubs
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Serialization
)

target_link_libraries(PixelCacheIntegrationTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PixelCacheIntegrationTest)
