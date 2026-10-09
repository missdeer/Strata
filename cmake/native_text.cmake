# Included only by explicit native cluster/text builds. The text layer has no
# network/expert/GPU dependency and can also be built for the SYCL host.
function(strata_add_native_text llama_root)
  get_filename_component(_text_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
  include(FetchContent)
  set(STRATA_JSONCONS_DIR "" CACHE PATH "jsoncons v1.7.0 source for offline native text builds")
  if(STRATA_JSONCONS_DIR)
    set(_jsoncons "${STRATA_JSONCONS_DIR}")
  else()
    FetchContent_Declare(strata_jsoncons
      URL https://codeload.github.com/danielaparker/jsoncons/zip/refs/tags/v1.7.0
      URL_HASH SHA256=63912bce2dbfd40d8cb1920adeaa4c1f94302fec2969f16d54ee6c9a8808c42c)
    FetchContent_GetProperties(strata_jsoncons)
    if(NOT strata_jsoncons_POPULATED)
      FetchContent_Populate(strata_jsoncons)
    endif()
    set(_jsoncons "${strata_jsoncons_SOURCE_DIR}")
  endif()
  if(NOT EXISTS "${llama_root}/common/jinja/runtime.cpp" OR NOT EXISTS "${_jsoncons}/include/jsoncons_ext/jsonschema/jsonschema.hpp")
    message(FATAL_ERROR "Native text needs the pinned llama.cpp Jinja implementation and jsoncons v1.7.0")
  endif()
  file(STRINGS "${_jsoncons}/include/jsoncons/config/version.hpp" _jsoncons_version
    REGEX "^#define JSONCONS_VERSION_(MAJOR|MINOR|PATCH) [0-9]+$")
  if(NOT "${_jsoncons_version}" STREQUAL "#define JSONCONS_VERSION_MAJOR 1;#define JSONCONS_VERSION_MINOR 7;#define JSONCONS_VERSION_PATCH 0")
    message(FATAL_ERROR "Native text requires jsoncons v1.7.0")
  endif()
  configure_file("${_jsoncons}/LICENSE" "${CMAKE_BINARY_DIR}/native-text-licenses/jsoncons-LICENSE.txt" COPYONLY)
  configure_file("${llama_root}/LICENSE" "${CMAKE_BINARY_DIR}/native-text-licenses/llama.cpp-LICENSE.txt" COPYONLY)
  add_library(strata_text_vendor STATIC
    "${llama_root}/src/unicode.cpp" "${llama_root}/src/unicode-data.cpp"
    "${llama_root}/common/unicode.cpp" "${llama_root}/common/json.cpp"
    "${llama_root}/common/jinja/lexer.cpp" "${llama_root}/common/jinja/parser.cpp"
    "${llama_root}/common/jinja/runtime.cpp" "${llama_root}/common/jinja/string.cpp"
    "${llama_root}/common/jinja/value.cpp")
  target_include_directories(strata_text_vendor SYSTEM PUBLIC "${llama_root}/common" "${llama_root}/vendor")
  target_link_libraries(strata_text_vendor PRIVATE ggml-base)
  add_library(strata_text_runtime STATIC
    "${_text_root}/src/runtime/tokenizer.cpp" "${_text_root}/src/runtime/chat_template.cpp"
    "${_text_root}/src/runtime/text_requests.cpp" "${_text_root}/src/runtime/text_output.cpp"
    "${_text_root}/src/runtime/text_structured.cpp")
  # tokenizer uses src/unicode.h; the Jinja interpreter uses common/unicode.h.
  set_source_files_properties("${_text_root}/src/runtime/tokenizer.cpp" PROPERTIES INCLUDE_DIRECTORIES "${llama_root}/src")
  target_include_directories(strata_text_runtime PUBLIC "${_text_root}/include")
  target_include_directories(strata_text_runtime SYSTEM PRIVATE "${_jsoncons}/include")
  target_link_libraries(strata_text_runtime PUBLIC strata_text_vendor)
  if(TARGET strata_warnings)
    target_link_libraries(strata_text_runtime PRIVATE strata_warnings)
  endif()
  add_executable(strata-text-check "${_text_root}/src/program/text_check.cpp")
  target_link_libraries(strata-text-check PRIVATE strata_text_runtime)
  if(STRATA_BUILD_TESTS)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
      enable_testing()
      add_test(NAME native_text_parity COMMAND "${Python3_EXECUTABLE}" "${_text_root}/tools/test_native_text.py"
        --exe $<TARGET_FILE:strata-text-check>)
      set_tests_properties(native_text_parity PROPERTIES TIMEOUT 300)
    endif()
  endif()
endfunction()
