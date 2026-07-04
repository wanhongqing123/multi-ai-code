# Android Native IM and Qt Desktop IM Design

## Status

Approved direction from discussion:

- Android must be upgraded from local-only chat state to a real Native IM SDK path.
- Windows and macOS need a dedicated IM application.
- The desktop IM application must not use Electron or QML.
- The desktop IM application should use Qt Widgets and C++.
- The first desktop IM version is a pure IM client. It does not embed the AICLI terminal or PTY.

## Context

The project already has three IM-related surfaces:

- iOS dedicated app under `ios/MultiAIIM`.
- Android dedicated app under `android/MultiAIIM`.
- Electron main app remote IM panel under `src/remote-im` and `electron/remote-im`.

iOS already uses a Native IM SDK through CocoaPods:

- `TXIMSDK_Plus_iOS_XCFramework`
- `V2TIMManager` in `ios/MultiAIIM/MultiAIIM/TencentIMClient.swift`

Android currently has local app state and UI helpers, but does not have a real IM SDK dependency:

- `ChatState`
- `RemoteIMSessionController`
- `LocalChatHistoryStore`
- `RemoteIMMediaStore`
- `RemoteIMPermissionPolicy`

That means Android can model messages locally, but it does not yet perform real SDK login, send, or receive.

## Goals

1. Make Android use the official Android Native IM SDK for real message transport.
2. Keep Android structure close to iOS: UI and state talk through a small client abstraction instead of calling the SDK everywhere.
3. Add a Windows/macOS desktop IM client implemented with Qt Widgets and C++.
4. Keep desktop IM independent from the Electron workbench and AICLI PTY terminal.
5. Align product behavior across iOS, Android, Windows, and macOS for the core IM use cases:
   - login and logout
   - contact or peer selection
   - text send and receive
   - image send and receive
   - voice send and receive
   - local chat history
   - local media cache
   - image click-to-preview

## Non-Goals

The first implementation will not include:

- Electron reuse for the dedicated desktop IM app.
- QML.
- AICLI terminal, PTY, or project/task workspace features inside the desktop IM app.
- Windows installer packaging, macOS notarization, or app-store distribution.
- A shared cross-language IM domain library across Swift, Java, and C++.
- A server-side UserSig service migration. The apps continue to use the existing credential model unless a separate security task changes it.

## Design Summary

Implement this in two stages:

1. Android Native IM integration.
2. Qt Widgets desktop IM app for Windows and macOS.

Android comes first because it closes an existing product gap and gives a second real mobile SDK implementation to compare against iOS before adding desktop clients.

The desktop app should be created as a new standalone project:

```text
desktop/qt-im/
  CMakeLists.txt
  src/
    main.cpp
    app/
    im/
    model/
    storage/
    ui/
    platform/
  tests/
```

The desktop app uses Qt Widgets for UI, C++ for state, and thin platform-specific SDK adapters for Windows and macOS.

## Android Architecture

### Dependency

Add the official Android IM SDK dependency to `android/MultiAIIM/app/build.gradle`.

The exact SDK version should be pinned at implementation time from the current official Maven Central recommendation.

### Main Components

Add a small Android IM client abstraction:

```text
RemoteIMClient
  connect(sdkAppId, userId, userSig)
  disconnect()
  sendText(peerId, text)
  sendImage(peerId, localPath)
  sendVoice(peerId, localPath, durationSeconds)
  setListener(listener)
```

Add a Native SDK implementation:

```text
NativeRemoteIMClient
  wraps SDK init/login/logout
  wraps text/image/voice sending
  maps SDK callbacks to IncomingText, IncomingImage, IncomingVoice
  downloads or caches incoming media through RemoteIMMediaStore
```

Keep `ChatState` as the app-level message state. SDK callbacks should be converted into existing message model operations:

- incoming text -> `ChatState.receiveText`
- incoming image -> `ChatState.receiveImage`
- incoming voice -> `ChatState.receiveVoice`
- outgoing send success -> `ChatState.updateMessageStatus(..., SENT)`
- outgoing send failure -> add a failure status if needed, or keep pending and surface an error

`RemoteIMSessionController` becomes the coordinator between UI, local history, media storage, and `RemoteIMClient`.

### Android Data Flow

Outgoing text:

```text
MainActivity
  -> RemoteIMSessionController.sendTextMessage
  -> ChatState.queueOutgoingText
  -> NativeRemoteIMClient.sendText
  -> update status and save history
```

Incoming image:

```text
SDK message callback
  -> NativeRemoteIMClient
  -> RemoteIMMediaStore caches image
  -> RemoteIMSessionController receives event
  -> ChatState.receiveImage
  -> save history
  -> MainActivity refreshes UI
```

### Android Permissions

Keep the existing permission policy and make the SDK path use it:

- image selection requires photo/media access where the platform requires it
- voice recording requires microphone permission
- network permission belongs in the manifest

Permission requests remain near UI actions. SDK calls should not request permissions directly.

### Android Error Handling

Errors should be visible but local state must remain recoverable:

- login failure keeps the user on the login/settings state
- send failure marks the message as failed when that status is added
- media download failure creates no local message until the file exists, or creates a received message with missing media only if the UI has a clear failed state
- SDK disconnect should show a connection state and allow reconnect

## Desktop Qt Architecture

### Project Layout

```text
desktop/qt-im/
  CMakeLists.txt
  src/
    main.cpp
    app/
      RemoteIMApplication.h
      RemoteIMApplication.cpp
    im/
      RemoteIMClient.h
      RemoteIMEvents.h
      FakeRemoteIMClient.h
      FakeRemoteIMClient.cpp
      WindowsRemoteIMClient.h
      WindowsRemoteIMClient.cpp
      MacRemoteIMClient.h
      MacRemoteIMClient.mm
    model/
      ChatState.h
      ChatState.cpp
      RemoteIMMessage.h
      RemoteIMContact.h
    storage/
      LocalSettingsStore.h
      LocalChatHistoryStore.h
      RemoteIMMediaStore.h
    ui/
      MainWindow.h
      MainWindow.cpp
      LoginDialog.h
      LoginDialog.cpp
      ConversationListWidget.h
      ConversationListWidget.cpp
      ChatViewWidget.h
      ChatViewWidget.cpp
      MessageBubbleWidget.h
      MessageBubbleWidget.cpp
      ImagePreviewDialog.h
      ImagePreviewDialog.cpp
```

### UI

Use Qt Widgets only.

Main window layout:

```text
+--------------------------------------------------+
| Multi-AI IM                                      |
+--------------------+-----------------------------+
| conversations       | current peer / status       |
|                    |-----------------------------|
| recent peers        | message list                |
|                    |                             |
|                    |-----------------------------|
|                    | voice | text input | + | send |
+--------------------+-----------------------------+
```

The `+` button opens image selection first. Extra actions can be added later through a small menu without changing the layout.

### Desktop SDK Adapters

Keep SDK-specific code behind one interface:

```text
RemoteIMClient
  connect
  disconnect
  sendText
  sendImage
  sendVoice
  callbacks for incoming text/image/voice and connection state
```

Implementations:

- `WindowsRemoteIMClient.cpp`
  - uses the official Windows C/C++ IM SDK
  - links SDK headers and libraries through CMake
- `MacRemoteIMClient.mm`
  - Objective-C++ bridge for the official macOS IM SDK
  - exposes a C++ interface to the rest of the Qt app
- `FakeRemoteIMClient`
  - used by unit tests and local UI development without SDK credentials

Do not put SDK calls directly in UI widgets.

### Desktop State and Storage

Use a C++ `ChatState` similar to iOS/Android concepts:

- owner user id
- contact list
- selected peer id
- message list
- queue outgoing text/image/voice
- receive incoming text/image/voice
- update message status

Local storage:

- settings: Qt settings or JSON under the app data directory
- chat history: JSON lines or SQLite under the app data directory
- media cache: app data directory with separate image and voice folders

The first implementation should prefer simple JSON files unless message volume forces SQLite. This keeps the initial app easier to inspect and debug.

### Desktop Data Flow

Outgoing image:

```text
Image button
  -> QFileDialog selects image
  -> RemoteIMMediaStore copies image to app cache
  -> ChatState.queueOutgoingImage
  -> platform RemoteIMClient.sendImage
  -> status update
  -> history save
```

Incoming voice:

```text
platform SDK callback
  -> platform RemoteIMClient normalizes event
  -> RemoteIMMediaStore stores downloaded voice file
  -> ChatState.receiveVoice
  -> history save
  -> UI refresh
```

## Cross-Platform Naming

New code should use neutral names such as:

- `RemoteIMClient`
- `NativeRemoteIMClient`
- `WindowsRemoteIMClient`
- `MacRemoteIMClient`

UI text should not expose SDK vendor names. Existing platform SDK names can still appear in build files, adapter implementation details, and documentation where needed.

## Testing

Android:

- Unit-test `ChatState` behavior remains intact.
- Unit-test `RemoteIMSessionController` with a fake `RemoteIMClient`.
- Unit-test SDK event mapping with fake incoming text/image/voice events.
- Keep media and permission policy tests.
- Run Gradle unit tests.
- Do one real-device smoke test after SDK credentials are configured.

Desktop Qt:

- Qt Test coverage for `ChatState`.
- Qt Test coverage for local settings and chat history stores.
- Fake client tests for send success, send failure, and incoming events.
- Build on macOS first.
- Add Windows build verification when a Windows machine or CI runner is available.

## Implementation Order

1. Add Android `RemoteIMClient` abstraction and fake implementation tests.
2. Add Android SDK dependency and `NativeRemoteIMClient`.
3. Wire Android controller/UI to the real client.
4. Validate Android text, image, and voice against iOS or the existing desktop remote IM path.
5. Create `desktop/qt-im` CMake skeleton.
6. Implement desktop model, storage, and fake-client UI.
7. Add Windows SDK adapter.
8. Add macOS SDK bridge.
9. Validate desktop text, image, and voice against iOS/Android.

## Risks

- SDK versions and integration steps may differ between Android, Windows, and macOS. Each platform adapter should stay isolated.
- macOS SDK bridging may require Objective-C++ build settings and framework signing details.
- Windows SDK packaging may require runtime DLL copying in the CMake install step.
- Local UserSig generation is convenient for development but should be revisited before production distribution.
- Voice format support may differ by platform; the media layer should normalize file extension and duration metadata.

## References

- Android IM SDK integration: https://cloud.tencent.com/document/product/269/75283
- Windows IM SDK integration: https://cloud.tencent.com/document/product/269/75287
- macOS IM SDK integration: https://cloud.tencent.com/document/product/269/75288
- C/C++ API overview: https://cloud.tencent.com/document/product/269/68286
