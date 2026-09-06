/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file examples/c_library_hook.c
 * @brief Plugging this allocator into a C library, which is the real reason
 *        the C layer exists.
 *
 * THE PROBLEM.  A C++ program that links this library gets everything routed
 * through it automatically, because `operator new` is replaced.  C libraries do
 * not use `operator new` -- they call `malloc` -- so their memory bypasses it
 * entirely and shows up in no counter at all.  In the compiler this came from,
 * that blind spot covered Capstone, SQLite, OpenSSL and miniz; the compression
 * of cached artifacts alone is 5.4% of the instructions of a cold build, and
 * how much memory it used was simply unknown.
 *
 * THE FIX.  Most C libraries let you replace their allocator, and they all want
 * the same thing: four function pointers with the signatures of `malloc`,
 * `calloc`, `realloc` and `free`.  That is exactly what this layer provides, so
 * wiring it up is a struct literal.
 *
 * Here the library is simulated -- so this example builds anywhere without
 * vendoring anything -- but the shape is the real one.  Below the simulation
 * are the actual incantations for four libraries you are likely to have.
 */

#include "util/host_allocator_c.h"
#include "util/vesta_memset.h"

#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 *  A stand-in for a C library that lets you swap its allocator.
 *  Same shape as cs_opt_mem (Capstone), sqlite3_mem_methods, and friends.
 * ------------------------------------------------------------------------- */

typedef struct {
    void *(*malloc_fn)(size_t);
    void *(*calloc_fn)(size_t, size_t);
    void *(*realloc_fn)(void *, size_t);
    void (*free_fn)(void *);
} lib_mem_hooks;

static lib_mem_hooks g_hooks = {NULL, NULL, NULL, NULL};

static void lib_set_allocator(const lib_mem_hooks *hooks) {
    g_hooks = *hooks;
}

/// Some work the "library" does, allocating as it goes.
static int lib_do_work(int n) {
    int i;
    char **rows = (char **)g_hooks.calloc_fn((size_t)n, sizeof(char *));
    if (rows == NULL) return -1;
    for (i = 0; i < n; ++i) {
        rows[i] = (char *)g_hooks.malloc_fn(128);
        if (rows[i] == NULL) return -1;
        vesta_memset(rows[i], 0, 128);
    }
    for (i = 0; i < n; ++i)
        g_hooks.free_fn(rows[i]);
    g_hooks.free_fn(rows);
    return n;
}

int main(void) {
    lib_mem_hooks hooks;
    int done;

    printf("== plugging the allocator into a C library ==\n\n");

    /* Four assignments.  That is the whole integration. */
    hooks.malloc_fn = vesta_host_alloc;
    hooks.calloc_fn = vesta_host_calloc;
    hooks.realloc_fn = vesta_host_realloc;
    hooks.free_fn = vesta_host_free;
    lib_set_allocator(&hooks);

    /* And now the library's memory can be labelled like anything else, which
       is the point: you can finally ask what a given library costs you. */
    {
        const unsigned previous = vesta_host_push_tag(2 /* medium */,
                                                      1 /* fixed */);
        done = lib_do_work(500);
        vesta_host_pop_tag(previous);
    }

    if (done < 0) {
        printf("the library ran out of memory\n");
        return 1;
    }
    printf("the library did %d units of work, and every byte it asked for\n"
           "went through our allocator and into our counters.\n", done);

    printf("\nReal libraries, for reference:\n\n");
    printf("  Capstone   cs_opt_mem m = { vesta_host_alloc, vesta_host_calloc,\n"
           "                             vesta_host_realloc, vesta_host_free,\n"
           "                             vsnprintf };\n"
           "             cs_option(handle, CS_OPT_MEM, (size_t)&m);\n\n");
    printf("  SQLite     sqlite3_config(SQLITE_CONFIG_MALLOC, &methods);\n\n");
    printf("  OpenSSL    CRYPTO_set_mem_functions(a, r, f);\n\n");
    printf("  zlib/miniz stream.zalloc = ...; stream.zfree = ...;\n\n");
    printf("Check what the version you actually vendored exposes rather than\n"
           "assuming these are available.\n");

    printf("\nRun with VESTA_HOST_ALLOC_STATS=1 to see the breakdown.\n");
    return 0;
}
