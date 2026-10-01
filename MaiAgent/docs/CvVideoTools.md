# Video analysis tools

`cv_scene_detect` and `cv_motion_detect` are MaiAgent tools backed by one
streaming FFmpeg decoder and OpenCV analysis implementation. MaiChat registers
both factories on iOS, Android, macOS, and Windows after linking the same
vendored OpenCV source. The direct `analyzeMaiCvVideo` entry point is also
available to non-Agent callers.

Both tools accept a local video `path` under the Agent's configured file access
boundary. They only read the input. To inspect a system gallery video, first use
`mobile_request_permission` for `photos` if needed, then
`mobile_export_media_original` and pass its exported path to a `cv_` tool.

| Tool | Default sampling | Default threshold | Return |
|---|---:|---:|---|
| `cv_scene_detect` | 0.5 s | 0.45 HSV histogram distance | Shot `segments` |
| `cv_motion_detect` | 0.25 s | 0.015 changed-pixel ratio | Motion `segments` |

Both return `{ "segments": [{"start_s": 0, "end_s": 1}], "duration_s": ..., "sampled_frames": ..., "decoded_frames": ..., "total_segments": ..., "truncated": ... }`.
`max_segments` defaults to 500 and may be raised to 2000; an omitted tail is
reported with `truncated: true`. An Agent tool result also marks truncation.

`cv_scene_detect` compares downscaled 3-channel HSV histograms, including
brightness. `cv_motion_detect` compares blurred grayscale frames after a pixel
threshold and morphological cleanup. Its output means visible change; camera
motion, lighting, and scene cuts can also produce intervals. It does not identify
people or objects. Sampling and thresholds are tunable through the tool schema.

The decoder retains one frame and one small analysis image at a time. Canceling
a tool turn stops the decode loop. The regression fixture contains a black to
white cut at 1 s, a second cut at 2 s, and moving imagery through 4 s; tests
exercise the direct entry point and the actual Agent tool adapters.
