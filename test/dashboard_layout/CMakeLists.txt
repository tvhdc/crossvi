add_executable(HomeLayoutTest
  BlockStyleTest.cpp
  CrossViLayoutTest.cpp
  HomeMenuMappingTest.cpp
  SectionCacheLayoutTest.cpp
  SectionCacheValidatorTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/SectionCacheValidator.cpp
)

target_include_directories(HomeLayoutTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(HomeLayoutTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(HomeLayoutTest)
