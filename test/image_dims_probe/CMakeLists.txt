add_executable(ImageDimsProbeTest
  ImageDimsProbeTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/ImageDecoderFactory.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/ImageDimsProbe.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/ImageToFramebufferDecoder.cpp
  ${REPO_ROOT}/lib/GfxRenderer/BitmapHelpers.cpp
  ${REPO_ROOT}/lib/PngToBmpConverter/PngToBmpConverter.cpp
)

target_include_directories(ImageDimsProbeTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/test/epub_source_identity/stubs
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Epub/Epub/converters
  ${REPO_ROOT}/lib/Epub/Epub/blocks
  ${REPO_ROOT}/lib/GfxRenderer
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/PngToBmpConverter
)

target_link_libraries(ImageDimsProbeTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ImageDimsProbeTest)
