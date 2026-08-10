add_executable(NetworkStoreTest
  NetworkStoreTest.cpp
  ${REPO_ROOT}/lib/KOReaderSync/KOReaderCredentialStore.cpp
  ${REPO_ROOT}/src/OpdsServerStore.cpp
  ${REPO_ROOT}/src/WifiCredentialStore.cpp
  ${REPO_ROOT}/lib/Serialization/PersistableStore.cpp
  ${REPO_ROOT}/lib/Serialization/AtomicFile.cpp
  ${REPO_ROOT}/lib/Serialization/AtomicJsonFile.cpp
)

target_include_directories(NetworkStoreTest PRIVATE
  ${REPO_ROOT}/test/network_store/stubs
  ${REPO_ROOT}/test/epub_source_identity/stubs
  ${REPO_ROOT}/simulator/src
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/KOReaderSync
  ${REPO_ROOT}/src
)

target_compile_definitions(NetworkStoreTest PRIVATE ARDUINOJSON_ENABLE_ARDUINO_STRING=1)

target_link_libraries(NetworkStoreTest PRIVATE
  crosspoint_test_common
  ArduinoJson
  GTest::gtest_main
)

gtest_discover_tests(NetworkStoreTest)
