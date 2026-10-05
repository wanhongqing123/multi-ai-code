#include "mai_fftools_embed.h"

#include <stdarg.h>

#include "libavutil/error.h"
#include "libavutil/log.h"

static void (*mai_log_sink)(void *opaque, const char *line);
static void *mai_log_opaque;

static void mai_log_callback(void *context, int level, const char *format, va_list arguments)
{
    if (!mai_log_sink || level > AV_LOG_INFO)
        return;

    char line[1024];
    int print_prefix = 1;
    av_log_format_line2(context, level, format, arguments, line, sizeof(line), &print_prefix);
    mai_log_sink(mai_log_opaque, line);
}

void mai_fftools_set_log_sink(void (*sink)(void *opaque, const char *line), void *opaque)
{
    mai_log_opaque = opaque;
    mai_log_sink = sink;
    av_log_set_callback(sink ? mai_log_callback : av_log_default_callback);
}

int mai_fftools_error_string(int code, char *buffer, size_t size)
{
    return av_strerror(code, buffer, size);
}
