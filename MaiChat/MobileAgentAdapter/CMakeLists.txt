cmake_minimum_required(VERSION 3.22)
project(MaiChatMobileAgent LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(MAIAGENT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS" OR ANDROID)
  set(MAIAGENT_BUILD_GRAPHICS ON CACHE BOOL "" FORCE)
  set(MAIAGENT_GRAPHICS_FFMPEG_INCLUDE_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/vendor/ffmpeg-build/install/include")
endif()
if(ANDROID)
  set(mbed_archive "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls-3.6.7.tar.bz2")
  file(SHA256 "${mbed_archive}" mbed_hash)
  if(NOT mbed_hash STREQUAL "a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6")
    message(FATAL_ERROR "Mbed TLS source checksum mismatch")
  endif()
  set(mbed_source "${CMAKE_CURRENT_BINARY_DIR}/vendor/mbedtls-3.6.7")
  if(NOT EXISTS "${mbed_source}/CMakeLists.txt")
    file(ARCHIVE_EXTRACT INPUT "${mbed_archive}" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/vendor")
  endif()
  set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
  set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
  add_compile_definitions(MBEDTLS_THREADING_C MBEDTLS_THREADING_PTHREAD)
  add_subdirectory("${mbed_source}" mbedtls EXCLUDE_FROM_ALL)
  export(TARGETS mbedtls mbedx509 mbedcrypto everest p256m FILE "${CMAKE_CURRENT_BINARY_DIR}/MbedTargets.cmake")
  set(MBEDTLS_INCLUDE_DIR "${mbed_source}/include" CACHE PATH "" FORCE)
  set(MBEDTLS_LIBRARY mbedtls CACHE STRING "" FORCE)
  set(MBEDX509_LIBRARY mbedx509 CACHE STRING "" FORCE)
  set(MBEDCRYPTO_LIBRARY mbedcrypto CACHE STRING "" FORCE)
endif()
add_subdirectory(../../MaiAgent maiagent EXCLUDE_FROM_ALL)
set(ffmpeg_source "${CMAKE_CURRENT_SOURCE_DIR}/../../MaiAgent/third_party/ffmpeg")
if(NOT EXISTS "${ffmpeg_source}/configure")
  message(FATAL_ERROR "Vendored FFmpeg source is missing")
endif()
set(ffmpeg_build "${CMAKE_CURRENT_BINARY_DIR}/vendor/ffmpeg-build")
set(ffmpeg_archives
  "${ffmpeg_build}/libmaifftools.a"
  "${ffmpeg_build}/libavdevice/libavdevice.a"
  "${ffmpeg_build}/libavfilter/libavfilter.a"
  "${ffmpeg_build}/libavformat/libavformat.a"
  "${ffmpeg_build}/libavcodec/libavcodec.a"
  "${ffmpeg_build}/libswresample/libswresample.a"
  "${ffmpeg_build}/libswscale/libswscale.a"
  "${ffmpeg_build}/libavutil/libavutil.a")
get_filename_component(ffmpeg_bin "${CMAKE_C_COMPILER}" DIRECTORY)
if(ANDROID)
  set(ffmpeg_platform Android)
  if(ANDROID_ABI STREQUAL "arm64-v8a")
    set(ffmpeg_arch aarch64)
    set(ffmpeg_triple aarch64-linux-android26)
  elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
    set(ffmpeg_arch arm)
    set(ffmpeg_triple armv7a-linux-androideabi26)
  else()
    message(FATAL_ERROR "Unsupported Android ABI for FFmpeg: ${ANDROID_ABI}")
  endif()
  set(ffmpeg_cc "${ffmpeg_bin}/${ffmpeg_triple}-clang")
  set(ffmpeg_ar "${ffmpeg_bin}/llvm-ar")
  set(ffmpeg_ranlib "${ffmpeg_bin}/llvm-ranlib")
  get_filename_component(ffmpeg_toolchain "${ffmpeg_bin}" DIRECTORY)
  set(ffmpeg_sysroot "${ffmpeg_toolchain}/sysroot")
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set(ffmpeg_platform iOS)
  set(ffmpeg_arch "${CMAKE_OSX_ARCHITECTURES}")
  if(NOT ffmpeg_arch MATCHES "^(arm64|x86_64)$")
    message(FATAL_ERROR "Build one iOS architecture at a time for FFmpeg: ${ffmpeg_arch}")
  endif()
  set(ffmpeg_cc "${CMAKE_C_COMPILER}")
  set(ffmpeg_ar "${CMAKE_AR}")
  set(ffmpeg_ranlib "${CMAKE_RANLIB}")
  set(ffmpeg_sysroot "${CMAKE_OSX_SYSROOT}")
elseif(APPLE)
  set(ffmpeg_platform Darwin)
  set(ffmpeg_arch "${CMAKE_SYSTEM_PROCESSOR}")
  set(ffmpeg_cc "${CMAKE_C_COMPILER}")
  set(ffmpeg_ar "${CMAKE_AR}")
  set(ffmpeg_ranlib "${CMAKE_RANLIB}")
  set(ffmpeg_sysroot "${CMAKE_OSX_SYSROOT}")
else()
  message(FATAL_ERROR "FFmpeg mobile image filters require Apple or Android")
endif()
set(ffmpeg_script "${CMAKE_CURRENT_SOURCE_DIR}/scripts/build-ffmpeg.sh")
add_custom_command(OUTPUT ${ffmpeg_archives}
  COMMAND bash "${ffmpeg_script}" "${ffmpeg_source}" "${ffmpeg_build}"
    "${ffmpeg_platform}" "${ffmpeg_cc}" "${ffmpeg_arch}" "${ffmpeg_sysroot}"
    "${ffmpeg_ar}" "${ffmpeg_ranlib}"
  DEPENDS "${ffmpeg_source}/configure" "${ffmpeg_source}/RELEASE" "${ffmpeg_script}"
          "${ffmpeg_source}/fftools/Makefile" "${ffmpeg_source}/fftools/ffmpeg.c"
          "${ffmpeg_source}/fftools/ffprobe.c" "${ffmpeg_source}/fftools/cmdutils.c"
          "${ffmpeg_source}/fftools/opt_common.c"
          "${ffmpeg_source}/fftools/mai_fftools_embed.h"
  VERBATIM)
add_custom_target(maichat_ffmpeg_build DEPENDS ${ffmpeg_archives})
add_library(maichat_ffmpeg_tools STATIC IMPORTED GLOBAL)
set_target_properties(maichat_ffmpeg_tools PROPERTIES
  IMPORTED_LOCATION "${ffmpeg_build}/libmaifftools.a")
add_dependencies(maichat_ffmpeg_tools maichat_ffmpeg_build)
foreach(library avdevice avfilter avformat avcodec swresample swscale avutil)
  add_library(maichat_ffmpeg_${library} STATIC IMPORTED GLOBAL)
  set_target_properties(maichat_ffmpeg_${library} PROPERTIES
    IMPORTED_LOCATION "${ffmpeg_build}/lib${library}/lib${library}.a")
  add_dependencies(maichat_ffmpeg_${library} maichat_ffmpeg_build)
endforeach()
if(TARGET maiagent_graphics)
  add_dependencies(maiagent_obs_graphics_compile maichat_ffmpeg_build)
  target_link_libraries(maiagent_graphics PRIVATE
    maichat_ffmpeg_avformat maichat_ffmpeg_avcodec
    maichat_ffmpeg_swresample maichat_ffmpeg_swscale
    maichat_ffmpeg_avutil)
endif()
add_library(maichat_mobile STATIC
  MaiMobileAgent.cpp
  ../Media/MaiGraphicsPresenter.cpp
  ../Media/MaiVideoPlayback.cpp
  ../Media/MaiVideoColorParameters.cpp
  ../MaiChatTools/MaiImageFilter.cpp
  ../MaiChatTools/MaiMobileHostTools.cpp)
if(ANDROID)
  target_sources(maichat_mobile PRIVATE
    ../android/MaiChat/app/src/main/cpp/MaiChatTools/MaiMobilePhotoAlbumTool.cpp)
elseif(APPLE)
  target_sources(maichat_mobile PRIVATE
    ../ios/MaiChat/MaiChat/MaiChatTools/MaiMobilePhotoAlbumTool.cpp)
endif()
add_dependencies(maichat_mobile maichat_ffmpeg_build)
target_include_directories(maichat_mobile PRIVATE "${ffmpeg_build}/install/include")
target_include_directories(maichat_mobile PRIVATE
  "${ffmpeg_source}/fftools")
target_include_directories(maichat_mobile PUBLIC . ../MaiChatTools ../Media
  PRIVATE ../../MaiAgent/third_party/nlohmann)
target_link_libraries(maichat_mobile PRIVATE maiagent
  maichat_ffmpeg_tools
  maichat_ffmpeg_avdevice maichat_ffmpeg_avfilter maichat_ffmpeg_avformat
  maichat_ffmpeg_avcodec maichat_ffmpeg_swresample maichat_ffmpeg_swscale
  maichat_ffmpeg_avutil)
if(TARGET maiagent_graphics)
  target_link_libraries(maichat_mobile PRIVATE maiagent_graphics)
endif()
if(ANDROID)
  target_link_libraries(maichat_mobile PRIVATE camera2ndk mediandk)
endif()
option(MAICHAT_MOBILE_TESTS "Build mobile adapter integration tests" OFF)
if(MAICHAT_MOBILE_TESTS)
  enable_testing()
  add_executable(MaiMobileAgentTests tests/MaiMobileAgentTests.cpp)
  target_include_directories(MaiMobileAgentTests PRIVATE ../../MaiAgent/third_party/httplib ../../MaiAgent/third_party/nlohmann)
  target_link_libraries(MaiMobileAgentTests PRIVATE maichat_mobile maiagent)
  add_test(NAME MaiMobileAgentTests COMMAND MaiMobileAgentTests)
  if(ANDROID)
    add_executable(MaiEmbeddedFfmpegToolsTest
      ../desktop/tests/MaiEmbeddedFfmpegToolsTest.cpp)
    target_include_directories(MaiEmbeddedFfmpegToolsTest PRIVATE
      ../../MaiAgent/third_party/ffmpeg/fftools
      ../../MaiAgent/third_party/nlohmann)
    target_link_libraries(MaiEmbeddedFfmpegToolsTest PRIVATE
      maichat_mobile maiagent android log jnigraphics)
  endif()
  if(CMAKE_SYSTEM_NAME STREQUAL "iOS" AND CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
    add_executable(MaiObsIosMetalRenderProbe
      ../Media/tests/MaiObsIosMetalRenderProbe.c
      ../Media/tests/MaiObsIosMetalRenderProbe.swift)
    target_link_libraries(MaiObsIosMetalRenderProbe PRIVATE maichat_mobile)
  endif()
endif()
