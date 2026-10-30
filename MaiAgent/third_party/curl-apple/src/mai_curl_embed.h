#pragma once

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The caller serializes executions. This entry returns an exit status and never spawns a
 * subprocess; the host must still validate arguments and capture output before exposing it. */
int mai_curl_execute(int argc, char *argv[]);
void mai_curl_set_cancel_check(int (*check)(void *), void *opaque);
int mai_curl_cancel_requested(void);
void mai_curl_set_error_file(FILE *stream);

#ifdef __cplusplus
}
#endif
