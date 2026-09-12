/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file tests/alloc/test_chunk_reclaim.cpp
 * @brief That giving a chunk back gives back the RIGHT chunks, and only those.
 *
 * WHAT MUST NOT BREAK.  A chunk handed to a size class never goes back on its
 * own, so one whose blocks all died is 64 KiB nobody can use -- measured on a
 * compile of 144.000 lines, 386 MiB of them.  `host_chunk_reclaim` hands those
 * back to the span pool.
 *
 * The danger is not that it reclaims too little: that only leaves memory where
 * it already was.  It is that it reclaims a chunk still holding something, and
 * THAT does not fail anywhere near here -- the block is handed to somebody else
 * and two owners write over each other, somewhere else entirely, much later.
 *
 * So the two checks that matter are the one with a block alive in every
 * sixteen, and the one that releases ACROSS THREADS: a block freed by a thread
 * that does not own its chunk lands in the owner's queue instead of its list,
 * and that is the case `bench_allocator` does not exercise at all -- it is
 * where an earlier attempt at counting live blocks quietly corrupted memory.
 */
#include "util/alloc/host_allocator.h"

#include <cstdio>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char *what) {
    std::printf("  [%s] %s\n", ok ? "OK  " : "FAIL", what);
    if (!ok) ++failures;
}

} // namespace

int main() {
    std::printf("[test_chunk_reclaim]\n");
    /* Por encima de `kReclaimFromClass`, que es donde el barrido mira: debajo
     * no reclamaria nada y la prueba pasaria sin probar nada. */
    const size_t n = 4096;
    std::vector<void *> v;

    for (int i = 0; i < 2000; ++i)
        v.push_back(util::host_alloc(n));
    for (void *p : v)
        util::host_free(p);
    v.clear();

    uint64_t blocks = 0, chunks = 0;
    const size_t seen =
        util::host_chunk_scan(util::kReclaimFromClass, &blocks, &chunks);
    check(seen > 0, "con todo liberado, el barrido ve trozos vacios");

    const size_t given = util::host_chunk_reclaim();
    check(given > 0, "y el reclamo los devuelve");
    check(given == seen, "exactamente los que habia visto");

    uint64_t b2 = 0, c2 = 0;
    check(util::host_chunk_scan(util::kReclaimFromClass, &b2, &c2) == 0,
          "y despues ya no queda ninguno");

    // Lo devuelto tiene que seguir sirviendo: volvio al reparto de tramos y se
    // parte en bloques otra vez como cualquier otro.
    bool ok = true;
    for (int i = 0; i < 2000; ++i) {
        void *p = util::host_alloc(n);
        if (p == nullptr) {
            ok = false;
            break;
        }
        *static_cast<unsigned char *>(p) = 0x5a;
        v.push_back(p);
    }
    check(ok, "y la memoria devuelta se vuelve a servir y se puede escribir");
    for (void *p : v)
        util::host_free(p);
    v.clear();

    // UN SOLO BLOQUE VIVO BASTA para que su trozo no se vaya.  Ver la cabecera.
    std::vector<void *> alive;
    for (int i = 0; i < 2000; ++i)
        v.push_back(util::host_alloc(n));
    for (size_t i = 0; i < v.size(); ++i) {
        if (i % 16 == 0)
            alive.push_back(v[i]);
        else
            util::host_free(v[i]);
    }
    v.clear();
    util::host_chunk_reclaim();
    ok = true;
    for (void *p : alive) {
        *static_cast<unsigned char *>(p) = 0xa5;
        if (util::host_usable_size(p) < n) ok = false;
    }
    check(ok, "un solo bloque vivo impide que su trozo se vaya");
    for (void *p : alive)
        util::host_free(p);
    alive.clear();

    // Y CON LIBERACIONES CRUZADAS.  Ver la cabecera.
    std::vector<void *> theirs(4000, nullptr);
    std::thread producer([&theirs, n] {
        for (size_t i = 0; i < theirs.size(); ++i)
            theirs[i] = util::host_alloc(n);
    });
    producer.join();
    std::thread consumer([&theirs] {
        for (void *p : theirs)
            util::host_free(p);
        util::host_chunk_reclaim();
    });
    consumer.join();
    util::host_chunk_reclaim();
    void *q = util::host_alloc(n);
    check(q != nullptr, "y con liberaciones cruzadas sigue entero");
    util::host_free(q);

    std::printf("[test_chunk_reclaim] %s\n",
                failures == 0 ? "TODO OK" : "CON FALLOS");
    return failures == 0 ? 0 : 1;
}
