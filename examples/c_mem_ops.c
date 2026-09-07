/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file examples/c_mem_ops.c
 * @brief Copying and filling memory from plain C, with no call to the C
 *        library and -- where it matters -- no call at all.
 *
 * This file is compiled as **C, not C++**, on purpose.  It is the example and
 * the proof at the same time: the memory headers claim to be usable from C, and
 * the only way to check that claim is to compile them with a C compiler.  A
 * namespace, a `template`, an `<atomic>` slipping in, and this stops building.
 *
 * WHY THAT MATTERS HERE MORE THAN ELSEWHERE.  A C library could be handed a
 * C++ implementation behind a wrapper -- but then every copy would cost a call
 * across the language boundary, which is exactly the cost these routines exist
 * to remove.  For the C side to pay nothing, the C compiler has to SEE the
 * body.  So the headers are C, and C++ gets a thin `util::` wrapper on top,
 * not the other way round.
 *
 * The three entry points, and when to reach for each:
 *
 *   vesta_memcpy         general use; picks the best path for this CPU.
 *   vesta_memcpy_inline  never calls anybody, at the price of giving up AVX2.
 *   vesta_memmove        when the regions might overlap.
 */

#include "util/mem/vesta_memcpy.h"
#include "util/mem/vesta_memset.h"

#include <stdio.h>

/* A header of a fixed, compile-time-known size: this is the shape that
 * `vesta_memset_inline` turns into a couple of stores with no branch at all,
 * because the compiler expands a constant size itself. */
struct Header {
    uint64_t magic;
    uint32_t version;
    uint32_t flags;
};

int main(void) {
    unsigned char src[256];
    unsigned char dst[256];
    struct Header h;
    size_t i;
    int ok = 1;

    printf("== memory primitives from C ==\n\n");

    for (i = 0; i < sizeof src; ++i)
        src[i] = (unsigned char)(i * 7u + 1u);

    /* 1. La copia de uso general. */
    vesta_memcpy(dst, src, sizeof src);
    for (i = 0; i < sizeof src; ++i)
        if (dst[i] != src[i]) ok = 0;
    printf("  vesta_memcpy of %zu bytes .......... %s\n", sizeof src,
           ok ? "ok" : "FAILED");

    /* 2. La que no llama a nadie.  Mismo resultado, distinto codigo. */
    vesta_memset_inline(dst, 0, sizeof dst);
    for (i = 0; i < sizeof dst; ++i)
        if (dst[i] != 0) ok = 0;
    printf("  vesta_memset_inline, no call ...... %s\n", ok ? "ok" : "FAILED");

    /* 3. Tamano constante: lo expande el compilador, sin bucle ni rama. */
    vesta_memset_inline(&h, 0, sizeof h);
    h.magic = 0x56455354414C4C43ull;
    h.version = 1;
    printf("  header zeroed and stamped ......... %s\n",
           (h.flags == 0 && h.version == 1) ? "ok" : "FAILED");

    /* 4. Solape: mover el bloque un byte a la derecha sobre si mismo.  Con
     *    `vesta_memcpy` esto seria incorrecto -- pisaria la fuente antes de
     *    leerla --, y por eso existe la otra. */
    vesta_memcpy(dst, src, 64);
    vesta_memmove(dst + 1, dst, 63);
    for (i = 0; i < 63; ++i)
        if (dst[i + 1] != src[i]) ok = 0;
    printf("  vesta_memmove with overlap ........ %s\n", ok ? "ok" : "FAILED");

    printf("\n%s\n", ok ? "all good" : "something is wrong");
    return ok ? 0 : 1;
}
