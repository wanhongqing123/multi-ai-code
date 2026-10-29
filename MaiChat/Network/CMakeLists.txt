cmake_minimum_required(VERSION 3.22)
project(MaiChatNetwork LANGUAGES C CXX)

option(MAICHAT_NETWORK_BUILD_TESTS "Build local TLS downloader tests" OFF)
option(MAICHAT_SCREEN_FRAME_ARCHIVE_ENABLED "Build the screen frame archive API" OFF)

if(NOT TARGET CURL::libcurl)
  find_package(CURL REQUIRED)
endif()

add_library(mai_curl_download STATIC MaiCurlDownload.cpp)
target_compile_features(mai_curl_download PUBLIC cxx_std_17)
target_include_directories(mai_curl_download PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(mai_curl_download PUBLIC CURL::libcurl)
set_target_properties(mai_curl_download PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(mai_cloud_vod STATIC MaiCloudVod.cpp)
target_compile_features(mai_cloud_vod PUBLIC cxx_std_17)
if(WIN32)
  target_compile_definitions(mai_cloud_vod PRIVATE NOMINMAX)
endif()
target_include_directories(mai_cloud_vod PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../MaiAgent/third_party")
target_link_libraries(mai_cloud_vod PUBLIC CURL::libcurl mai_cloud_drive_client)
set_target_properties(mai_cloud_vod PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(mai_video_circle_client STATIC MaiVideoCircleClient.cpp)
target_compile_features(mai_video_circle_client PUBLIC cxx_std_17)
target_include_directories(mai_video_circle_client PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../MaiAgent/third_party")
target_link_libraries(mai_video_circle_client PUBLIC CURL::libcurl)
set_target_properties(mai_video_circle_client PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(NOT MAI_OPENSSL_CRYPTO_TARGET)
  find_package(OpenSSL REQUIRED COMPONENTS Crypto)
  set(MAI_OPENSSL_CRYPTO_TARGET OpenSSL::Crypto)
endif()
add_library(mai_cloud_drive_client STATIC MaiCloudDriveClient.cpp)
target_compile_features(mai_cloud_drive_client PUBLIC cxx_std_17)
target_include_directories(mai_cloud_drive_client PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../MaiAgent/third_party"
          "${CMAKE_CURRENT_SOURCE_DIR}/../../MaiAgent/third_party/openssl/include"
          ${MAI_OPENSSL_INCLUDE_DIR})
target_link_libraries(mai_cloud_drive_client PUBLIC CURL::libcurl
  ${MAI_OPENSSL_CRYPTO_TARGET})
set_target_properties(mai_cloud_drive_client PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(CMAKE_SYSTEM_NAME STREQUAL "iOS" OR MAICHAT_NETWORK_BUILD_TESTS OR
    MAICHAT_SCREEN_FRAME_ARCHIVE_ENABLED)
  find_package(ZLIB REQUIRED)
  add_library(mai_screen_frame_archive STATIC MaiScreenFrameArchive.cpp)
  target_compile_features(mai_screen_frame_archive PUBLIC cxx_std_17)
  target_include_directories(mai_screen_frame_archive PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../MaiAgent/third_party"
            "${CMAKE_CURRENT_SOURCE_DIR}/../../MaiAgent/third_party/openssl/include"
            ${MAI_OPENSSL_INCLUDE_DIR})
  target_link_libraries(mai_screen_frame_archive PUBLIC
    ${MAI_OPENSSL_CRYPTO_TARGET} ZLIB::ZLIB)
  set_target_properties(mai_screen_frame_archive PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

if(MAICHAT_NETWORK_BUILD_TESTS)
  enable_testing()
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_executable(mai_screen_frame_archive_test tests/MaiScreenFrameArchiveTest.cpp)
  target_include_directories(mai_screen_frame_archive_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../MaiAgent/third_party")
  target_link_libraries(mai_screen_frame_archive_test PRIVATE mai_screen_frame_archive)
  add_test(NAME mai_screen_frame_archive
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_screen_frame_archive.py"
      "$<TARGET_FILE:mai_screen_frame_archive_test>")
  set_tests_properties(mai_screen_frame_archive PROPERTIES TIMEOUT 90)
  add_executable(mai_curl_download_test tests/MaiCurlDownloadTest.cpp)
  target_link_libraries(mai_curl_download_test PRIVATE mai_curl_download)
  add_test(NAME mai_curl_download_local_tls
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_local_tls.py"
      "$<TARGET_FILE:mai_curl_download_test>")

  add_executable(mai_video_circle_client_test tests/MaiVideoCircleClientTest.cpp)
  target_link_libraries(mai_video_circle_client_test PRIVATE mai_video_circle_client)
  add_test(NAME mai_video_circle_local_tls
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_video_circle_local_tls.py"
      "$<TARGET_FILE:mai_video_circle_client_test>")

  add_executable(mai_cloud_drive_client_test tests/MaiCloudDriveClientTest.cpp)
  target_include_directories(mai_cloud_drive_client_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../MaiAgent/third_party")
  target_link_libraries(mai_cloud_drive_client_test PRIVATE mai_cloud_drive_client)
  add_test(NAME mai_cloud_drive_local_tls
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_cloud_drive_local_tls.py"
      "$<TARGET_FILE:mai_cloud_drive_client_test>")

  add_executable(mai_cloud_vod_test tests/MaiCloudVodTest.cpp)
  target_link_libraries(mai_cloud_vod_test PRIVATE mai_cloud_vod)
  add_test(NAME mai_cloud_vod_local_tls
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_cloud_vod_local_tls.py"
      "$<TARGET_FILE:mai_cloud_vod_test>")

  # Operator-run only. This target is never registered as a CTest because it
  # writes to the dedicated real OSS bucket when explicitly orchestrated.
  add_executable(mai_cloud_drive_real_oss_runner
    tests/MaiCloudDriveRealOssRunner.cpp)
  target_link_libraries(mai_cloud_drive_real_oss_runner PRIVATE mai_cloud_drive_client)
endif()
