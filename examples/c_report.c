/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file examples/c_report.c
 * @brief Asking for the report, and naming your own symbols, from C.
 *
 * WHY THIS EXAMPLE EXISTS.  Because "the allocator knows how to report" would
 * be worth very little if the report were a C++ feature.  A program written in
 * C -- or a C library carried inside a bigger project -- allocates through
 * `util/alloc/host_allocator_c.h` and is therefore already counted; this is the
 * other half: asking for that measurement as data, and saying what a name of
 * ITS language looks like.
 *
 * And it is compiled AS C on purpose.  That is the only thing that keeps
 * `util/report/alloc_csv_c.h` honest: the day someone puts a `bool`, a default
 * argument or a namespace in it, this stops building instead of the breakage
 * waiting for a user in C to find it.
 *
 * The two hooks are separate questions and it shows here:
 *
 *   - the RESOLVER answers "whose address is this" -- it reads symbols, and a
 *     program that has none says so by returning zero, which is a legitimate
 *     answer;
 *   - the FORMATTER answers "how is that written in my language".  C does not
 *     mangle, but it does have prefixes that are noise in a report, and only
 *     the project knows which ones.
 */

#include "util/report/alloc_csv_c.h"
#include "util/alloc/host_allocator_c.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 *  Two functions that allocate, so the report has something to report.
 * ------------------------------------------------------------------------- */

/* Plain `malloc` and `calloc`, on purpose.  Those ARE this allocator -- the C
 * entry points are redirected at link time -- so this is what the code of a C
 * library carried inside a project looks like: unchanged, unaware, and
 * measured.  `vesta_host_alloc` still exists for wiring a library up through
 * its own hook (see `c_library_hook.c`); it is not what a caller has to reach
 * for to be counted. */
static void *vesta_example_parse(size_t n) {
    return malloc(n);
}

static void *vesta_example_index(size_t n) {
    return calloc(n, sizeof(int));
}

/* ---------------------------------------------------------------------------
 *  The formatter.  In C there is nothing to demangle, and that is exactly why
 *  it is worth showing: the hook is not "the demangler", it is "how MY names
 *  are written".  Here it trims the prefix every symbol in this file carries,
 *  which is noise once you already know which program you are looking at.
 *
 *  The buffer is static because the contract says so: the returned pointer is
 *  valid until the next call, like `strerror`.  Keeping every name alive would
 *  mean holding thousands of strings for a report written once.
 * ------------------------------------------------------------------------- */

static const char *trim_prefix(const char *raw) {
    static char out[256];
    const char *prefix = "vesta_example_";
    size_t n = strlen(prefix);
    if (raw == NULL) return NULL;
    if (strncmp(raw, prefix, n) != 0) {
        /* NULL, not a copy: it means "I have nothing to add", and the library
         * then keeps the raw name.  Returning something worse than what came
         * in is the one thing a formatter must not do. */
        return NULL;
    }
    snprintf(out, sizeof(out), "%s", raw + n);
    return out;
}

/* ---------------------------------------------------------------------------
 *  The resolver.  A real one reads the symbol table or the debug info of the
 *  running image; this one stands in for it with a table, so the example
 *  builds anywhere, and shows the shape: innermost frame first, `inlined` set
 *  on every frame that was folded into the NEXT one, and `module` saying whose
 *  code it is -- which the project answers, because what counts as a module is
 *  a property of its own tree.
 * ------------------------------------------------------------------------- */

static unsigned fake_resolver(const void *pc, VestaAllocFrame *out,
                              unsigned max) {
    if (max < 2) return 0;
    /* Innermost: where the allocation physically happens. */
    out[0].function = "vesta_example_parse";
    out[0].file = "examples/c_report.c";
    out[0].line = 46;
    out[0].inlined = 1;              /* folded into the caller below */
    out[0].module = "examples";
    /* Outermost: the function that exists in the binary. */
    out[1].function = "main";
    out[1].file = "examples/c_report.c";
    out[1].line = 120;
    out[1].inlined = 0;
    out[1].module = "examples";
    (void)pc;
    return 2;
}

/**
 * @brief Donde van a parar los punteros, para que el bucle EXISTA.
 *
 * Sin esto no se mide nada, y no se nota: `malloc` y `free` no son funciones
 * cualesquiera para el compilador -- conoce lo que hacen --, asi que un par
 * cuyo resultado nadie lee lo BORRA entero, y eso vale igual con las llamadas
 * redirigidas al enlazar.  El programa sigue saliendo con cero, el informe
 * sale vacio, y lo que parece es que el asignador no ve nada.
 *
 * Pasando el puntero por un `volatile` el compilador ya no puede demostrar que
 * sobra, y el bucle se ejecuta.  Es la misma trampa que se comio el bucle de
 * calentamiento del banco de `operator new`.
 */
static void *volatile g_sink;

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "alloc_report_c";
    void *a;
    void *b;
    int i;

    /* Something to measure.  The tag says what it is FOR, so the report can
     * split "short-lived scratch" from "lives to the end". */
    for (i = 0; i < 64; ++i) {
        unsigned previous = vesta_host_push_tag(1 /* instant */, 1 /* fixed */);
        a = vesta_example_parse(128);
        vesta_host_pop_tag(previous);
        b = vesta_example_index(32);
        g_sink = a;              /* ver `g_sink`: sin esto no hay bucle */
        g_sink = b;
        free(a);
        free(b);
    }

    vesta_alloc_set_symbol_resolver(fake_resolver);
    vesta_alloc_set_name_formatter(trim_prefix);

    printf("a name through the formatter: %s\n",
           vesta_alloc_readable_name("vesta_example_parse"));
    printf("one it does not know stays as it was: %s\n",
           vesta_alloc_readable_name("memcpy"));

    if (!vesta_alloc_write_csv(dir)) {
        /* Said, and with a failing exit code.  A report that was asked for and
         * did not happen must not finish quietly: the run is over and there is
         * nothing left to look at. */
        fprintf(stderr, "the report could not be written into '%s'\n", dir);
        return 1;
    }
    printf("wrote sites.csv, frames.csv, summary.csv, sizes.csv and tags.csv "
           "into '%s'\n", dir);
    printf("now walk it:  python -m alloc_tree %s\n", dir);
    return 0;
}
