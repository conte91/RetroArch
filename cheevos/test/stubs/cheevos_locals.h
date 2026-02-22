/* Test stub: replaces cheevos_locals.h for unit tests of cheevos_cache.c.
 * Provides only the macros that cheevos_cache.c actually uses from the
 * real header, without pulling in rcheevos, task_queue, rthreads, etc. */

/* Use the real header's guard so -include of this file blocks the real
 * cheevos_locals.h from loading (the preprocessor skips it on the guard check). */
#ifndef __RARCH_CHEEVOS_LOCALS_H
#define __RARCH_CHEEVOS_LOCALS_H

#include <stdio.h>

#define RCHEEVOS_STRINGIFY2(x) #x
#define RCHEEVOS_STRINGIFY(x)  RCHEEVOS_STRINGIFY2(x)
#define RCHEEVOS_TAG "[RCHEEVOS]: (" __FILE__ ":" RCHEEVOS_STRINGIFY(__LINE__) ") "

#define CHEEVOS_LOG(...) fprintf(stderr, __VA_ARGS__)
#define CHEEVOS_ERR(...) fprintf(stderr, __VA_ARGS__)

#endif /* __RARCH_CHEEVOS_LOCALS_H */
