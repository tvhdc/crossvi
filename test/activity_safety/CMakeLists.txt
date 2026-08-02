add_executable(ActivitySafetyTest ActivitySafetyTest.cpp)

target_include_directories(ActivitySafetyTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/activities
)

target_link_libraries(ActivitySafetyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ActivitySafetyTest)
