#ifndef LS_LOG_H
#define LS_LOG_H

#include <stdio.h>

/* DeaDBeeF's own trace output goes to stderr, so this lands in the same
 * place as the player's log and needs no separate file. */
#define LS_LOG(...)                                                            \
    do {                                                                       \
        fprintf(stderr, "[lyricscope] ");                                      \
        fprintf(stderr, __VA_ARGS__);                                          \
        fprintf(stderr, "\n");                                                 \
        fflush(stderr);                                                        \
    } while (0)

#endif /* LS_LOG_H */
