add_executable(ButtonNavigatorTest
  ButtonNavigatorTest.cpp
  ${REPO_ROOT}/src/util/ButtonNavigator.cpp
)

target_include_directories(ButtonNavigatorTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
)
target_link_libraries(ButtonNavigatorTest PRIVATE crosspoint_test_common GTest::gtest_main)

gtest_discover_tests(ButtonNavigatorTest)
