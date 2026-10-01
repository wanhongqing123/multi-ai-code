#ifndef MAI_FFTOOLS_EMBED_H
#define MAI_FFTOOLS_EMBED_H

#ifdef __cplusplus
extern "C" {
#endif

int mai_ffmpeg_execute(int argc, char **argv);
void mai_ffmpeg_set_cancel_check(int (*check)(void *opaque), void *opaque);
int mai_ffprobe_execute(int argc, char **argv);
void mai_ffprobe_set_cancel_check(int (*check)(void *opaque), void *opaque);
void mai_ffprobe_show_help_default(const char *opt, const char *arg);

#ifdef __cplusplus
}
#endif

#endif
