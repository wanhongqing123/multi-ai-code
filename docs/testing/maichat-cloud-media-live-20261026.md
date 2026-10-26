# MaiChat private cloud media: 2026-10-26 live acceptance

Source state: repository baseline `8ac971d3104ae447f965e8fb6ea0c0d479e4f248`
plus local, uncommitted changes. This record distinguishes code tests, public
service behavior, isolated client packages, and the user's installed apps.

## Scope and live service

- The account-private drive accepts MP4/MOV video up to 2,000,000,000 bytes
  and JPEG/PNG/WebP/HEIC images up to 20,000,000 bytes, sharing a
  10,000,000,000-byte account quota. Clients upload file bytes directly to the
  separate private Guangzhou OSS bucket through signed multipart requests.
  The account service stores metadata and signs 60-second original and
  thumbnail reads; it does not proxy media bytes.
- The versioned production auth and signer source is
  `/opt/maichat/releases/cloud-drive-image-20261025T1700Z-P8k4m`.
  The account feature flag is enabled for signed-in accounts. The public
  privacy policy at `https://ichat.life/legal/privacy.html` was updated on
  2026-10-26 to cover pictures, videos and embedded source metadata; a public
  HTTPS read from ECS returned 200 with the new paragraph.
- Before migration, a root-only live SQLite backup was taken under
  `/opt/maichat/backups/pre-cloud-image-20261025T1700Z-P8k4m`.
  Applying the image CHECK-constraint migration to its isolated clone kept all
  seven cloud rows byte-for-byte, `quick_check=ok` and foreign-key errors 0.
  After switching production, the user's one ready video row matched its
  pre-switch hash; OSS HEAD matched its original size and MIME type. The six
  older deleted test rows remain reserved for their 24-hour race hold and must
  be observed after expiry before claiming final metadata purge.

## Real OSS and account checks

- A disposable probe directly exercised private signed thumbnails for H.264
  MP4, PNG and HEIC. A separate synthetic HEVC Main10/HLG/−180° MP4 also
  returned JPEG. These probes do **not** establish support for every video
  codec or the user's Dolby Vision MOV. Each probe's objects, multipart IDs
  and recovery journal were cleaned.
- An iOS simulator with disposable account B completed real JPEG upload,
  private thumbnail display, original preview and deletion. Its final account
  DB rows, OSS objects and incomplete uploads were independently read back as
  zero; `quick_check=ok`.
- Normal Android App on a 16 KB emulator with disposable account A completed
  real PNG and H.264 upload, both thumbnails, image preview, video playback,
  local playback-cache cleanup and deletion. Its final account list/quota
  returned to the initial values. Root independently found no new DB rows or
  OSS objects/uploads; A's six older deletion holds remain unchanged.
- A Mac Desktop UI test with disposable B completed real H.264 MP4, JPEG and
  HEIC upload, thumbnail display, original preview/playback and deletion.
  Root independently found B DB rows, OSS objects and uploads all zero.
- Another signed-in test account could not obtain the user's original or
  thumbnail link: both public API requests returned 404.

## Client packages and limits of evidence

| Client | Exact package / evidence | Verification boundary |
|---|---|---|
| iOS | `MaiChat/ios/MaiChat/dist-cloud-image-v123/MaiChat-iOS-2.2.3-123-cloud-image-dev.ipa`, 263,123,606 B, SHA-256 `8633944ead24239f7192dc80cc43c906578da1d3a26ace5eb7e04516931ae27c` | Signed and archive-verified, simulator local/real-account UI and Agent tests passed. Not installed on user's phone; it still has 2.2.3 (122). PhotosPicker system-grid selection was visually inspected but not reliably automated. |
| Mac Desktop | `MaiChat/desktop/dist-cloud-image-v2226/MaiChat-macos-arm64-2.2.26-cloud-image-test.zip`, 142,598,107 B, SHA-256 `d62a93d1f2735caacb154a8067bd5ad711fe45c1aa4bd009276b0737dbb65652` | Extracted App version 2.2.26, deep codesign, exact binary SHA and native smoke passed. Real B UI passed. Existing running App was not replaced. |
| Android | `MaiChat/android/MaiChat/dist-cloud-image-normal-v233/MaiChat-android-normal-debug-2.2.3-233.apk`, 174,466,903 B, SHA-256 `7e4b6ea9bbf943cac600d3517bea5b4f2d913a769136ea1eef2698e013602dd1` | Normal App on 16 KB emulator passed real A flow. This uses a debug certificate and cannot replace the prior release-signed installation; no Android phone test. |

The Windows build machine is unreachable: its configured bind address is no
longer present locally, and two alternate configured addresses close before
SSH authentication. No Windows image-cloud package or target-platform test is
claimed. App Store Connect and Play Console privacy/data-safety responses were
not changed in this run. No App Store, Play Store or GitHub release was made.

OSS [video snapshots](https://www.alibabacloud.com/help/en/oss/user-guide/video-snapshots)
support H.264/H.265; an initial Android canary used an
MPEG-4 Part 2 (`mp4v`) test clip and correctly received `BadRequest: not
supported format.` The PNG thumbnail had succeeded; the test was changed to
an H.264 clip and passed. Unsupported video codecs show a placeholder icon.
See `docs/diagnostics/maichat-android-cloud-thumbnail-codec-canary-20261026.md`.

For the user-reported HEVC Main10/HLG/Dolby Vision/−180° MOV that does not
play, the original 18,452,323-byte file was later supplied and tested only in
an isolated iOS 26.5 simulator build 123. Its −180° automatic rotation adds
an FFplay flip filter that cannot connect to the VideoToolbox frame path;
the new zero-frame software fallback displayed the original picture. A strict
matrix-only removal kept all seven tracks and the same audio/video packets,
eliminated the filter error and displayed native frames. Removing only the five
`mebx` tracks did not eliminate the native error. The user's actual iPhone
still has build 122, so signed build 123 must be installed and tested there
before claiming the on-device incident fixed. See
`docs/diagnostics/maichat-ios-hlg-rotated-mov-playback-20261026.md`.
