# 文件作用：把 Android JNI 桥接、图形与媒体代码构建成 App 使用的原生库。
# 编译选项来自 Gradle/NDK；clangd 数据库由真实原生构建生成。
cmake_minimum_required(VERSION 3.22)
project(MaiChatAndroidAgent LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_subdirectory(../../../../../../MobileAgentAdapter mobile)
add_library(maichat_agent SHARED MaiAgentJni.cpp MaiChatAuthJni.cpp
  MaiChatAttachmentDownloadJni.cpp MaiCloudDriveJni.cpp MaiGraphicsViewJni.cpp
  MaiSessionSyncJni.cpp MaiVideoCircleJni.cpp
  media/MaiImageDecodeJni.cpp media/MaiFfplayJni.cpp
  media/TencentRealtimeSpeechWebSocketJni.cpp)
target_include_directories(maichat_agent PRIVATE
  ../../../../../../Network
  ../../../../../../Media
  ../../../../../../Media/FfplayCompat
  ../../../../../../SessionSync
  ../../../../../../../MaiAgent/include)
target_link_libraries(maichat_agent PRIVATE maichat_mobile mai_curl_download
  mai_video_circle_client mai_cloud_drive_client CURL::libcurl maichat_ffplay_hosted
  jnigraphics android log)
if(TARGET maiagent_obs_gles)
  add_dependencies(maichat_agent maiagent_obs_gles)
  target_link_libraries(maichat_agent PRIVATE maiagent_graphics)
endif()
