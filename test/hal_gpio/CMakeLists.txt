add_executable(HalGPIOTest
  HalGPIOTest.cpp
  ${REPO_ROOT}/lib/hal/HalGPIO.cpp
)

target_compile_definitions(HalGPIOTest PRIVATE CROSSPOINT_EMULATED=0)
target_include_directories(HalGPIOTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/hal
)
target_link_libraries(HalGPIOTest PRIVATE
  GTest::gtest_main
  crosspoint_test_common
)

gtest_discover_tests(HalGPIOTest)
