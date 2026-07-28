cmake_minimum_required(VERSION 3.21)
# C 用于 third_party/md4c（Markdown 解析，纯 C 实现）。
project(MaiChatDesktop VERSION 0.1.4 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

option(MAICHAT_USE_FAKE_CLIENT "Use the fake IM client for desktop UI development" OFF)
# 远程桌面依赖腾讯 TRTC C++ SDK（vendor/tencent-trtc）。仅 Windows 已 vendoring；
# 其它平台先关掉，编译出的应用不含远程桌面能力但其余功能不受影响。
option(MAICHAT_ENABLE_REMOTE_DESKTOP "Build remote desktop support (requires TRTC SDK)" ON)

set(MAICHAT_TRTC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/vendor/tencent-trtc/windows")
if(MAICHAT_ENABLE_REMOTE_DESKTOP AND WIN32 AND EXISTS "${MAICHAT_TRTC_ROOT}/lib/x64/liteav.lib")
  set(MAICHAT_TRTC_AVAILABLE ON)
else()
  set(MAICHAT_TRTC_AVAILABLE OFF)
  if(MAICHAT_ENABLE_REMOTE_DESKTOP AND WIN32)
    message(WARNING "TRTC SDK not found under ${MAICHAT_TRTC_ROOT}; remote desktop will be disabled")
  endif()
endif()

find_package(Qt5 REQUIRED COMPONENTS Core Network Sql Widgets Test)
find_package(ZLIB REQUIRED)

# md4c：CommonMark + GFM 扩展的 Markdown 解析器（与 Qt 内嵌同源，MIT）。
# 来源 https://github.com/mity/md4c release-0.5.2，见 third_party/md4c/LICENSE.md。
add_library(md4c STATIC
  third_party/md4c/md4c.c
  third_party/md4c/md4c.h
  third_party/md4c/md4c-html.c
  third_party/md4c/md4c-html.h
  third_party/md4c/entity.c
  third_party/md4c/entity.h
)
target_include_directories(md4c PUBLIC third_party/md4c)

add_library(remote_im_core
  src/model/RemoteIMContact.h
  src/model/RemoteIMMessage.h
  src/model/ChatState.h
  src/model/ChatState.cpp
  src/markdown/MarkdownRenderer.h
  src/markdown/MarkdownRenderer.cpp
  src/im/RemoteIMClient.h
  src/im/RemoteIMCredentialDefaults.h
  src/im/TencentUserSigGenerator.h
  src/im/TencentUserSigGenerator.cpp
  src/im/TimSdkApi.h
  src/im/DynamicTimSdkApi.h
  src/im/DynamicTimSdkApi.cpp
  src/im/TimSdkRemoteIMClient.h
  src/im/TimSdkRemoteIMClient.cpp
  src/im/FakeRemoteIMClient.h
  src/im/FakeRemoteIMClient.cpp
  src/storage/LocalMessageDatabase.h
  src/storage/LocalMessageDatabase.cpp
  src/storage/LocalSettingsStore.h
  src/storage/LocalSettingsStore.cpp
  src/storage/RemoteIMMediaStore.h
  src/storage/RemoteIMMediaStore.cpp
  src/remote/RemoteDesktopSignal.h
  src/remote/RemoteDesktopSignal.cpp
  src/remote/RemoteDesktopAuth.h
  src/remote/RemoteDesktopAuth.cpp
  src/remote/RemoteDesktopSession.h
  src/remote/RemoteDesktopSession.cpp
  src/remote/RemoteDesktopSettings.h
  src/remote/RemoteDesktopSettings.cpp
  src/remote/RemoteDesktopController.h
  src/remote/RemoteDesktopController.cpp
  src/remote/TrtcEngine.h
  src/remote/TrtcEngine.cpp
)
target_include_directories(remote_im_core PUBLIC src)
target_link_libraries(remote_im_core PUBLIC Qt5::Core Qt5::Network Qt5::Sql ZLIB::ZLIB PRIVATE md4c)
if(MAICHAT_TRTC_AVAILABLE)
  target_include_directories(remote_im_core PRIVATE
    "${MAICHAT_TRTC_ROOT}/include"
    "${MAICHAT_TRTC_ROOT}/include/TRTC")
  target_link_libraries(remote_im_core PRIVATE "${MAICHAT_TRTC_ROOT}/lib/x64/liteav.lib")
  target_compile_definitions(remote_im_core PRIVATE MAICHAT_HAVE_TRTC=1)
endif()

add_library(remote_im_desktop_app
  src/app/RemoteIMApplication.h
  src/app/RemoteIMApplication.cpp
)
target_include_directories(remote_im_desktop_app PUBLIC src)
target_link_libraries(remote_im_desktop_app PUBLIC remote_im_core)

add_library(remote_im_desktop_ui
  resources/resources.qrc
  src/ui/AddContactDialog.h
  src/ui/AddContactDialog.cpp
  src/ui/LoginDialog.h
  src/ui/LoginDialog.cpp
  src/ui/MainWindow.h
  src/ui/MainWindow.cpp
  src/ui/ImagePreviewDialog.h
  src/ui/ImagePreviewDialog.cpp
  src/ui/RemoteDesktopConsentDialog.h
  src/ui/RemoteDesktopConsentDialog.cpp
  src/ui/SharingIndicatorBar.h
  src/ui/SharingIndicatorBar.cpp
  src/ui/RemoteDesktopViewerDialog.h
  src/ui/RemoteDesktopViewerDialog.cpp
  src/ui/UiZoom.h
  src/ui/UiZoom.cpp
)
target_include_directories(remote_im_desktop_ui PUBLIC src)
target_link_libraries(remote_im_desktop_ui PUBLIC remote_im_desktop_app remote_im_core Qt5::Widgets)

add_executable(maichat
  src/main.cpp
  src/platform/DesktopRemoteIMClientFactory.h
  src/platform/DesktopRemoteIMClientFactory.cpp
  src/platform/UnsupportedRemoteIMClient.h
  src/platform/UnsupportedRemoteIMClient.cpp
  src/platform/MacRemoteIMClient.h
  src/platform/MacRemoteIMClient.cpp
  src/platform/WindowsRemoteIMClient.h
  src/platform/WindowsRemoteIMClient.cpp
)
if(WIN32)
  # GUI 子系统（/SUBSYSTEM:WINDOWS）：不加时 exe 是控制台程序，双击启动会先弹一个
  # 黑色控制台窗口再出界面。Qt5::Core 会对 WIN32_EXECUTABLE 目标自动链接
  # Qt5::WinMain（qtmain.lib 提供 WinMain → main 转发），main() 无需改动。
  set_target_properties(maichat PROPERTIES WIN32_EXECUTABLE TRUE)
  # 应用图标（任务栏/资源管理器/快捷方式）。
  enable_language(RC)
  target_sources(maichat PRIVATE resources/windows/app.rc)
endif()
if(APPLE)
  set(APP_ICON_MACOS "${CMAKE_CURRENT_SOURCE_DIR}/resources/macos/AppIcon.icns")
  set_source_files_properties("${APP_ICON_MACOS}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
  target_sources(maichat PRIVATE "${APP_ICON_MACOS}")
  set_target_properties(maichat PROPERTIES
    MACOSX_BUNDLE TRUE
    OUTPUT_NAME "MaiChat"
    MACOSX_BUNDLE_BUNDLE_NAME "MaiChat"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.kongshang.maichat"
    MACOSX_BUNDLE_ICON_FILE "AppIcon.icns"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
  )
endif()
target_link_libraries(maichat PRIVATE remote_im_desktop_ui remote_im_desktop_app remote_im_core Qt5::Widgets)
if(MAICHAT_USE_FAKE_CLIENT)
  target_compile_definitions(maichat PRIVATE MAICHAT_USE_FAKE_CLIENT=1)
endif()

add_executable(chat_state_test tests/ChatStateTest.cpp)
target_link_libraries(chat_state_test PRIVATE remote_im_core Qt5::Test)

add_executable(fake_remote_im_client_test tests/FakeRemoteIMClientTest.cpp)
target_link_libraries(fake_remote_im_client_test PRIVATE remote_im_core Qt5::Test)

add_executable(tencent_user_sig_generator_test tests/TencentUserSigGeneratorTest.cpp)
target_link_libraries(tencent_user_sig_generator_test PRIVATE remote_im_core Qt5::Test ZLIB::ZLIB)

add_executable(tim_sdk_remote_im_client_test tests/TimSdkRemoteIMClientTest.cpp)
target_link_libraries(tim_sdk_remote_im_client_test PRIVATE remote_im_core Qt5::Network Qt5::Test)

add_executable(markdown_renderer_test tests/MarkdownRendererTest.cpp)
target_link_libraries(markdown_renderer_test PRIVATE remote_im_core Qt5::Test)

add_executable(local_message_database_test tests/LocalMessageDatabaseTest.cpp)
target_link_libraries(local_message_database_test PRIVATE remote_im_core Qt5::Test)

add_executable(remote_im_application_test tests/RemoteIMApplicationTest.cpp)
target_link_libraries(remote_im_application_test PRIVATE remote_im_desktop_app remote_im_core Qt5::Test)

add_executable(login_dialog_test tests/LoginDialogTest.cpp)
target_link_libraries(login_dialog_test PRIVATE remote_im_desktop_ui remote_im_desktop_app remote_im_core Qt5::Widgets Qt5::Test)

add_executable(add_contact_dialog_test tests/AddContactDialogTest.cpp)
target_link_libraries(add_contact_dialog_test PRIVATE remote_im_desktop_ui remote_im_desktop_app remote_im_core Qt5::Widgets Qt5::Test)

add_executable(main_window_layout_test tests/MainWindowLayoutTest.cpp)
target_link_libraries(main_window_layout_test PRIVATE remote_im_desktop_ui remote_im_desktop_app remote_im_core Qt5::Widgets Qt5::Test)

add_executable(remote_desktop_signal_test tests/RemoteDesktopSignalTest.cpp)
target_link_libraries(remote_desktop_signal_test PRIVATE remote_im_core Qt5::Test)

add_executable(remote_desktop_auth_test tests/RemoteDesktopAuthTest.cpp)
target_link_libraries(remote_desktop_auth_test PRIVATE remote_im_core Qt5::Test)

add_executable(remote_desktop_session_test tests/RemoteDesktopSessionTest.cpp)
target_link_libraries(remote_desktop_session_test PRIVATE remote_im_core Qt5::Test)

add_executable(remote_desktop_settings_test tests/RemoteDesktopSettingsTest.cpp)
target_link_libraries(remote_desktop_settings_test PRIVATE remote_im_core Qt5::Test)

add_executable(remote_desktop_controller_test tests/RemoteDesktopControllerTest.cpp)
target_link_libraries(remote_desktop_controller_test PRIVATE remote_im_core Qt5::Test)

add_executable(trtc_engine_test tests/TrtcEngineTest.cpp)
target_link_libraries(trtc_engine_test PRIVATE remote_im_core Qt5::Test)

if(MAICHAT_TRTC_AVAILABLE)
  # TRTC 运行时 DLL 必须与 exe 同目录才能加载。构建后自动拷贝，
  # 避免"链接通过但一运行就找不到 SDK"这类只在本地复现的问题。
  file(GLOB MAICHAT_TRTC_RUNTIME_DLLS "${MAICHAT_TRTC_ROOT}/lib/x64/*.dll")
  foreach(target maichat trtc_engine_test)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              ${MAICHAT_TRTC_RUNTIME_DLLS} "$<TARGET_FILE_DIR:${target}>"
      COMMENT "Copying TRTC runtime DLLs next to ${target}")
  endforeach()
endif()

add_executable(remote_desktop_ui_test tests/RemoteDesktopUiTest.cpp)
target_link_libraries(remote_desktop_ui_test PRIVATE remote_im_desktop_ui remote_im_core Qt5::Widgets Qt5::Test)

add_executable(ui_zoom_test tests/UiZoomTest.cpp)
target_link_libraries(ui_zoom_test PRIVATE remote_im_desktop_ui remote_im_desktop_app remote_im_core Qt5::Widgets Qt5::Test)

enable_testing()
add_test(NAME chat_state_test COMMAND chat_state_test)
add_test(NAME fake_remote_im_client_test COMMAND fake_remote_im_client_test)
add_test(NAME tencent_user_sig_generator_test COMMAND tencent_user_sig_generator_test)
add_test(NAME tim_sdk_remote_im_client_test COMMAND tim_sdk_remote_im_client_test)
add_test(NAME markdown_renderer_test COMMAND markdown_renderer_test)
add_test(NAME local_message_database_test COMMAND local_message_database_test)
add_test(NAME remote_im_application_test COMMAND remote_im_application_test)
add_test(NAME login_dialog_test COMMAND login_dialog_test)
add_test(NAME add_contact_dialog_test COMMAND add_contact_dialog_test)
add_test(NAME main_window_layout_test COMMAND main_window_layout_test)
add_test(NAME remote_desktop_signal_test COMMAND remote_desktop_signal_test)
add_test(NAME remote_desktop_auth_test COMMAND remote_desktop_auth_test)
add_test(NAME remote_desktop_session_test COMMAND remote_desktop_session_test)
add_test(NAME remote_desktop_settings_test COMMAND remote_desktop_settings_test)
add_test(NAME remote_desktop_controller_test COMMAND remote_desktop_controller_test)
add_test(NAME trtc_engine_test COMMAND trtc_engine_test)
add_test(NAME remote_desktop_ui_test COMMAND remote_desktop_ui_test)
add_test(NAME ui_zoom_test COMMAND ui_zoom_test)
