add_executable(JpegConverterSafetyTest
  JpegConverterSafetyTest.cpp
  ${REPO_ROOT}/lib/JpegToBmpConverter/JpegToBmpConverter.cpp
  ${REPO_ROOT}/lib/GfxRenderer/BitmapHelpers.cpp
)

target_include_directories(JpegConverterSafetyTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${CMAKE_CURRENT_SOURCE_DIR}/../image_dims_probe/stubs
  ${REPO_ROOT}/lib/JpegToBmpConverter
  ${CMAKE_CURRENT_SOURCE_DIR}/../epub_source_identity/stubs
  ${REPO_ROOT}/lib/GfxRenderer
  ${REPO_ROOT}/lib/Memory
)

target_link_libraries(JpegConverterSafetyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(JpegConverterSafetyTest)
