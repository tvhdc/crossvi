cmake_minimum_required(VERSION 3.16)
project(crosspoint_reader_tests C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.17.0
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

FetchContent_Declare(
  arduinojson
  GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
  GIT_TAG v7.4.2
)
FetchContent_MakeAvailable(arduinojson)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_library(crosspoint_test_common INTERFACE)
target_include_directories(crosspoint_test_common INTERFACE
  ${REPO_ROOT}
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Utf8
)
target_compile_options(crosspoint_test_common INTERFACE
  -Wall
  -Wextra
  -pedantic
)

enable_testing()
include(GoogleTest)

find_program(PYTHON3_EXECUTABLE NAMES python3)
if(NOT PYTHON3_EXECUTABLE)
  message(FATAL_ERROR "python3 is required for code-generation tests")
endif()

execute_process(
  COMMAND ${PYTHON3_EXECUTABLE} ${REPO_ROOT}/scripts/gen_i18n.py --strip-unused
  WORKING_DIRECTORY ${REPO_ROOT}
  RESULT_VARIABLE I18N_CODEGEN_RESULT
)
if(NOT I18N_CODEGEN_RESULT EQUAL 0)
  message(FATAL_ERROR "Failed to generate I18n headers for host tests")
endif()

add_test(NAME CodegenTest COMMAND ${PYTHON3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/codegen/CodegenTest.py)
add_test(NAME FontConverterTest COMMAND ${PYTHON3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/font_converter/FontConverterTest.py)
find_program(NODE_EXECUTABLE NAMES node)
if(NODE_EXECUTABLE)
  add_test(NAME VocabularyConverterTest COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/vocabulary/VocabularyConverterTest.js)
  add_test(NAME FlashToolTest COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/web_tools/FlashToolTest.js)
endif()

add_subdirectory(streaming_json_parser)
add_subdirectory(release_json_parser)
add_subdirectory(ota_version)
add_subdirectory(differential_rounding)
add_subdirectory(hyphenation_eval)
add_subdirectory(utf8_compose)
add_subdirectory(opds_filename)
add_subdirectory(opds_parser)
add_subdirectory(minibidi_arabic)
add_subdirectory(combining_marks)
add_subdirectory(txt_line_wrap)
add_subdirectory(progress_file_codec)
add_subdirectory(dashboard_layout)
add_subdirectory(reading_stats)
add_subdirectory(per_book_reader_settings_codec)
add_subdirectory(clippings_core)
add_subdirectory(nearby_sync_core)
add_subdirectory(book_cache_utils)
add_subdirectory(epub_source_identity)
add_subdirectory(xtc)
add_subdirectory(atomic_persistence)
add_subdirectory(reader_feature_utils)
add_subdirectory(reader_gestures)
add_subdirectory(boot_recovery)
add_subdirectory(dictionary)
add_subdirectory(font_system)
add_subdirectory(bookmark_json)
add_subdirectory(hal_gpio)
add_subdirectory(bitmap_validation)
add_subdirectory(library_catalog)
add_subdirectory(image_dims_probe)
add_subdirectory(sleep_frame_store)
add_subdirectory(png_converter_safety)
add_subdirectory(pixel_cache_integration)
add_subdirectory(network_streaming)
add_subdirectory(legacy_settings_codec)
add_subdirectory(button_navigator)
add_subdirectory(activity_safety)
add_subdirectory(mapped_input)
add_subdirectory(koreader_xpath)
add_subdirectory(koreader_sync_client)
add_subdirectory(credential_integrity)
add_subdirectory(network_store)
add_subdirectory(section_buffered_io)
add_subdirectory(vocabulary)
