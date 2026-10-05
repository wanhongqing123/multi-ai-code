#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The host serializes calls and limits commands before invoking this in-process entry. */
int mai_openssl_execute(int argc, char **argv);
void mai_openssl_set_output_sink(void (*sink)(void *opaque, const char *bytes,
                                              size_t length, int is_error),
                                  void *opaque);

#ifdef __cplusplus
}
#endif
