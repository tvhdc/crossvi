add_executable(PngConverterSafetyTest
  PngConverterSafetyTest.cpp
  ${REPO_ROOT}/lib/PngToBmpConverter/PngToBmpConverter.cpp
  ${REPO_ROOT}/lib/GfxRenderer/BitmapHelpers.cpp
  ${REPO_ROOT}/lib/Memory/BuildScratch.cpp
  ${REPO_ROOT}/lib/miniz/src/InflateStream.cpp
  ${REPO_ROOT}/lib/miniz/src/miniz_impl.c
)

target_include_directories(PngConverterSafetyTest BEFORE PRIVATE
  ${REPO_ROOT}/lib/miniz/src
  ${REPO_ROOT}/lib/PngToBmpConverter
  ${CMAKE_CURRENT_SOURCE_DIR}/../image_dims_probe/stubs
  ${CMAKE_CURRENT_SOURCE_DIR}/../epub_source_identity/stubs
  ${REPO_ROOT}/lib/GfxRenderer
  ${REPO_ROOT}/lib/Memory
)

target_link_libraries(PngConverterSafetyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PngConverterSafetyTest)
