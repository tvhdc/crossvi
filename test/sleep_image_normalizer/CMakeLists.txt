add_executable(SleepImageNormalizerTest
  SleepImageNormalizerTest.cpp
  ${REPO_ROOT}/src/activities/boot_sleep/SleepImageNormalizer.cpp
  ${REPO_ROOT}/src/activities/boot_sleep/SleepImageValidation.cpp
  ${REPO_ROOT}/src/activities/boot_sleep/SleepImageSelectionStore.cpp
  ${REPO_ROOT}/lib/PngToBmpConverter/PngToBmpConverter.cpp
  ${REPO_ROOT}/lib/GfxRenderer/Bitmap.cpp
  ${REPO_ROOT}/lib/GfxRenderer/BitmapHelpers.cpp
  ${REPO_ROOT}/lib/Serialization/AtomicFile.cpp
  ${REPO_ROOT}/lib/Serialization/StagedFileTransaction.cpp
  ${REPO_ROOT}/lib/Memory/BuildScratch.cpp
  ${REPO_ROOT}/lib/miniz/src/InflateStream.cpp
  ${REPO_ROOT}/lib/miniz/src/miniz_impl.c
)

target_include_directories(SleepImageNormalizerTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/miniz/src
  ${REPO_ROOT}/lib/PngToBmpConverter
  ${CMAKE_CURRENT_SOURCE_DIR}/../epub_source_identity/stubs
  ${CMAKE_CURRENT_SOURCE_DIR}/../image_dims_probe/stubs
  ${REPO_ROOT}/src/activities/boot_sleep
  ${REPO_ROOT}/lib/GfxRenderer
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Memory
)

target_link_libraries(SleepImageNormalizerTest PRIVATE crosspoint_test_common ArduinoJson GTest::gtest_main)
gtest_discover_tests(SleepImageNormalizerTest)
