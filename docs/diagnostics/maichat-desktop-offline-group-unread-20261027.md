# Desktop offline group unread remains zero

## Report and reproduction

- Reported on 2026-10-27, macOS 26.4, source `f2637dcc5`, isolated Mac group test app at `MaiChat/desktop/dist-group-chat-e2e/MaiChat-macos-arm64/MaiChat.app`.
- In the three-account Tencent IM group test, leave Mac C offline, send a group text from another account, then log Mac C in without opening the group. The observed group unread count was `0 -> 0`; opening the group therefore could not verify unread clearing.
- Expected: the group row shows the unread count before opening, and opening it clears the count.
- The group and all three isolated accounts from that run were deleted after the test. The user's accounts and installed apps were not used for this reproduction.

## Evidence and cause

- `TimSdkRemoteIMClient::syncInitialData()` called `TIMConvGetConvList` once immediately after login. No `TIMSetConvEventCallback` was registered. The SDK's `TIMConversationManager.h` says conversation updates include unread changes and recommends querying the conversation list after `kTIMConvEvent_Finish`.
- When the initial list is empty and a later conversation sync supplies the group unread count, the old client has no path to receive it. A focused test with an empty initial list and later sync/update callback failed before the fix because the callback was absent.
- This is a proven client-side gap and matches the reported `0 -> 0`. The earlier live run did not retain the raw SDK conversation payload, so it does not prove this was the only factor in that particular run.

## Fix and verification

- Register the native conversation event callback, refresh after sync finish, and apply group Add/Update events as they arrive. Ignore replies from an older list fetch after a newer event and unregister the callback on logout/destruction.
- The focused test changed from failing to passing. Mac Qt Release `tim_sdk_remote_im_client_test`, `remote_im_application_test`, and `main_window_layout_test` passed (3/3). The normal Mac app target built successfully.
- Fix commit: `2190c81e`. Mac 2.2.29 staged package hash: pending package build.
- Remaining boundary: the exact offline three-account test must be rerun with the new Mac binary before calling the live unread scenario verified. Windows build and UI remain separately unverified at this point.
