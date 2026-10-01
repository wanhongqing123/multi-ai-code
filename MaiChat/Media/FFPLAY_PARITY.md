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
