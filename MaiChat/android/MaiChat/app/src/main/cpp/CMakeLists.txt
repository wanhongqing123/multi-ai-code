cmake_minimum_required(VERSION 3.22)
project(MaiChatAndroidAgent LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_subdirectory(../../../../../../MobileAgentAdapter mobile)
add_library(maichat_agent SHARED MaiAgentJni.cpp MaiGraphicsViewJni.cpp
  media/MaiImageDecodeJni.cpp media/MaiFfplayJni.cpp)
target_include_directories(maichat_agent PRIVATE
  ../../../../../../Media/FfplayCompat)
target_link_libraries(maichat_agent PRIVATE maichat_mobile maichat_ffplay_hosted
  jnigraphics android log)
if(TARGET maiagent_obs_gles)
  add_dependencies(maichat_agent maiagent_obs_gles)
  target_link_libraries(maichat_agent PRIVATE maiagent_graphics)
endif()
