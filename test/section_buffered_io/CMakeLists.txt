add_executable(SectionBufferedIoTest
  SectionBufferedIoTest.cpp
  ${REPO_ROOT}/simulator/src/HalStorage.cpp
)

target_include_directories(SectionBufferedIoTest BEFORE PRIVATE
  ${REPO_ROOT}/simulator/src
  ${REPO_ROOT}/lib/Epub/Epub
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Serialization
)

target_link_libraries(SectionBufferedIoTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SectionBufferedIoTest)
