add_executable(KOReaderSyncClientTest
  KOReaderSyncClientTest.cpp
  ${REPO_ROOT}/lib/KOReaderSync/KOReaderSyncClient.cpp
  ${REPO_ROOT}/lib/HttpTransportPolicy/HttpTransportPolicy.cpp
  ${REPO_ROOT}/simulator/src/NetworkClient.cpp
)

target_include_directories(KOReaderSyncClientTest PRIVATE
  ${REPO_ROOT}/test/koreader_sync_client/stubs
  ${REPO_ROOT}/test/network_store/stubs
  ${REPO_ROOT}/test/epub_source_identity/stubs
  ${REPO_ROOT}/simulator/src
  ${REPO_ROOT}/lib/HttpTransportPolicy
  ${REPO_ROOT}/lib/KOReaderSync
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Serialization
)

target_compile_definitions(KOReaderSyncClientTest PRIVATE ARDUINOJSON_ENABLE_ARDUINO_STRING=1)

target_link_libraries(KOReaderSyncClientTest PRIVATE
  crosspoint_test_common
  ArduinoJson
  GTest::gtest_main
)

gtest_discover_tests(KOReaderSyncClientTest)
