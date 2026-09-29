#pragma once

// The copied graphics-ffmpeg.c includes OBS's top-level compatibility header.
// It only needs the FFmpeg declarations here; none of that header's macros are
// referenced by this source file.
#include <libavcodec/avcodec.h>
