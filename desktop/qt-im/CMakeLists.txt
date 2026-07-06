cmake_minimum_required(VERSION 3.21)
project(MultiAIIMDesktop LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

option(MULTI_AI_IM_USE_FAKE_CLIENT "Use the fake IM client for desktop UI development" OFF)

find_package(Qt5 REQUIRED COMPONENTS Core Network Widgets Test)
find_package(ZLIB REQUIRED)

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
  src/storage/LocalChatHistoryStore.h
  src/storage/LocalChatHistoryStore.cpp
  src/storage/LocalSettingsStore.h
  src/storage/LocalSettingsStore.cpp
  src/storage/RemoteIMMediaStore.h
  src/storage/RemoteIMMediaStore.cpp
)
target_include_directories(remote_im_core PUBLIC src)
target_link_libraries(remote_im_core PUBLIC Qt5::Core Qt5::Network ZLIB::ZLIB)

add_library(remote_im_desktop_app
  src/app/RemoteIMApplication.h
  src/app/RemoteIMApplication.cpp
)
target_include_directories(remote_im_desktop_app PUBLIC src)
target_link_libraries(remote_im_desktop_app PUBLIC remote_im_core)

add_library(remote_im_desktop_ui
  src/ui/AddContactDialog.h
  src/ui/AddContactDialog.cpp
  src/ui/LoginDialog.h
  src/ui/LoginDialog.cpp
  src/ui/MainWindow.h
  src/ui/MainWindow.cpp
  src/ui/ImagePreviewDialog.h
  src/ui/ImagePreviewDialog.cpp
)
target_include_directories(remote_im_desktop_ui PUBLIC src)
target_link_libraries(remote_im_desktop_ui PUBLIC remote_im_desktop_app remote_im_core Qt5::Widgets)

add_executable(multi_ai_im_desktop
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
if(APPLE)
  set(APP_ICON_MACOS "${CMAKE_CURRENT_SOURCE_DIR}/resources/macos/AppIcon.icns")
  set_source_files_properties("${APP_ICON_MACOS}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
  target_sources(multi_ai_im_desktop PRIVATE "${APP_ICON_MACOS}")
  set_target_properties(multi_ai_im_desktop PROPERTIES
    MACOSX_BUNDLE TRUE
    OUTPUT_NAME "Multi-AI Code IM"
    MACOSX_BUNDLE_BUNDLE_NAME "Multi-AI Code IM"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.multi-ai-code.im.desktop"
    MACOSX_BUNDLE_ICON_FILE "AppIcon.icns"
  )
endif()
target_link_libraries(multi_ai_im_desktop PRIVATE remote_im_desktop_ui remote_im_desktop_app remote_im_core Qt5::Widgets)
if(MULTI_AI_IM_USE_FAKE_CLIENT)
  target_compile_definitions(multi_ai_im_desktop PRIVATE MULTI_AI_IM_USE_FAKE_CLIENT=1)
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

add_executable(local_chat_history_store_test tests/LocalChatHistoryStoreTest.cpp)
target_link_libraries(local_chat_history_store_test PRIVATE remote_im_core Qt5::Test)

add_executable(remote_im_application_test tests/RemoteIMApplicationTest.cpp)
target_link_libraries(remote_im_application_test PRIVATE remote_im_desktop_app remote_im_core Qt5::Test)

add_executable(login_dialog_test tests/LoginDialogTest.cpp)
target_link_libraries(login_dialog_test PRIVATE remote_im_desktop_ui remote_im_desktop_app remote_im_core Qt5::Widgets Qt5::Test)

add_executable(add_contact_dialog_test tests/AddContactDialogTest.cpp)
target_link_libraries(add_contact_dialog_test PRIVATE remote_im_desktop_ui remote_im_desktop_app remote_im_core Qt5::Widgets Qt5::Test)

add_executable(main_window_layout_test tests/MainWindowLayoutTest.cpp)
target_link_libraries(main_window_layout_test PRIVATE remote_im_desktop_ui remote_im_desktop_app remote_im_core Qt5::Widgets Qt5::Test)

enable_testing()
add_test(NAME chat_state_test COMMAND chat_state_test)
add_test(NAME fake_remote_im_client_test COMMAND fake_remote_im_client_test)
add_test(NAME tencent_user_sig_generator_test COMMAND tencent_user_sig_generator_test)
add_test(NAME tim_sdk_remote_im_client_test COMMAND tim_sdk_remote_im_client_test)
add_test(NAME markdown_renderer_test COMMAND markdown_renderer_test)
add_test(NAME local_chat_history_store_test COMMAND local_chat_history_store_test)
add_test(NAME remote_im_application_test COMMAND remote_im_application_test)
add_test(NAME login_dialog_test COMMAND login_dialog_test)
add_test(NAME add_contact_dialog_test COMMAND add_contact_dialog_test)
add_test(NAME main_window_layout_test COMMAND main_window_layout_test)
