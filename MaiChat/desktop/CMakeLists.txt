cmake_minimum_required(VERSION 3.21)
# C 用于 third_party/md4c（Markdown 解析，纯 C 实现）。
project(MaiChatDesktop VERSION 0.1.87 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

option(MAICHAT_USE_FAKE_CLIENT "Use the fake IM client for desktop UI development" OFF)
# 远程桌面依赖腾讯 TRTC C++ SDK（vendor/tencent-trtc）。
option(MAICHAT_ENABLE_REMOTE_DESKTOP "Build remote desktop support (requires TRTC SDK)" ON)

set(MAICHAT_TRTC_AVAILABLE OFF)
if(MAICHAT_ENABLE_REMOTE_DESKTOP)
  if(WIN32)
    set(MAICHAT_TRTC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/vendor/tencent-trtc/windows")
    if(EXISTS "${MAICHAT_TRTC_ROOT}/lib/x64/liteav.lib")
      set(MAICHAT_TRTC_AVAILABLE ON)
    endif()
  elseif(APPLE)
    set(MAICHAT_TRTC_ROOT
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/tencent-trtc/macos/TXLiteAVSDK_TRTC_Mac")
    set(MAICHAT_TRTC_MAC_SLICE "macos-arm64_x86_64")
    set(MAICHAT_TRTC_MAC_FRAMEWORKS
        "${MAICHAT_TRTC_ROOT}/TXLiteAVSDK_TRTC_Mac.xcframework/${MAICHAT_TRTC_MAC_SLICE}/TXLiteAVSDK_TRTC_Mac.framework"
        "${MAICHAT_TRTC_ROOT}/TXFFmpeg.xcframework/${MAICHAT_TRTC_MAC_SLICE}/TXFFmpeg.framework"
        "${MAICHAT_TRTC_ROOT}/TXSoundTouch.xcframework/${MAICHAT_TRTC_MAC_SLICE}/TXSoundTouch.framework")
    set(MAICHAT_TRTC_AVAILABLE ON)
    foreach(framework ${MAICHAT_TRTC_MAC_FRAMEWORKS})
      if(NOT EXISTS "${framework}")
        set(MAICHAT_TRTC_AVAILABLE OFF)
      endif()
    endforeach()
  endif()

  if(NOT MAICHAT_TRTC_AVAILABLE AND (WIN32 OR APPLE))
    message(WARNING "TRTC SDK not found under ${MAICHAT_TRTC_ROOT}; remote desktop will be disabled")
  endif()
endif()

# Multimedia/MultimediaWidgets：视频消息的播放窗（QMediaPlayer + QVideoWidget）。
# 注意 QMediaPlayer 的解码后端是插件（plugins/mediaservice 下的 wmfengine/dsengine），
# 链接通过不代表能播——打包脚本会校验插件是否随包部署。
find_package(Qt5 REQUIRED COMPONENTS Core Network Sql Widgets Multimedia MultimediaWidgets Test)
find_package(ZLIB REQUIRED)

# md4c：CommonMark + GFM 扩展的 Markdown 解析器（与 Qt 内嵌同源，MIT）。
# 来源 https://github.com/mity/md4c release-0.5.2，见 third_party/md4c/LICENSE.md。
# ---- MaiAgent：agent 核心，纯 C++ 的库 ----
#
# 直接 add_subdirectory 源码编进来，不走预编译产物：
# 两边同一个工具链、同一套 Release 选项，省掉 ABI 对不上的一整类问题。
#
# 它自带的第三方依赖全在 MaiAgent/third_party 里，**构建过程不下载任何东西**。
# 和这边的依赖不打架：Windows 上它的 curl 走 Schannel（不碰我们 vendor 的 openssl），
# sqlite 是它自己 amalgamation 的一份，这边本来也没有。
#
# 测试关掉：那是 MaiAgent 自己仓库里跑的事，不该拖慢这边的构建。
set(MAIAGENT_BUILD_TESTS OFF CACHE BOOL "" FORCE)

# AUTOMOC/AUTOUIC/AUTORCC 是**目录属性**，会顺着 add_subdirectory 传染进去。
# MaiAgent 里一个 Q_OBJECT 都没有，让 moc 去扫它纯属浪费，还会给它每个目标
# 凭空多出一个 mocs_compilation.cpp。进去之前关掉，出来再恢复。
set(_maichat_automoc ${CMAKE_AUTOMOC})
set(_maichat_autouic ${CMAKE_AUTOUIC})
set(_maichat_autorcc ${CMAKE_AUTORCC})
set(CMAKE_AUTOMOC OFF)
set(CMAKE_AUTOUIC OFF)
set(CMAKE_AUTORCC OFF)

# 还要把 ZLIB_FOUND 一起藏起来。
#
# 上面那句 find_package(ZLIB REQUIRED) 把 ZLIB_FOUND 留在了当前作用域，
# 而子目录会继承它。curl 的 CMakeLists 是这么写的：
#
#     curl_dependency_option(ZLIB)   # 看 CURL_ZLIB，我们已经设成 OFF 了
#     if(ZLIB_FOUND)                 # 但这一句不看 CURL_ZLIB，只看有没有找到过
#       list(APPEND CURL_LIBS ZLIB::ZLIB)
#
# 所以光在 MaiAgent 那边设 CURL_ZLIB=OFF 是拦不住的。而这边的 ZLIB 指的是
# Qt 自带的私有副本（include/QtZlib），它的 zlib.h 第一行就 #include <qglobal.h>——
# curl 的 C 文件没有 Qt 的 include 路径，编译直接 C1083，报错还落在 curl 的源码里。
set(_maichat_zlib_found ${ZLIB_FOUND})
set(ZLIB_FOUND FALSE)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../MaiAgent maiagent EXCLUDE_FROM_ALL)

set(ZLIB_FOUND ${_maichat_zlib_found})
set(CMAKE_AUTOMOC ${_maichat_automoc})
set(CMAKE_AUTOUIC ${_maichat_autouic})
set(CMAKE_AUTORCC ${_maichat_autorcc})

add_library(md4c STATIC
  third_party/md4c/md4c.c
  third_party/md4c/md4c.h
  third_party/md4c/md4c-html.c
  third_party/md4c/md4c-html.h
  third_party/md4c/entity.c
  third_party/md4c/entity.h
)
target_include_directories(md4c PUBLIC third_party/md4c)

# Markdown 的解析 + 排版 + 绘制。**两端共用这一套**（agent 和 IM）。
#
# 单独成库是被依赖方向逼出来的：绘制要 Qt5::Widgets，而 remote_im_core 是
# Core/Network/Sql 的，给它加 Widgets 会把一批无头测试也拖进去要 QApplication。
# 反过来它又不能放进 remote_im_desktop_ui——maichat_agent 在那一层下面，也要用。
# 所以放在最底下，谁都能依赖，它谁也不依赖。
add_library(maichat_markdown
  src/markdown/MarkdownDocument.h
  src/markdown/MarkdownDocument.cpp
  src/markdown/MarkdownTheme.h
  src/markdown/MarkdownTheme.cpp
  src/markdown/MarkdownLayout.h
  src/markdown/MarkdownLayout.cpp
  src/markdown/MarkdownLayoutCache.h
  src/markdown/MarkdownLayoutCache.cpp
  src/markdown/MarkdownView.h
  src/markdown/MarkdownView.cpp
  src/markdown/MarkdownLabel.h
  src/markdown/MarkdownLabel.cpp
)
target_include_directories(maichat_markdown PUBLIC src)
target_link_libraries(maichat_markdown PUBLIC Qt5::Core Qt5::Gui Qt5::Widgets PRIVATE md4c)

add_library(remote_im_core
  src/model/RemoteIMContact.h
  src/model/RemoteIMMessage.h
  src/model/RemoteIMVideoPayload.h
  src/model/ChatState.h
  src/model/ChatState.cpp
  src/model/ContactGroups.h
  src/model/ContactGroups.cpp
  src/model/MessageNotification.h
  src/model/MessageNotification.cpp
  src/model/MessageQuote.h
  src/model/MessageQuote.cpp
  src/model/MessageSearch.h
  src/model/MessageSearch.cpp
  src/model/PinyinInitials.cpp
  src/diagnostics/PerformanceLog.h
  src/diagnostics/PerformanceLog.cpp
  src/diagnostics/RemoteDiagnosticsEvidence.h
  src/diagnostics/RemoteDiagnosticsEvidence.cpp
  src/diagnostics/RemoteDiagnosticsProtocol.h
  src/diagnostics/RemoteDiagnosticsProtocol.cpp
  src/markdown/MarkdownRenderer.h
  src/markdown/PreviewText.h
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
  src/im/VideoFileMetadata.h
  src/im/VideoFileMetadata.cpp
  src/im/FakeRemoteIMClient.h
  src/im/FakeRemoteIMClient.cpp
  src/storage/LocalMessageDatabase.h
  src/storage/LocalMessageDatabase.cpp
  src/storage/LocalSettingsStore.h
  src/storage/LocalSettingsStore.cpp
  src/storage/RemoteIMMediaStore.h
  src/storage/RemoteIMMediaStore.cpp
  src/logging/AppLog.h
  src/logging/AppLog.cpp
  src/remote/CaptureGeometry.h
  src/remote/CaptureGeometry.cpp
  src/remote/RemoteDesktopSignal.h
  src/remote/RemoteDesktopSignal.cpp
  src/remote/RemoteInputProtocol.h
  src/remote/RemoteInputProtocol.cpp
  src/remote/RemoteKeyMapping.h
  src/remote/RemoteKeyMapping.cpp
  src/remote/RemoteInputInjector.h
  src/remote/RemoteInputInjector.cpp
  src/remote/RemoteInputSink.cpp
  src/remote/RemoteInputSender.h
  src/remote/RemoteInputSender.cpp
  src/remote/DisplaySleepBlocker.h
  src/remote/DisplaySleepBlocker.cpp
  src/remote/SecureDesktopMonitor.h
  src/remote/SecureDesktopMonitor.cpp
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
  src/remote/TrtcNetworkProxyApi.h
)
target_include_directories(remote_im_core PUBLIC src)
target_link_libraries(remote_im_core PUBLIC Qt5::Core Qt5::Network Qt5::Sql ZLIB::ZLIB PRIVATE md4c)
if(APPLE)
  target_link_libraries(remote_im_core PRIVATE
    "-framework ApplicationServices"
    "-framework IOKit")
endif()
if(MAICHAT_TRTC_AVAILABLE)
  if(WIN32)
    target_include_directories(remote_im_core PRIVATE
      "${MAICHAT_TRTC_ROOT}/include"
      "${MAICHAT_TRTC_ROOT}/include/TRTC")
    target_link_libraries(remote_im_core PRIVATE "${MAICHAT_TRTC_ROOT}/lib/x64/liteav.lib")
  elseif(APPLE)
    set(MAICHAT_TRTC_MAC_HEADERS
        "${MAICHAT_TRTC_ROOT}/TXLiteAVSDK_TRTC_Mac.xcframework/${MAICHAT_TRTC_MAC_SLICE}/TXLiteAVSDK_TRTC_Mac.framework/Headers")
    target_include_directories(remote_im_core PRIVATE
      "${MAICHAT_TRTC_MAC_HEADERS}"
      "${MAICHAT_TRTC_MAC_HEADERS}/cpp_interface")
    target_link_libraries(remote_im_core PRIVATE ${MAICHAT_TRTC_MAC_FRAMEWORKS})
  endif()
  target_compile_definitions(remote_im_core PRIVATE MAICHAT_HAVE_TRTC=1)
endif()

add_library(remote_im_desktop_app
  src/diagnostics/RemoteDiagnosticsController.h
  src/diagnostics/RemoteDiagnosticsController.cpp
  src/app/RemoteIMApplication.h
  src/app/RemoteIMApplication.cpp
  src/app/VideoCoverImage.h
  src/app/VideoCoverImage.cpp
)
target_include_directories(remote_im_desktop_app PUBLIC src)
# Qt5::Gui：视频封面要用 QImage/QPainter 画（并在 Windows 上把系统缩略图的 HBITMAP
# 转成 QImage）。remote_im_core 保持无 GUI 依赖，封面这层留在应用层。
target_link_libraries(remote_im_desktop_app PUBLIC remote_im_core Qt5::Gui)
if(WIN32)
  # IShellItemImageFactory（资源管理器的缩略图提供程序）取视频首帧。
  target_link_libraries(remote_im_desktop_app PRIVATE shell32 ole32)
endif()

add_library(remote_im_desktop_ui
  resources/resources.qrc
  src/ui/AppMessageDialog.h
  src/ui/AppMessageDialog.cpp
  src/ui/AppTextInputDialog.h
  src/ui/AppTextInputDialog.cpp
  src/ui/RemoteDiagnosticsDialog.h
  src/ui/RemoteDiagnosticsDialog.cpp
  src/ui/BroadcastDialog.h
  src/ui/BroadcastDialog.cpp
  src/ui/AddContactDialog.h
  src/ui/AddContactDialog.cpp
  src/ui/LoginDialog.h
  src/ui/LoginDialog.cpp
  src/ui/MainWindow.h
  src/ui/MainWindow.cpp
  src/ui/MessageImageLoader.h
  src/ui/MessageImageLoader.cpp
  src/ui/FilePreviewDialog.h
  src/ui/FilePreviewDialog.cpp
  src/ui/ImagePreviewDialog.h
  src/ui/ImagePreviewDialog.cpp
  src/ui/VideoPreviewDialog.h
  src/ui/VideoPreviewDialog.cpp
  src/ui/RemoteDesktopConsentDialog.h
  src/ui/RemoteDesktopConsentDialog.cpp
  src/ui/RemoteDesktopProxyDialog.h
  src/ui/RemoteDesktopProxyDialog.cpp
  src/ui/SharingIndicatorBar.h
  src/ui/SharingIndicatorBar.cpp
  src/ui/RemoteDesktopViewPanel.h
  src/ui/RemoteDesktopViewPanel.cpp
  src/ui/RemoteDesktopSessionCard.h
  src/ui/RemoteDesktopSessionCard.cpp
  src/ui/RemoteInputCapture.h
  src/ui/RemoteInputCapture.cpp
  src/ui/UiZoom.h
  src/ui/UiZoom.cpp
)
target_include_directories(remote_im_desktop_ui PUBLIC src)
target_link_libraries(remote_im_desktop_ui PUBLIC remote_im_desktop_app remote_im_core
  maichat_agent Qt5::Widgets Qt5::Multimedia Qt5::MultimediaWidgets)

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
# 版本号带进运行时：日志第一行就得答出"用户跑的到底是哪个包"，
# 这个问题每次排障都要问一遍。
target_compile_definitions(maichat PRIVATE MAICHAT_VERSION="${PROJECT_VERSION}")
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
  if(MAICHAT_TRTC_AVAILABLE)
    set_target_properties(maichat PROPERTIES
      BUILD_WITH_INSTALL_RPATH TRUE
      BUILD_RPATH "@executable_path/../Frameworks"
      INSTALL_RPATH "@executable_path/../Frameworks"
    )
    add_custom_command(TARGET maichat POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory
              "$<TARGET_BUNDLE_DIR:maichat>/Contents/Frameworks"
      COMMAND /usr/bin/ditto
              "${MAICHAT_TRTC_ROOT}/TXLiteAVSDK_TRTC_Mac.xcframework/${MAICHAT_TRTC_MAC_SLICE}/TXLiteAVSDK_TRTC_Mac.framework"
              "$<TARGET_BUNDLE_DIR:maichat>/Contents/Frameworks/TXLiteAVSDK_TRTC_Mac.framework"
      COMMAND /usr/bin/ditto
              "${MAICHAT_TRTC_ROOT}/TXFFmpeg.xcframework/${MAICHAT_TRTC_MAC_SLICE}/TXFFmpeg.framework"
              "$<TARGET_BUNDLE_DIR:maichat>/Contents/Frameworks/TXFFmpeg.framework"
      COMMAND /usr/bin/ditto
              "${MAICHAT_TRTC_ROOT}/TXSoundTouch.xcframework/${MAICHAT_TRTC_MAC_SLICE}/TXSoundTouch.framework"
              "$<TARGET_BUNDLE_DIR:maichat>/Contents/Frameworks/TXSoundTouch.framework"
      COMMENT "Embedding macOS TRTC frameworks into MaiChat.app"
    )
  endif()
endif()
target_link_libraries(maichat PRIVATE remote_im_desktop_ui remote_im_desktop_app remote_im_core maichat_agent Qt5::Widgets)
if(WIN32)
  # SetCurrentProcessExplicitAppUserModelID：没有它系统通知会被静默丢弃。
  target_link_libraries(maichat PRIVATE shell32)
endif()
if(MAICHAT_USE_FAKE_CLIENT)
  target_compile_definitions(maichat PRIVATE MAICHAT_USE_FAKE_CLIENT=1)
endif()

# ---- agent 接入层 ----
# 只做一件事：把 MaiAgent 的事件搬到 Qt 主线程。界面代码不直接碰 MaiAgent 的事件总线。
add_library(maichat_agent
  src/agent/AgentController.h
  src/agent/AgentController.cpp
  src/agent/AgentChatPanel.h
  src/agent/AgentChatPanel.cpp
  src/agent/AgentSessionList.h
  src/agent/AgentSessionList.cpp
)
target_include_directories(maichat_agent PUBLIC src)
target_link_libraries(maichat_agent PUBLIC Qt5::Core Qt5::Widgets maiagent maichat_markdown)

add_executable(markdown_document_test tests/MarkdownDocumentTest.cpp)
target_link_libraries(markdown_document_test PRIVATE maichat_markdown Qt5::Test)

add_executable(markdown_layout_test tests/MarkdownLayoutTest.cpp)
target_link_libraries(markdown_layout_test PRIVATE maichat_markdown Qt5::Widgets Qt5::Test)

add_executable(markdown_view_test tests/MarkdownViewTest.cpp)
target_link_libraries(markdown_view_test PRIVATE maichat_markdown Qt5::Widgets Qt5::Test)

add_executable(markdown_label_test tests/MarkdownLabelTest.cpp)
target_link_libraries(markdown_label_test PRIVATE maichat_markdown Qt5::Widgets Qt5::Test)

add_executable(agent_controller_test tests/AgentControllerTest.cpp)
target_link_libraries(agent_controller_test PRIVATE remote_im_desktop_ui remote_im_desktop_app
  remote_im_core maichat_agent maichat_markdown Qt5::Widgets Qt5::Test)

add_executable(agent_panel_session_test tests/AgentPanelSessionTest.cpp)
# UiZoom 在 remote_im_desktop_ui 里，而 AgentChatPanel 用它做缩放，所以要一起链。
target_link_libraries(agent_panel_session_test PRIVATE remote_im_desktop_ui remote_im_desktop_app
  remote_im_core maichat_agent maichat_markdown Qt5::Widgets Qt5::Test)

add_executable(remote_diagnostics_controller_test tests/RemoteDiagnosticsControllerTest.cpp)
target_link_libraries(remote_diagnostics_controller_test PRIVATE remote_im_desktop_app Qt5::Test)
target_compile_definitions(remote_diagnostics_controller_test PRIVATE
  MAICHAT_DIAGNOSTICS_REPORT_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fixtures/remote-diagnostics-report.json")

add_executable(attachment_download_evidence_test tests/AttachmentDownloadEvidenceTest.cpp)
target_link_libraries(attachment_download_evidence_test PRIVATE remote_im_core Qt5::Network Qt5::Test)

add_executable(remote_diagnostics_ui_test tests/RemoteDiagnosticsUiTest.cpp)
target_link_libraries(remote_diagnostics_ui_test PRIVATE remote_im_desktop_ui remote_im_desktop_app Qt5::Widgets Qt5::Test)

add_executable(remote_diagnostics_evidence_test tests/RemoteDiagnosticsEvidenceTest.cpp)
target_link_libraries(remote_diagnostics_evidence_test PRIVATE remote_im_core Qt5::Test)

add_executable(remote_diagnostics_protocol_test tests/RemoteDiagnosticsProtocolTest.cpp)
target_link_libraries(remote_diagnostics_protocol_test PRIVATE remote_im_core Qt5::Test)
# 回绝样例是三端共用的同一份文件，测试直接读它，不再各自抄一份提示串。
target_compile_definitions(remote_diagnostics_protocol_test PRIVATE
  MAICHAT_FAILURE_RECEIPT_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fixtures/remote-diagnostics-failure-receipts.json")

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

add_executable(app_message_dialog_test tests/AppMessageDialogTest.cpp)
target_link_libraries(app_message_dialog_test PRIVATE remote_im_desktop_ui remote_im_core Qt5::Widgets Qt5::Test)

add_executable(message_notification_test tests/MessageNotificationTest.cpp)
target_link_libraries(message_notification_test PRIVATE remote_im_core Qt5::Test)

# 视觉自检程序：手工跑起来截图用，不进 ctest。
add_executable(agent_page_visual_probe tests/AgentPageVisualProbe.cpp)
target_link_libraries(agent_page_visual_probe PRIVATE remote_im_desktop_ui remote_im_desktop_app
  remote_im_core maichat_agent maichat_markdown Qt5::Widgets Qt5::Test)

add_executable(im_markdown_visual_probe tests/ImMarkdownVisualProbe.cpp)
target_link_libraries(im_markdown_visual_probe PRIVATE remote_im_desktop_ui remote_im_desktop_app
  remote_im_core maichat_markdown Qt5::Widgets Qt5::Test)

add_executable(im_stress_probe tests/ImStressProbe.cpp)
target_link_libraries(im_stress_probe PRIVATE remote_im_desktop_ui remote_im_desktop_app
  remote_im_core maichat_markdown Qt5::Widgets Qt5::Test)

add_executable(markdown_text_probe tests/MarkdownTextProbe.cpp)
target_link_libraries(markdown_text_probe PRIVATE maichat_markdown Qt5::Widgets)

add_executable(markdown_visual_probe tests/MarkdownVisualProbe.cpp)
target_link_libraries(markdown_visual_probe PRIVATE maichat_markdown Qt5::Widgets)

add_executable(quote_visual_probe tests/QuoteVisualProbe.cpp)
target_link_libraries(quote_visual_probe PRIVATE remote_im_desktop_ui remote_im_desktop_app remote_im_core Qt5::Widgets)

add_executable(message_quote_test tests/MessageQuoteTest.cpp)
target_link_libraries(message_quote_test PRIVATE remote_im_core Qt5::Test)

add_executable(broadcast_dialog_test tests/BroadcastDialogTest.cpp)
target_link_libraries(broadcast_dialog_test PRIVATE remote_im_desktop_ui remote_im_desktop_app remote_im_core Qt5::Widgets Qt5::Test)

add_executable(app_text_input_dialog_test tests/AppTextInputDialogTest.cpp)
target_link_libraries(app_text_input_dialog_test PRIVATE remote_im_desktop_ui remote_im_core Qt5::Widgets Qt5::Test)

add_executable(file_preview_dialog_test tests/FilePreviewDialogTest.cpp)
target_link_libraries(file_preview_dialog_test PRIVATE remote_im_desktop_ui remote_im_core Qt5::Widgets Qt5::Test)

add_executable(video_preview_dialog_test tests/VideoPreviewDialogTest.cpp)
target_link_libraries(video_preview_dialog_test PRIVATE remote_im_desktop_ui remote_im_core Qt5::Widgets Qt5::Multimedia Qt5::Test)
target_compile_definitions(video_preview_dialog_test PRIVATE
  MAICHAT_TEST_VIDEO_PATH="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/video-lifecycle.mp4")

add_executable(add_contact_dialog_test tests/AddContactDialogTest.cpp)
target_link_libraries(add_contact_dialog_test PRIVATE remote_im_desktop_ui remote_im_desktop_app remote_im_core Qt5::Widgets Qt5::Test)

add_executable(main_window_layout_test tests/MainWindowLayoutTest.cpp)
target_link_libraries(main_window_layout_test PRIVATE remote_im_desktop_ui remote_im_desktop_app remote_im_core Qt5::Widgets Qt5::Test)

add_executable(remote_desktop_signal_test tests/RemoteDesktopSignalTest.cpp)
target_link_libraries(remote_desktop_signal_test PRIVATE remote_im_core Qt5::Test)

add_executable(remote_input_protocol_test tests/RemoteInputProtocolTest.cpp)
target_link_libraries(remote_input_protocol_test PRIVATE remote_im_core Qt5::Test)

add_executable(remote_input_injector_test tests/RemoteInputInjectorTest.cpp)
target_link_libraries(remote_input_injector_test PRIVATE remote_im_core Qt5::Test)

add_executable(remote_input_sender_test tests/RemoteInputSenderTest.cpp)
target_link_libraries(remote_input_sender_test PRIVATE remote_im_core Qt5::Test)

add_executable(secure_desktop_monitor_test tests/SecureDesktopMonitorTest.cpp)
target_link_libraries(secure_desktop_monitor_test PRIVATE remote_im_core Qt5::Test)

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

add_executable(video_file_metadata_test tests/VideoFileMetadataTest.cpp)
target_link_libraries(video_file_metadata_test PRIVATE remote_im_core Qt5::Test)

add_executable(video_cover_image_test tests/VideoCoverImageTest.cpp)
target_link_libraries(video_cover_image_test PRIVATE remote_im_desktop_app remote_im_core Qt5::Gui Qt5::Test)

if(MAICHAT_TRTC_AVAILABLE AND WIN32)
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

add_executable(remote_input_capture_test tests/RemoteInputCaptureTest.cpp)
target_link_libraries(remote_input_capture_test PRIVATE remote_im_desktop_ui remote_im_core Qt5::Widgets Qt5::Test)

add_executable(remote_desktop_ui_test tests/RemoteDesktopUiTest.cpp)
target_link_libraries(remote_desktop_ui_test PRIVATE remote_im_desktop_ui remote_im_core Qt5::Widgets Qt5::Test)

add_executable(message_image_loader_test tests/MessageImageLoaderTest.cpp)
target_link_libraries(message_image_loader_test PRIVATE remote_im_desktop_ui remote_im_desktop_app remote_im_core Qt5::Widgets Qt5::Test)

add_executable(message_search_test tests/MessageSearchTest.cpp)
target_link_libraries(message_search_test PRIVATE remote_im_core Qt5::Widgets Qt5::Test)

add_executable(ui_zoom_test tests/UiZoomTest.cpp)
target_link_libraries(ui_zoom_test PRIVATE remote_im_desktop_ui remote_im_desktop_app remote_im_core Qt5::Widgets Qt5::Test)

add_executable(performance_log_test tests/PerformanceLogTest.cpp)
target_link_libraries(performance_log_test PRIVATE remote_im_core Qt5::Test)
enable_testing()
add_test(NAME markdown_document_test COMMAND markdown_document_test)
add_test(NAME markdown_layout_test COMMAND markdown_layout_test)
add_test(NAME markdown_view_test COMMAND markdown_view_test)
add_test(NAME markdown_label_test COMMAND markdown_label_test)
add_test(NAME agent_controller_test COMMAND agent_controller_test)
add_test(NAME agent_panel_session_test COMMAND agent_panel_session_test)
add_test(NAME performance_log_test COMMAND performance_log_test)
add_test(NAME remote_diagnostics_controller_test COMMAND remote_diagnostics_controller_test)
add_test(NAME attachment_download_evidence_test COMMAND attachment_download_evidence_test)
add_test(NAME remote_diagnostics_ui_test COMMAND remote_diagnostics_ui_test)
add_test(NAME remote_diagnostics_evidence_test COMMAND remote_diagnostics_evidence_test)
add_test(NAME remote_diagnostics_protocol_test COMMAND remote_diagnostics_protocol_test)
add_test(NAME message_notification_test COMMAND message_notification_test)
add_test(NAME message_quote_test COMMAND message_quote_test)
add_test(NAME broadcast_dialog_test COMMAND broadcast_dialog_test)
add_test(NAME chat_state_test COMMAND chat_state_test)
add_test(NAME fake_remote_im_client_test COMMAND fake_remote_im_client_test)
add_test(NAME tencent_user_sig_generator_test COMMAND tencent_user_sig_generator_test)
add_test(NAME tim_sdk_remote_im_client_test COMMAND tim_sdk_remote_im_client_test)
add_test(NAME markdown_renderer_test COMMAND markdown_renderer_test)
add_test(NAME local_message_database_test COMMAND local_message_database_test)
add_test(NAME remote_im_application_test COMMAND remote_im_application_test)
add_test(NAME login_dialog_test COMMAND login_dialog_test)
add_test(NAME app_message_dialog_test COMMAND app_message_dialog_test)
add_test(NAME app_text_input_dialog_test COMMAND app_text_input_dialog_test)
add_test(NAME file_preview_dialog_test COMMAND file_preview_dialog_test)
add_test(NAME video_preview_dialog_test COMMAND video_preview_dialog_test)
add_test(NAME add_contact_dialog_test COMMAND add_contact_dialog_test)
add_test(NAME main_window_layout_test COMMAND main_window_layout_test)
add_test(NAME remote_desktop_signal_test COMMAND remote_desktop_signal_test)
add_test(NAME remote_input_protocol_test COMMAND remote_input_protocol_test)
add_test(NAME remote_input_injector_test COMMAND remote_input_injector_test)
add_test(NAME remote_input_sender_test COMMAND remote_input_sender_test)
add_test(NAME secure_desktop_monitor_test COMMAND secure_desktop_monitor_test)
add_test(NAME remote_desktop_auth_test COMMAND remote_desktop_auth_test)
add_test(NAME remote_desktop_session_test COMMAND remote_desktop_session_test)
add_test(NAME remote_desktop_settings_test COMMAND remote_desktop_settings_test)
add_test(NAME remote_desktop_controller_test COMMAND remote_desktop_controller_test)
add_test(NAME trtc_engine_test COMMAND trtc_engine_test)
add_test(NAME video_file_metadata_test COMMAND video_file_metadata_test)
add_test(NAME video_cover_image_test COMMAND video_cover_image_test)
add_test(NAME remote_input_capture_test COMMAND remote_input_capture_test)
add_test(NAME remote_desktop_ui_test COMMAND remote_desktop_ui_test)
add_test(NAME message_image_loader_test COMMAND message_image_loader_test)
add_test(NAME message_search_test COMMAND message_search_test)
add_test(NAME ui_zoom_test COMMAND ui_zoom_test)
