add_executable(SleepFrameStoreTest
  SleepFrameStoreTest.cpp
  SleepImageValidationStub.cpp
  ${REPO_ROOT}/src/activities/boot_sleep/SleepFrameStore.cpp
  ${REPO_ROOT}/src/activities/boot_sleep/SleepImageSelectionStore.cpp
  ${REPO_ROOT}/lib/Serialization/AtomicFile.cpp
  ${REPO_ROOT}/lib/Serialization/StagedFileTransaction.cpp
)

target_include_directories(SleepFrameStoreTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${CMAKE_CURRENT_SOURCE_DIR}/../epub_source_identity/stubs
  ${REPO_ROOT}/src/activities/boot_sleep
  ${REPO_ROOT}/lib/Serialization
)

target_link_libraries(SleepFrameStoreTest PRIVATE crosspoint_test_common ArduinoJson GTest::gtest_main)
gtest_discover_tests(SleepFrameStoreTest)
