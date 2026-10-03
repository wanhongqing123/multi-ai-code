# ffplay 9.0.2 parity checklist

Source of truth: `MaiAgent/third_party/ffmpeg/fftools/ffplay.c`. This file tracks
the shared in-process player and its MaiChat/Agent controls. A check mark needs
runtime evidence on macOS, Windows, iOS, and Android; compilation alone is not
acceptance. The existing `MaiVideoPlayback` is a video-only starting point.

| ffplay behavior | Shared engine | Agent command | Mac | Windows | iOS | Android |
|---|---|---|---|---|---|---|
| Local file and network input, format/decoder options | | | | | | |
| Audio, video, and subtitle stream selection/cycling | | | | | | |
| Packet/frame queues and serial invalidation on seek | | | | | | |
| Audio/video/external clocks and synchronization | | | | | | |
| Audio output, mute, and volume | | | | | | |
| Pause/resume, frame step, relative/absolute/chapter seek | | | | | | |
| Loop count, end/autoexit, duration/start position | | | | | | |
| Video/audio filter graphs and filter cycling | | | | | | |
| Subtitle decoding, timing, blending, and track changes | | | | | | |
| Video rendering, aspect ratio, autorotation, frame drop | | | | | | |
| Audio waves/RDFT visualization and show-mode cycling | | | | | | |
| Fullscreen, window size/position, border, on-top | | | | | | |
| Hardware decode selection and software fallback | | | | | | |
| Status, error, cancellation, repeated use without process exit | | | | | | |

The cross-platform contract separates shared playback state and commands
from native view and audio-device handles. Each platform supplies a Graphics
surface and a native audio device without starting a child process or linking
SDL. `MaiChat/Media/ffplay.c` must remain byte-identical to FFmpeg's original;
the compatibility layer maps its platform calls to MaiChat. Agent commands
must exercise the same playback session as MaiChat's own controls.

## iOS picture quality: 2026-10-03 device evidence

- On iPhone 14 Pro Max, the video popup initially requested a 430×839 Metal
  drawable for a 430×839-point view, even though the screen scale is 3×. UIKit
  enlarged that 1× output. The iOS host view now requests 1290×2517 pixels;
  real-device logs confirmed `contentScale=3.0`, `screenScale=3.0` and a
  1080×1920 source frame. The sampled playback status reported `fd=0` before
  and after the change. The normal popup and Agent playback tool use this same
  host view.
- A sampled gallery MOV is HEVC Main10, BT.2020 HLG with a Dolby Vision
  Profile 8 configuration record. The ordinary SDL texture route advertises
  8-bit formats and presents I420/BT.709/limited, losing source precision before
  Graphics. For local HDR HEVC without subtitle streams, the iOS host now uses
  FFplay's existing alternate AVFrame renderer entry point and VideoToolbox
  decode. Graphics receives P010/BT.2020, runs the existing OBS HLG/PQ
  conversion shader, and tone-maps to the app's SDR Metal swapchain. Other
  files retain the ordinary SDL route. The upstream `ffplay.c` remains intact.
- Real-device testing on the 29.6-second 1080×1920 HLG sample confirmed about
  800 accepted frames and 800 Graphics-rendered frames, `fd=0`, no shader
  errors, accepted pause/play controls, and a finite 50% seek. Dolby Vision
  dynamic metadata and HDR/EDR display output are not implemented; visual
  parity with Photos still requires comparison on the same device. HDR files
  with subtitle streams currently retain the 8-bit compatibility route.
- `SWS_BILINEAR` in `MaiGraphicsPresenter::decodeImage` is used for image
  decoding, not this video draw path. The video draw path currently samples
  `default.effect` linearly. Compare the same source in Photos and the normal
  MaiChat popup before changing scaling filters; preserve the upstream
  `ffplay.c` constraint above.
