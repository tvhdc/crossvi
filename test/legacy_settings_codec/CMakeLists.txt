# Build an exact configure-time copy so JsonSettingsIO's quoted includes resolve
# to the bounded host stubs instead of pulling the device Settings UI graph.
configure_file(
  ${REPO_ROOT}/src/JsonSettingsIO.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/JsonSettingsIO.production.cpp
  COPYONLY
)

add_executable(LegacySettingsCodecTest
  LegacySettingsCodecTest.cpp
  LegacySettingsMigrationIntegrationTest.cpp
  TestSupport.cpp
  ${REPO_ROOT}/src/CrossPointSettings.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/JsonSettingsIO.production.cpp
  ${REPO_ROOT}/src/LegacySettingsCodec.cpp
  ${REPO_ROOT}/lib/I18n/I18nStrings.cpp
)

target_include_directories(LegacySettingsCodecTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/test/epub_source_identity/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/I18n
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Version
  ${REPO_ROOT}/simulator/src
)

target_compile_options(LegacySettingsCodecTest PRIVATE
  -include ${REPO_ROOT}/simulator/src/WString.h
)

target_link_libraries(LegacySettingsCodecTest PRIVATE
  ArduinoJson
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(LegacySettingsCodecTest)
