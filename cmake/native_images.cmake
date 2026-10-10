# Image codecs are linked into the opt-in native server. They are not runtime
# Python dependencies. All downloads are fixed releases checked by SHA256.
function(strata_image_source name override url digest out)
  set(${override} "" CACHE PATH "Offline source directory for ${name}")
  if(${override})
    set(_source "${${override}}")
  else()
    include(FetchContent)
    FetchContent_Declare(${name} URL "${url}" URL_HASH "SHA256=${digest}")
    FetchContent_GetProperties(${name})
    if(NOT ${name}_POPULATED)
      FetchContent_Populate(${name})
    endif()
    set(_source "${${name}_SOURCE_DIR}")
  endif()
  set(${out} "${_source}" PARENT_SCOPE)
endfunction()
function(strata_add_image_codecs)
  if(TARGET strata_image_codecs)
    return()
  endif()
  strata_image_source(strata_webp STRATA_WEBP_DIR
    https://codeload.github.com/webmproject/libwebp/zip/refs/tags/v1.6.0
    7a89bb7a6a0c30b5c0f521c2673635381b4e7c916585f4a1f702d3b47f15d5e5 _webp)
  strata_image_source(strata_avif STRATA_AVIF_DIR
    https://codeload.github.com/AOMediaCodec/libavif/zip/refs/tags/v1.4.2
    79b6ed913a5f573e6c5c85721bb3046231df48c9743a7f4c3f5146ed0c1c06d0 _avif)
  strata_image_source(strata_aom STRATA_AOM_DIR
    https://aomedia.googlesource.com/aom/+archive/refs/tags/v3.14.1.tar.gz
    0c50d3bd353bad2f13276b67b2403b9f078c8984760f685549a2634294572e0a _aom)
  file(READ "${_webp}/configure.ac" _webp_version LIMIT 128)
  file(READ "${_avif}/include/avif/avif.h" _avif_version LIMIT 6000)
  file(READ "${_aom}/CHANGELOG" _aom_version LIMIT 64)
  if(NOT _webp_version MATCHES "AC_INIT\\(\\[libwebp\\], \\[1.6.0\\]" OR
     NOT _avif_version MATCHES "#define AVIF_VERSION_MAJOR 1" OR
     NOT _avif_version MATCHES "#define AVIF_VERSION_MINOR 4" OR
     NOT _avif_version MATCHES "#define AVIF_VERSION_PATCH 2" OR
     NOT _aom_version MATCHES "v3.14.1")
    message(FATAL_ERROR "Native images require libwebp 1.6.0, libavif 1.4.2 and libaom 3.14.1")
  endif()
  set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
  set(BUILD_SHARED_LIBS OFF)
  foreach(_flag WEBP_BUILD_ANIM_UTILS WEBP_BUILD_CWEBP WEBP_BUILD_DWEBP WEBP_BUILD_GIF2WEBP
      WEBP_BUILD_IMG2WEBP WEBP_BUILD_VWEBP WEBP_BUILD_WEBPINFO WEBP_BUILD_LIBWEBPMUX WEBP_BUILD_WEBPMUX
      WEBP_BUILD_EXTRAS WEBP_BUILD_WEBP_JS WEBP_BUILD_FUZZTEST)
    set(${_flag} OFF)
  endforeach()
  # Keep the library's CPU dispatch. Disabling all SIMD also adds -mno-sse2,
  # which violates the x86-64 floating-point ABI with clang/IntelLLVM.
  set(WEBP_ENABLE_SIMD ON)
  add_subdirectory("${_webp}" "${CMAKE_BINARY_DIR}/native-images/webp" EXCLUDE_FROM_ALL)
  # Decoding WebP needs neither its encoder nor sharpened YUV conversion.
  # webpdemux's upstream link uses webp; bind the same decoder ABI directly.
  set_property(TARGET webpdemux PROPERTY LINK_LIBRARIES webpdecoder)
  set(AVIF_CODEC_AOM LOCAL)
  set(AVIF_CODEC_AOM_DECODE ON)
  set(AVIF_CODEC_AOM_ENCODE OFF)
  foreach(_codec DAV1D LIBGAV1 RAV1E SVT AVM)
    set(AVIF_CODEC_${_codec} OFF)
  endforeach()
  set(AVIF_LIBYUV OFF)
  set(AVIF_BUILD_APPS OFF)
  set(AVIF_BUILD_TESTS OFF)
  set(AVIF_ENABLE_WERROR OFF)
  set(FETCHCONTENT_SOURCE_DIR_LIBAOM "${_aom}")
  set(AOM_TARGET_CPU generic CACHE STRING "Portable native image AV1 decoder" FORCE)
  set(CONFIG_RUNTIME_CPU_DETECT 0 CACHE INTERNAL "")
  add_subdirectory("${_avif}" "${CMAKE_BINARY_DIR}/native-images/avif" EXCLUDE_FROM_ALL)
  add_library(strata_avif_exif STATIC "${_avif}/apps/shared/avifexif.c")
  target_link_libraries(strata_avif_exif PRIVATE avif)
  target_include_directories(strata_avif_exif PUBLIC "${_avif}/apps/shared")
  add_library(strata_image_codecs INTERFACE)
  target_link_libraries(strata_image_codecs INTERFACE webpdemux strata_avif_exif avif)
  foreach(_license "${_webp}/COPYING" "${_webp}/PATENTS" "${_avif}/LICENSE" "${_aom}/LICENSE" "${_aom}/PATENTS" "${_aom}/third_party/libyuv/LICENSE")
    get_filename_component(_file "${_license}" NAME)
    get_filename_component(_parent "${_license}" DIRECTORY)
    get_filename_component(_parent "${_parent}" NAME)
    configure_file("${_license}" "${CMAKE_BINARY_DIR}/native-text-licenses/${_parent}-${_file}.txt" COPYONLY)
  endforeach()
endfunction()
