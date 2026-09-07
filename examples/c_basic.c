/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file examples/c_basic.c
 * @brief Using the allocator from plain C.
 *
 * This file is compiled as **C, not C++**, on purpose.  It is the example and
 * the proof at the same time: if `util/alloc/host_allocator_c.h` ever stops being
 * valid C -- a stray `bool`, a default argument, a namespace -- this stops
 * building, and the promise the header makes is checked instead of claimed.
 *
 * The four functions map one-to-one onto `malloc`, `calloc`, `realloc` and
 * `free`, so porting existing C code is a rename.  There is a fifth,
 * `vesta_host_usable_size`, which has no equivalent in the standard: it tells
 * you how much you REALLY got, which is often more than you asked for.
 */

#include "util/alloc/host_allocator_c.h"
#include "util/mem/vesta_memset.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    printf("== using the allocator from C ==\n\n");

    /* 1. Plain allocate and free. */
    char *buf = (char *)vesta_host_alloc(100);
    if (buf == NULL) {
        printf("out of memory\n");
        return 1;
    }
    vesta_memset(buf, 'x', 100);
    printf("asked for 100 bytes, actually got %zu\n",
           vesta_host_usable_size(buf));
    printf("  (rounding up to a size class is not waste if you use it)\n");

    /* 2. Growing.  Note the careful shape: if realloc fails, the OLD pointer is
       still valid and still yours to free.  Assigning straight to `buf` would
       leak it -- the classic realloc mistake. */
    char *bigger = (char *)vesta_host_realloc(buf, 5000);
    if (bigger == NULL) {
        vesta_host_free(buf); /* buf survived the failure */
        printf("could not grow\n");
        return 1;
    }
    buf = bigger;
    printf("grew to 5000, first byte still '%c' (contents preserved)\n",
           buf[0]);

    /* 3. Zeroed memory. */
    int *counters = (int *)vesta_host_calloc(256, sizeof(int));
    if (counters == NULL) {
        vesta_host_free(buf);
        return 1;
    }
    printf("calloc gave %d zeroed ints, counters[100] = %d\n", 256,
           counters[100]);

    /* 4. Saying what an allocation is FOR.  There are no destructors in C, so
       restoring the previous tag is your job -- and it matters: a tag left in
       place quietly counts everything that comes afterwards under the wrong
       heading, and the report still looks plausible. */
    {
        const unsigned previous = vesta_host_push_tag(1 /* instant */,
                                                      1 /* fixed */);
        void *scratch = vesta_host_alloc(64);
        vesta_host_free(scratch);
        vesta_host_pop_tag(previous);
        printf("tagged one allocation as instant/fixed\n");
    }

    vesta_host_free(counters);
    vesta_host_free(buf);
    vesta_host_free(NULL); /* freeing NULL is fine, like free() */

    printf("\nRun with VESTA_HOST_ALLOC_STATS=1 to see the summary at exit.\n");
    return 0;
}
