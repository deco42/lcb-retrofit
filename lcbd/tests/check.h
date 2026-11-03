#ifndef LCBD_CHECK_H
#define LCBD_CHECK_H

#include <stdio.h>

static int check_count, check_failure_count;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        check_count++;                                                                                                 \
        if (!(cond)) {                                                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                                   \
            check_failure_count++;                                                                                     \
        }                                                                                                              \
    } while (0)

static inline int check_report(const char *name)
{
    printf("%s: %d checks, %d failed\n", name, check_count, check_failure_count);

    return check_failure_count != 0;
}

#endif
