/* PS5 RetroArch - what a frontend executable's link needs beyond the payload SDK.
 *
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The compiler's builtins archive on this host (clang 23) is built with
 * _FORTIFY_SOURCE, so its emulated thread-locals (emutls.c, which SDL2 and the
 * OpenGL SDK pull in) call __memset_chk, which the console's libc does not have.
 * The check is the caller's buffer size; this keeps it.
 */
#include <stddef.h>
#include <string.h>

void abort(void);

void *__memset_chk(void *destination, int value, size_t length, size_t destination_length)
{
    if (length > destination_length)
        abort();
    return memset(destination, value, length);
}
