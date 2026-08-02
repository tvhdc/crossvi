add_executable(MappedInputManagerTest
  MappedInputManagerTest.cpp
  ${REPO_ROOT}/src/MappedInputManager.cpp
)

target_include_directories(MappedInputManagerTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/EpdFont
)
target_link_libraries(MappedInputManagerTest PRIVATE crosspoint_test_common GTest::gtest_main)

gtest_discover_tests(MappedInputManagerTest)
