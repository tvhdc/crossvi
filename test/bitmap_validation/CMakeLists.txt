add_executable(BitmapValidationTest
  BitmapValidationTest.cpp
  ${REPO_ROOT}/lib/GfxRenderer/Bitmap.cpp
  ${REPO_ROOT}/lib/GfxRenderer/BitmapHelpers.cpp
  ${REPO_ROOT}/lib/Serialization/StagedFileTransaction.cpp
)

target_include_directories(BitmapValidationTest PRIVATE
  ${REPO_ROOT}/test/epub_source_identity/stubs
  ${REPO_ROOT}/lib/GfxRenderer
  ${REPO_ROOT}/lib/Serialization
)

target_link_libraries(BitmapValidationTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BitmapValidationTest)
