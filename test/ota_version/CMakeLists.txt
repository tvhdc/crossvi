add_executable(OtaVersionTest OtaVersionTest.cpp)

target_link_libraries(OtaVersionTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

target_include_directories(OtaVersionTest PRIVATE ${REPO_ROOT}/src)

gtest_discover_tests(OtaVersionTest)
