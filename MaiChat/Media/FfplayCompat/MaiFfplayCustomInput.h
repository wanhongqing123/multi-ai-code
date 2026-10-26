#pragma once

#include "MaiFfplayHost.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif
#include "libavformat/avio.h"
#include "libavutil/error.h"
#include "libavutil/mem.h"
#ifdef __cplusplus
}
#endif

// Shared FFmpeg custom-AVIO adapter for VOD playback and HDR preflight.
// It owns only the AVIO allocation. The host owns the underlying byte source.
typedef struct MaiFfplayCustomInput {
    AVIOContext* avio;
    MaiFfplayHost host;
    uint64_t offset;
} MaiFfplayCustomInput;

static int maiFfplayCustomRead(void* opaque, uint8_t* output, int capacity) {
    MaiFfplayCustomInput* input = (MaiFfplayCustomInput*)opaque;
    if (!input || !input->host.read_input_at || capacity <= 0) return AVERROR(EINVAL);
    if (input->offset >= input->host.input_size_bytes) return AVERROR_EOF;
    const uint64_t left = input->host.input_size_bytes - input->offset;
    if (left < (uint64_t)capacity) capacity = (int)left;
    const int count = input->host.read_input_at(input->host.user_data,
                                                input->offset, output, capacity);
    if (count < 0 || count > capacity) return AVERROR(EIO);
    if (count == 0) return AVERROR_EOF;
    input->offset += (uint64_t)count;
    return count;
}

static int64_t maiFfplayCustomSeek(void* opaque, int64_t offset, int whence) {
    MaiFfplayCustomInput* input = (MaiFfplayCustomInput*)opaque;
    if (!input) return AVERROR(EINVAL);
    whence &= ~AVSEEK_FORCE;
    if (whence == AVSEEK_SIZE) return (int64_t)input->host.input_size_bytes;
    uint64_t base = 0;
    if (whence == SEEK_CUR) base = input->offset;
    else if (whence == SEEK_END) base = input->host.input_size_bytes;
    else if (whence != SEEK_SET) return AVERROR(EINVAL);
    if (offset >= 0) {
        if ((uint64_t)offset > input->host.input_size_bytes - base)
            return AVERROR(EINVAL);
        input->offset = base + (uint64_t)offset;
    } else {
        // Avoid negating INT64_MIN, whose positive value is not representable.
        const uint64_t backwards = (uint64_t)(-(offset + 1)) + 1;
        if (backwards > base) return AVERROR(EINVAL);
        input->offset = base - backwards;
    }
    return (int64_t)input->offset;
}

static MaiFfplayCustomInput* maiFfplayCustomInputCreate(const MaiFfplayHost* host) {
    if (!host || !host->read_input_at || !host->input_size_bytes ||
        host->input_size_bytes > INT64_MAX) return NULL;
    MaiFfplayCustomInput* input =
        (MaiFfplayCustomInput*)av_mallocz(sizeof(MaiFfplayCustomInput));
    if (!input) return NULL;
    input->host = *host;
    const int bufferSize = 64 * 1024;
    uint8_t* buffer = (uint8_t*)av_malloc(bufferSize);
    if (!buffer) {
        av_free(input);
        return NULL;
    }
    input->avio = avio_alloc_context(buffer, bufferSize, 0, input,
                                     maiFfplayCustomRead, NULL,
                                     maiFfplayCustomSeek);
    if (!input->avio) {
        av_free(buffer);
        av_free(input);
        return NULL;
    }
    input->avio->seekable = AVIO_SEEKABLE_NORMAL;
    return input;
}

static void maiFfplayCustomInputDestroy(MaiFfplayCustomInput** input) {
    if (!input || !*input) return;
    avio_context_free(&(*input)->avio);
    av_freep(input);
}
