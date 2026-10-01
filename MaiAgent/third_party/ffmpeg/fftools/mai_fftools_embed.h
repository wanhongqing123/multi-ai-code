#ifndef MAI_FFTOOLS_EMBED_H
#define MAI_FFTOOLS_EMBED_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int mai_ffmpeg_execute(int argc, char **argv);
void mai_ffmpeg_set_cancel_check(int (*check)(void *opaque), void *opaque);
int mai_ffprobe_execute(int argc, char **argv);
void mai_ffprobe_set_cancel_check(int (*check)(void *opaque), void *opaque);
void mai_ffprobe_show_help_default(const char *opt, const char *arg);
void mai_fftools_set_log_sink(void (*sink)(void *opaque, const char *line), void *opaque);
int mai_fftools_error_string(int code, char *buffer, size_t size);

#ifdef __cplusplus
}
#endif

#endif
