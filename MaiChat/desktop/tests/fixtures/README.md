# Video lifecycle fixture

`video-lifecycle.mp4` is a generated one-second blue H.264 baseline video, 160×90, 10 fps, no audio. It exercises opening/closing the video preview without depending on another product or submodule.

Regenerate with:

```sh
ffmpeg -f lavfi -i 'color=c=blue:s=160x90:r=10:d=1' -c:v libx264 -profile:v baseline -pix_fmt yuv420p -movflags +faststart -an video-lifecycle.mp4
```
