add_executable(XtcContractTest
  XtcContractTest.cpp
  ${REPO_ROOT}/lib/Xtc/Xtc.cpp
  ${REPO_ROOT}/lib/Xtc/Xtc/XtcParser.cpp
  ${REPO_ROOT}/lib/GfxRenderer/Bitmap.cpp
  ${REPO_ROOT}/lib/GfxRenderer/BitmapHelpers.cpp
  ${REPO_ROOT}/lib/Serialization/StagedFileTransaction.cpp
)

target_include_directories(XtcContractTest PRIVATE
  ${REPO_ROOT}/test/epub_source_identity/stubs
  ${REPO_ROOT}/lib/Xtc
  ${REPO_ROOT}/lib/Xtc/Xtc
  ${REPO_ROOT}/lib/ZipFile
  ${REPO_ROOT}/lib/GfxRenderer
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Serialization
)

target_compile_definitions(XtcContractTest PRIVATE
  XTC_CONVERTER_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resources"
)

target_link_libraries(XtcContractTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(XtcContractTest)
