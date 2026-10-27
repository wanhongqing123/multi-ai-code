# Windows Desktop 2.2.29 release build failures

## Reproduction

- Time: 2026-10-27 (Asia/Shanghai). Target: Windows VS2019, Qt 5.15.2, native Release build. The Windows build used an isolated staging directory; the installed app was not replaced.
- Source `f72359b43e174596697b7f8f80530b3b526333dd`: build `maichat` and `desktop_session_sync_host_test` with the normal compiler flags, then run the full Host test suite.
- Expected: both targets compile without ad hoc flags, and the Host suite completes without a cross-case exception.

## Observations and cause

- `MaiChat/Network/MaiCloudVod.cpp` used `std::min` while a Windows header had defined the `min` macro. A temporary `/DNOMINMAX` flag made that target compile, proving the macro conflict. The separate Host test failed to compile under MSVC C2065 because a constructor argument lambda named `coordinator` during that variable's own initialization.
- After those compile fixes, source `ff200f282e3b7a460ec5dcb91cf7cb32929f24a5` built successfully without a manual `/DNOMINMAX`. The full `desktop_session_sync_host_test` passed its first 18 cases and threw MSVC C++ exception `0xe06d7363` in `agentToolsReuseRemoteLifecycleCoordinator`. That case passed alone.
- The minimum failing sequence was `coordinatorAppliesPhoneLifecycleToDesktop` followed by `agentToolsReuseRemoteLifecycleCoordinator`. CDB stopped on the first throw in the main thread during `QMetaCallEvent::placeMetaCall`: a stale test callback reached `QHashData::detach_helper`, then `operator new(size=0xffffffffffffffff)` and `_Throw_bad_array_new_length`. The former case had scheduled `QTimer::singleShot(0, [completion])` without a context object, so its callback could run after its local coordinator was destroyed when the following case processed events.
- A temporary one-line change binding that timer to the test's `AgentController` changed the minimum pair from an exception to 4/4 checks passing and the complete Host suite from 19 checks then an exception to 22/22 checks passing. The temporary patch was restored before the formal fix.

## Fix and verification

- Commit `ff200f282e3b7a460ec5dcb91cf7cb32929f24a5`: add `NOMINMAX` to the `mai_cloud_vod` target on Windows; avoid the Host test's constructor-time self-reference.
- Commit `ce03dc8c16dbb0efe964b1318d25e6effd0828b3`: give all seven same-pattern zero-delay Host test callbacks a per-test QObject context, so pending callbacks are canceled when their test ends.
- Windows source `ce03dc8c1`, with no temporary flags or patches: the minimum pair passed 4/4, and the full Host suite passed 22/22 in Release. macOS arm64 Release built and its Host suite passed in 14.75 seconds.
- Windows `ce03dc8c1` VS2019/Qt Release rebuilt without `CL=/DNOMINMAX`. The minimum pair passed 4/4 and complete Host passed 22/22; TIM SDK, IM application, layout, login, Session Sync channel and Agent CTest suites passed 6/6. The native group conversation, login and Creation Center screenshots were inspected without clipping. The isolated staging app and ZIP extraction each passed `--smoke` with version 2.2.29, Qt 5.15.2 and OpenSSL 3.5.9.
- Windows staging path: `E:\OpenSource\multi-ai-code\MaiChat\desktop\dist-v2.2.29-ce03dc8c\`. The smoke logs recorded the staged executable and separately extracted ZIP executable paths, with no installed-app process replacement.
- Installer: `MaiChat-Setup-win64-v2.2.29-ce03dc8c.exe`, 112843616 bytes, SHA-256 `49932d629b61825a81ab774c9e6241d54a831d198c39d960cb07ff4bf08d918f`, Product/FileVersion `2.2.29-ce03dc8c`, Authenticode `NotSigned`.
- ZIP: `MaiChat-win64-v2.2.29-20261027-ce03dc8c.zip`, 139809817 bytes, SHA-256 `478c9a79b99a7b82ae7217341f88fe142944f04e2e45cf62e44b28018661fe84`, with an unsigned 90355712-byte `maichat.exe`.

## Boundary

The Windows build and tests above used VS2019/Qt Release. No Windows installer was run and no existing installed app was replaced. The EXE/ZIP are independently staged packages.

GitHub Release `maichat-2026-10-27` was published from product source `ce03dc8c16dbb0efe964b1318d25e6effd0828b3`. An unauthenticated public Releases API read returned `draft=false`, the tag at that same commit, five assets in `uploaded` state with their expected sizes and SHA-256 digests, and this release as `/releases/latest`. The public release page returned HTTP 200.
