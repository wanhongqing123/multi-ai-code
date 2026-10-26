# iOS HDR MOV playback fails before the first frame (2026-10-26)

## Report and reproduction boundary

- User device: iPhone 14 Pro Max. In the app's video player, one camera MOV opens to a black/nonresponsive picture; two same-scene MOV files play. The user later supplied the original failing file. The exact user entry point, on-device build and device log remain unverified.
- Supplied original: 18,452,323 bytes, SHA-256 `0d5c2ac9f84d9502ec489f7e34398aaae1ac2aae800941a814ba59e47ee7be14`. Local `ffprobe` confirms QuickTime MOV, HEVC Main 10 `hvc1`, `yuv420p10le`, 1920×1080, 16.505 s, BT.2020/HLG, Dolby Vision profile 8 with HLG compatibility, a −180° display matrix, AAC stereo, and five `mebx` data streams. The original stays outside the repository and was not edited or uploaded.
- Source tree inspected at `8ac971d3104ae447f965e8fb6ea0c0d479e4f248` with local changes. An earlier synthetic 128×128 HEVC Main 10/HLG/180° MOV under `/tmp/maichat-hlg-rotation-probe-20261026` was only a preliminary probe, not the reproduction below.

## Code evidence

- `MaiChat/ios/MaiChat/MaiChat/Media/MaiFfplayIosBridge.cpp` selects the native VideoToolbox/Graphics renderer for local HEVC Main 10 HLG without subtitle streams. A Dolby Vision/HLG camera MOV matches that selector if the local download retained the reported metadata.
- Shared `MaiChat/Media/ffplay.c` injects `hflip` and `vflip` for a −180° display matrix when automatic rotation is on. It selects best video/audio/subtitle streams; it does not open `mebx` data streams for playback.
- When the video filter graph fails before a frame, `ffplay.c` sends `FF_QUIT_EVENT`; its `do_exit()` returns status 0. The old iOS bridge retried the ordinary renderer only for nonzero exit status with zero presented frames. Therefore a zero-status, zero-frame native failure silently suppressed the fallback. The original-file simulator run below proves that this MOV hits that filter failure in the current iOS media stack.
- Apple documents Dolby Vision profile 8.4/HLG Main 10 playback on iPhone 12 and later. Existing `MaiChat/Media/FFPLAY_PARITY.md` records a successful iPhone 14 Pro Max HDR Main 10/Dolby Vision profile 8 sample. The codec combination alone is insufficient to explain the failure.

## Fix and verification

- The iOS bridge now falls back whenever the native attempt exits without a presented frame, regardless of its exit code. If neither route presents a frame, it returns an error instead of appearing to complete normally.
- A frame is counted only after the graphics presenter accepts it for its render queue. The ordinary software RGBA callback now counts successful frame submissions too; otherwise a video that actually displayed through the fallback would be mislabeled as a zero-frame failure. An accepted submission does not prove that Metal drew that frame; timed screenshots provide the visible-result check below.
- UI and Agent close mark the session as stopping before sending the command. Stop keeps requesting close until the worker finishes, covering the interval when FFplay resets its SDL event queue for the fallback run.
- `MaiChat/Media/tests/MaiFfplayIosBridgeFallbackTests.cpp` uses stub FFplay and graphics functions. Against the pre-fix bridge, it fails with `zero-frame native attempt did not fall back to software` (exit 1). Against the new bridge, it passes (exit 0): zero-code zero-frame retry, rejected native presentation followed by software retry, ordinary software RGBA presentation counted as success, native first-frame success, both routes zero-frame error, UI close and Agent close without retry.
- Targeted test command from repository root: `clang++ -std=c++17 -Wall -Wextra -Werror -pthread MaiChat/Media/tests/MaiFfplayIosBridgeFallbackTests.cpp MaiChat/ios/MaiChat/MaiChat/Media/MaiFfplayIosBridge.cpp -o /tmp/MaiFfplayIosBridgeFallbackTests && /tmp/MaiFfplayIosBridgeFallbackTests`.
- The changed bridge compiles for arm64 iOS Simulator with `xcrun --sdk iphonesimulator clang++ -std=c++17 -arch arm64 -mios-simulator-version-min=16.0 -Wall -Wextra -Werror -fsyntax-only ...`. Full-app and original-file simulator verification is recorded below.

## Original-file simulator reproduction and causal A/B (2026-10-26)

- Built the current source using `xcodebuild -workspace MaiChat/ios/MaiChat/MaiChat.xcworkspace -scheme MaiChat -configuration Debug -sdk iphonesimulator ... CODE_SIGNING_ALLOWED=NO build`; build succeeded, `CFBundleVersion=123`. Installed only on a newly created iOS 26.5 iPhone 17 Pro Max simulator. No real phone, user account, cloud object, or existing simulator App was changed.
- For each run, replaced only the test App bundle's `ffplay-seek-sample.mp4` with a byte-identical copy of the MOV variant, installed that bundle, and launched the existing `--ai-ui-test --mixed-gallery-ui-test` route. FFmpeg recognized the copied bytes as QuickTime MOV with the expected two or seven streams. The initial `--ffplay-ui-test` route crashed due to a missing SwiftUI `RemoteIMAppState` environment object; it was not used for playback conclusions.
- The strict `rotation-only-zero.mov` A/B changes only the video `tkhd` display matrix from −180° to zero, retaining all seven tracks, Dolby Vision metadata, and the same compressed audio/video packets. SHA-256 of this private variant is `8ee139e126eac02a903eb19f85de1b2bd55ff4c7bd35b51e94370e1677992b2a`. The strict `mebx-only-removed.mov` A/B removes only the five `mebx` tracks from the container, retaining the −180° matrix and Dolby Vision metadata; its SHA-256 is `7e31e1d2d5667681517757dbffe81020aa91fb9130d0a8ac98bc1f38f212f82e`. Private variants and screenshots remain outside the repository.

| Source | Native FFplay filter result | Follow-up frames | Actual simulator screen |
| --- | --- | --- | --- |
| Original: −180°, five `mebx` | `ffplay_buffer` → `auto_scale_0` → `ffplay_vflip` conversion fails once | 123 bridge retries ordinary path; first accepted frame `1920x1080 format=3 colorspace=1 range=0` (I420/BT.709); accepted milestones 100/200/300/400 | Visible moving video at playback 0:05/0:16 |
| Rotation matrix zero, five `mebx` | No `ffplay_vflip` conversion error | Native first accepted frame `1920x1080 format=4 colorspace=2 range=0` (P010/BT.2020); accepted milestones 100/200/300/400 | Visible moving video, upside-down as expected without the matrix; five repeat-run screenshots at t3/t6/t9/t12/t15 |
| Original rotation, `mebx` removed | Same `ffplay_vflip` conversion failure as original | Same ordinary fallback first frame (I420/BT.709); accepted milestones 100/200/300/400 | Visible moving video at playback ~0:05 |

Sanitized FFplay excerpt from the original-file run (the input path and image content are omitted):

```text
Impossible to convert between the formats supported by the filter 'ffplay_buffer' and the filter 'auto_scale_0'
Link 'ffplay_buffer.default' -> 'auto_scale_0.default':
Link 'auto_scale_0.default' -> 'ffplay_vflip.default':
[FFplayFrame] 1920x1080 format=3 colorspace=1 range=0
[FFplayFrame] presented=100
```

This isolates the −180° auto-rotation filter as the trigger of the native route's failure for this MOV. The five `mebx` tracks are not necessary for the failure. The original's HEVC Main 10/HLG/Dolby Vision stream decodes and displays through the existing 123 software fallback; H.264/SDR transcoding is unnecessary to demonstrate a playable route.

One screenshot of the rotation-zero variant immediately after its first simulator installation showed controls and advancing time but a black video area despite accepted frames; the same run later showed video, and a restart showed moving video in all five timed screenshots. `presented` is a successful graphics enqueue, not proof that the Metal frame was drawn, so this transient remains unexplained. It does not establish persistent black playback on the rotation-zero variant.

**Verification limit:** This proves behavior for the original bytes inside a Debug 123 iOS simulator build and identifies the failing FFplay filter link. It does not prove that the signed 123 iPhone build, the user's installed build, or a downloaded cloud copy follows the same route. Those require build/version and byte-hash checks plus a real-device playback trace. The old pre-fix bridge's failure is established by the targeted stub test above, not by a real-device A/B.

## Native HDR rotation repair gate (2026-10-26)

The bridge's zero-frame fallback prevents a silent black result, but the fallback loses the P010/BT.2020/HLG frame path. The shared FFplay filter graph now inserts `hwdownload` followed by a `format` filter using `frame->hw_frames_ctx->sw_format` before the auto-rotation filters when a VideoToolbox frame needs rotation. This keeps the FFplay clock, decoder and ordinary unrotated path unchanged.

`MaiChat/Media/tests/run_rotated_hdr_playback_test.py` generates a 192×108 HEVC Main 10, BT.2020/HLG MOV with a −180° display matrix in a temporary directory. It checks the codec, color metadata and matrix with `ffprobe`, builds a C++ driver against the **real Mac hosted FFplay library** and the iOS bridge, and runs three cases: unrotated HDR, rotated HDR and ordinary SDR. The driver requires a non-flat video frame, zero `Impossible to convert between the formats` errors, and P010/BT.2020/HLG output from the HDR cases. It accepts `--original <path>` to read the private user MOV in place; no user media is copied into the repository.

Before rebuilding the FFplay library, the generated rotated case failed as expected: `presented=1 non_flat=1 hdr=0 p010_bt2020_hlg=0 filter_link_errors=1` (exit 6). The frame came from the bridge's software fallback. After rebuilding `maichat_ffplay_hosted` from the filter repair, the cases reported:

| Case | Non-flat frame | P010/BT.2020/HLG frame | Filter link errors |
| --- | ---: | ---: | ---: |
| Generated unrotated HDR | 1 | 1 | 0 |
| Generated −180° HDR | 1 | 1 | 0 |
| Generated SDR | 1 | 0 | 0 |
| Supplied original MOV, read in place | 1 | 1 | 0 |

Targeted command, from the repository root, after building the Mac hosted library:

```sh
cmake --build MaiChat/desktop/build-release --target maichat_ffplay_hosted -j4
python3 MaiChat/Media/tests/run_rotated_hdr_playback_test.py \
  --library MaiChat/desktop/build-release/libmaichat_ffplay_hosted.dylib
```

For the exact original, add `--original <path-to-private-original.mov>`. This gate proves that the shared FFplay/bridge path submitted a non-flat P010/BT.2020/HLG frame to the host; it does **not** prove that Metal drew that frame on the actual iPhone screen. The simulator/phone UI, color appearance, playback continuity and rotation orientation require separate visual checks.

## Exact-original iOS Simulator after native filter repair (2026-10-26)

- Built `MaiChat` Debug 123 for arm64 iOS Simulator from this working tree with the repaired shared `ffplay.c`; the independently rebuilt simulator hosted FFplay dylib was linked into the App. The first `xcodebuild` attempt with a generic simulator destination failed during an unrelated vendored OpenSSL configuration because Xcode supplied `undefined_arch`; rebuilding with the explicit isolated simulator destination and `ARCHS=arm64` succeeded.
- Installed on a new iOS 26.5 iPhone 17 Pro Max simulator, UUID `F2D10E2B-80C6-4A14-9DAC-0BC4E7750134`. The original MOV was copied byte-for-byte to this *test App bundle's* `ffplay-seek-sample.mp4` resource. Both copies had SHA-256 `0d5c2ac9f84d9502ec489f7e34398aaae1ac2aae800941a814ba59e47ee7be14`. Launched `--ai-ui-test --mixed-gallery-ui-test`; no real iPhone, account, cloud object or user's installed App was touched.
- The exact original now reaches the native frame route: `[FFplayFrame] 1920x1080 format=4 colorspace=2 range=0` (P010/BT.2020), `presented=100` and later frame milestones. The captured simulator process log has **zero** `Impossible to convert`, `auto_scale_0`, software `format=3 colorspace=1` and `[MaiVideoHDR] render failed` entries. In warm runs, screenshots at playback 0:02 and 0:09 showed correctly oriented, moving beach footage. The accepted-frame count and screen images together show that the filter repair eliminated the previous filter failure and software color downgrade on this route.
- A fresh simulator erase, boot and install exposed a separate cold-display problem. The command started at 10:06:49 +08:00; screenshots at 10:06:52, 10:06:55 and 10:06:58 had player controls and an advancing clock but a black video rectangle. At 10:07:02 and 10:07:07 the video rectangle contained visible, correctly oriented footage. The FFplay log for this cold run still recorded native P010 frames and no filter/Graphics error. Local private evidence: `/tmp/maichat-native-cold-console.log`, `/tmp/maichat-native-cold-timing.txt` and `/tmp/maichat-native-cold-t02.png` through `t06.png`. These files remain outside the repository.

That cold-display problem was investigated and repaired in the next section. The preceding screenshots remain the red baseline.

## First-use HDR shader stall and focused repair (2026-10-26)

- A separate erased iOS 26.5 simulator ran a newly built App containing temporary timing probes in both the iOS bridge and `MaiGraphicsPresenter`. Before installation, `strings` confirmed the probes were present in the actual `MaiChat.debug.dylib`; the original test-bundle MOV had the same SHA-256 stated above. The `maichat_mobile` CMake library uses `-DNDEBUG` even when the Xcode App is Debug, so the Presenter probes had to be explicitly enabled for this temporary build.
- The first P010 frame entered the Presenter at monotonic time 581604446 ms. The graphics thread started `format_conversion.effect` compilation at 581604466 ms and finished at 581612769 ms: **8,303 ms**. FFplay continued submitting frames meanwhile; frame 200 was queued at 581611083 ms. The first actual render ended successfully at 581614591 ms, **10,145 ms** after it started. Already queued frames 2, 3, 100, 200 and 300 were then skipped because a newer generation had arrived. This directly explains the advancing playback clock with black video on first use. The P010 frame format and Metal renderer were working once compilation finished.
- The shared Presenter now selects `MaiAgent/Graphics/data/MaiP010HlgConversion.effect` only for P010/HLG. This derived resource contains the same OBS P010/HLG vertex shader, pixel shader, color helpers and uniform calculations as the unchanged full `format_conversion.effect`, but only one conversion technique. Other formats retain the full effect. iOS, Android and Desktop already package `MaiAgent/Graphics/data/*.effect` from one shared location.
- In a new erased simulator with the exact original, the focused effect compiled on first use in **144 ms**. The first actual P010 render succeeded **1,664 ms** after it started; subsequent renders took 1–9 ms. Screenshots taken by one launch-and-capture script at elapsed 3, 6, 9, 12, 17 and 20 seconds showed correctly oriented, moving video throughout the clip. Consecutive picture-region mean absolute RGB differences were 10.76–17.07, so the same frozen frame was not being shown. The old full-effect and focused-effect screenshots at the original clip's final frame were **pixel-identical across the entire 1320×2868 screen** (RGB MAE 0), including the video rectangle.
- Temporary `HDRTiming` instrumentation was removed from both source files. A clean incremental simulator build succeeded, and `strings` found no timing marker in its App binary. After erasing another isolated simulator and installing that clean build, the player was briefly black at launch elapsed 3 seconds while its UI still read **0:00**; by elapsed 6 seconds it showed an upright picture at **0:03**. Screenshots at elapsed 10, 17 and 20 seconds continued to show changing video. The clean log reported native P010 frame milestones 100/200/300/400 and no filter or shader error. Local screenshots and logs are under `/private/tmp/maichat-hdr-fast-*` and `/private/tmp/maichat-hdr-clean-*`; the user video is not in the repository.

**Remaining acceptance boundary:** The simulator first-use stall has been reduced from about 10 seconds of black with an advancing clock to a short startup interval before the first picture. The signed iPhone build must still be checked on the actual device; simulator evidence alone does not establish the real-device picture, audio, color or full UI behavior. A dedicated loading indicator for startup remains a possible UX improvement if real-device first-frame latency is noticeable.
