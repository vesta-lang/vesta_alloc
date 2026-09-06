/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file bench/bench_vs_malloc.cpp
 * @brief Head to head against malloc/calloc/realloc, in one process.
 *
 * WHY A SECOND BENCHMARK.  `bench_allocator` compares by re-running the same
 * binary with `VESTA_NO_HOST_SLAB=1`.  That is useful -- it measures the whole
 * program end to end -- but it has two weaknesses as a comparison: the system
 * allocator is reached THROUGH our wrapper, and the two numbers come from
 * different runs, so anything that drifts between them (CPU frequency, page
 * cache, another process waking up) lands on the difference.
 *
 * Here both allocators are called directly, in the same process, back to back.
 *
 * INTERLEAVED, AND IN BOTH ORDERS.  Every measurement runs A-B-B-A and averages
 * each side.  Two runs in a row do not distinguish a change from drift: if the
 * machine speeds up or slows down during the benchmark, a plain A-then-B blames
 * the allocator for it.  A-B-B-A cancels a linear drift.
 *
 * WHAT IS MEASURED, and why each one is here:
 *
 *   1. hot     allocate and free immediately.  Best case: the block comes
 *              straight back off the free list, still in cache.
 *   2. burst   allocate a batch, then free the batch.  A compiler's usual
 *              shape, and where size classes earn their keep.
 *   3. churn   a live set with random insert/remove.  The realistic one, and
 *              the one that punishes fragmentation -- the other two never make
 *              an allocator work.
 *   4. calloc  the same, zeroed.  Worth separating: an allocator that gets
 *              fresh pages from the OS can skip the memset, one that recycles
 *              cannot.
 *   5. realloc growth from small to large, which is where a bad realloc shows
 *              up as a copy it did not need to make.
 *
 * Sizes go from 16 bytes to 1 MiB, because the answer changes completely along
 * the way: small allocations are dominated by the free-list path and large ones
 * by whatever the allocator does with the operating system.
 */

#include "util/host_allocator.h"
#include "util/host_allocator_c.h"
#include "util/os_memory.h"
#include "util/host_allocator_layout.h"
#include "util/vesta_memcpy.h"
#include "util/vesta_memset.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double ns_since(Clock::time_point t0, long long ops) {
    const auto dt = Clock::now() - t0;
    return double(std::chrono::duration_cast<std::chrono::nanoseconds>(dt)
                      .count()) /
           double(ops);
}

/// A tiny deterministic generator.  `rand()` takes a lock in some libcs, which
/// would end up in the measurement.
struct Rng {
    uint64_t s = 0x9E3779B97F4A7C15ull;
    uint32_t next() noexcept {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return uint32_t(s >> 32);
    }
};

// ---------------------------------------------------------------------------
//  The two allocators, behind the same shape so the loops are identical.
// ---------------------------------------------------------------------------

struct Ours {
    static void *alloc(size_t n) noexcept { return util::host_alloc(n); }
    static void *zeroed(size_t n) noexcept { return vesta_host_calloc(1, n); }
    static void *grow(void *p, size_t n) noexcept {
        return vesta_host_realloc(p, n);
    }
    static void release(void *p) noexcept { util::host_free(p); }
};

/// El de un solo dueno: la misma maquinaria sin pasar por la ranura del hilo.
/// Es un objeto, no un modo global, asi que convive con los otros dos.
util::SingleOwnerAllocator g_owned;
struct Owned {
    static void *alloc(size_t n) noexcept { return g_owned.alloc(n); }
    static void *zeroed(size_t n) noexcept {
        void *p = g_owned.alloc(n);
        if (p != nullptr) vesta_memset(p, 0, n);
        return p;
    }
    static void *grow(void *p, size_t n) noexcept {
        return util::host_realloc(p, n);
    }
    static void release(void *p) noexcept { g_owned.free(p); }
};

struct System {
    static void *alloc(size_t n) noexcept { return std::malloc(n); }
    static void *zeroed(size_t n) noexcept { return std::calloc(1, n); }
    static void *grow(void *p, size_t n) noexcept {
        return std::realloc(p, n);
    }
    static void release(void *p) noexcept { std::free(p); }
};

// ---------------------------------------------------------------------------
//  The patterns.  Each returns nanoseconds per operation.
// ---------------------------------------------------------------------------

template <class A> double hot(size_t size, int rounds) {
    const auto t0 = Clock::now();
    for (int i = 0; i < rounds; ++i) {
        void *p = A::alloc(size);
        // Touch the first byte: an allocator that hands back an address it
        // never committed would otherwise not be charged for it.
        if (p != nullptr) *static_cast<volatile char *>(p) = 1;
        A::release(p);
    }
    return ns_since(t0, 2LL * rounds);
}

template <class A> double burst(size_t size, int batch, int rounds) {
    std::vector<void *> live(static_cast<size_t>(batch));
    const auto t0 = Clock::now();
    for (int r = 0; r < rounds; ++r) {
        for (int i = 0; i < batch; ++i) {
            live[size_t(i)] = A::alloc(size);
            if (live[size_t(i)] != nullptr)
                *static_cast<volatile char *>(live[size_t(i)]) = 1;
        }
        for (int i = 0; i < batch; ++i)
            A::release(live[size_t(i)]);
    }
    return ns_since(t0, 2LL * batch * rounds);
}

/// A live set of @p live blocks; each step frees a random one and allocates a
/// replacement.  This is the one that makes an allocator work.
template <class A> double churn(size_t size, int live_n, int steps) {
    std::vector<void *> live(static_cast<size_t>(live_n));
    for (int i = 0; i < live_n; ++i)
        live[size_t(i)] = A::alloc(size);

    Rng rng;
    const auto t0 = Clock::now();
    for (int i = 0; i < steps; ++i) {
        const size_t slot = rng.next() % uint32_t(live_n);
        A::release(live[slot]);
        live[slot] = A::alloc(size);
        if (live[slot] != nullptr)
            *static_cast<volatile char *>(live[slot]) = 1;
    }
    const double r = ns_since(t0, 2LL * steps);

    for (int i = 0; i < live_n; ++i)
        A::release(live[size_t(i)]);
    return r;
}

template <class A> double zeroed(size_t size, int rounds) {
    const auto t0 = Clock::now();
    for (int i = 0; i < rounds; ++i) {
        void *p = A::zeroed(size);
        if (p != nullptr) *static_cast<volatile char *>(p) = 1;
        A::release(p);
    }
    return ns_since(t0, 2LL * rounds);
}

/// Grow 64 bytes up to @p size, doubling.  Counts one op per step.
template <class A> double grow(size_t size, int rounds) {
    long long ops = 0;
    const auto t0 = Clock::now();
    for (int r = 0; r < rounds; ++r) {
        void *p = A::alloc(64);
        for (size_t n = 128; n <= size; n *= 2) {
            void *q = A::grow(p, n);
            if (q == nullptr) break;
            p = q;
            ++ops;
        }
        A::release(p);
    }
    return ops == 0 ? 0.0 : ns_since(t0, ops);
}

// ---------------------------------------------------------------------------
//  Measuring, A-B-B-A.
// ---------------------------------------------------------------------------

struct Pair {
    double ours = 0.0;
    double owned = 0.0;
    double sys = 0.0;
};

/* Los casos son structs de ambito de fichero y no lambdas locales porque hacen
 * falta con un `operator()` de PLANTILLA -- el mismo bucle se instancia para
 * los dos asignadores --, y C++17 no permite plantillas miembro dentro de una
 * clase local.  Una lambda generica tampoco vale: el parametro es el TIPO del
 * asignador, no un valor. */
struct HotCase {
    size_t size;
    int rounds;
    template <class A> double operator()() const {
        return hot<A>(size, rounds);
    }
};
struct BurstCase {
    size_t size;
    int rounds;
    template <class A> double operator()() const {
        return burst<A>(size, 512, rounds);
    }
};
struct ChurnCase {
    size_t size;
    int rounds;
    template <class A> double operator()() const {
        return churn<A>(size, 1024, rounds);
    }
};
struct ZeroedCase {
    size_t size;
    int rounds;
    template <class A> double operator()() const {
        return zeroed<A>(size, rounds);
    }
};
struct GrowCase {
    size_t size;
    int rounds;
    template <class A> double operator()() const {
        return grow<A>(size, rounds);
    }
};

/// @param f  Called as `f.template operator()<Alloc>()`, returns ns/op.
template <class F> Pair abba(F run) {
    const double a1 = run.template operator()<Ours>();
    const double c1 = run.template operator()<Owned>();
    const double b1 = run.template operator()<System>();
    const double b2 = run.template operator()<System>();
    const double c2 = run.template operator()<Owned>();
    const double a2 = run.template operator()<Ours>();
    Pair p;
    p.ours = (a1 + a2) / 2.0;
    p.owned = (c1 + c2) / 2.0;
    p.sys = (b1 + b2) / 2.0;
    return p;
}

void row(const char *label, size_t size, Pair p) {
    char name[48];
    if (size >= (1u << 20))
        std::snprintf(name, sizeof(name), "%s %zuM", label, size >> 20);
    else if (size >= 1024)
        std::snprintf(name, sizeof(name), "%s %zuK", label, size >> 10);
    else
        std::snprintf(name, sizeof(name), "%s %zu", label, size);

    /* La proporcion se calcula contra la MEJOR de las dos nuestras, porque la
     * pregunta es "que se puede conseguir con esta biblioteca", no "cual de sus
     * dos formas escogimos".  Las dos columnas estan a la vista para poder ver
     * cuanto cuesta compartir. */
    const double best = (p.owned > 0.0 && p.owned < p.ours) ? p.owned : p.ours;
    const double ratio = best > 0.0 ? p.sys / best : 0.0;
    std::printf("  %-16s %9.2f %9.2f %9.2f  %6.2fx  %s\n", name, p.ours,
                p.owned, p.sys, ratio, ratio >= 1.0 ? "" : "<- system wins");
}

void header(const char *title) {
    std::printf("\n%s\n", title);
    std::printf("  %-16s %9s %9s %9s  %8s\n", "case", "shared", "owned",
                "malloc", "best/sys");
    std::printf("  ---------------- --------- --------- ---------  --------\n");
}

const size_t kSizes[] = {16, 64, 256, 1024, 4096, 65536, 1u << 20};
const int kSizeCount = int(sizeof(kSizes) / sizeof(kSizes[0]));

// ---------------------------------------------------------------------------
//  Lo que el reloj no ve: lo que cuesta un bloque en MEMORIA
// ---------------------------------------------------------------------------
//
// Las tablas de arriba miden tiempo, y hay cambios que no lo tocan y sin
// embargo empeoran el asignador: mover una clase de tamano, cambiar la cabecera
// del trozo o el relleno de las reservas alineadas cambia cuanta memoria se
// queda quieta sin que ninguna cifra de nanosegundos se mueva.
//
// Estas dos tablas son ARITMETICA, no medida: salen iguales en cualquier
// maquina y en cualquier corrida.  Eso las hace mejores detectoras de
// regresion que un tiempo -- una diferencia aqui es un cambio, nunca ruido --.

/// La clase que serviria @p n bytes, buscada recorriendo la lista.  La tabla
/// rapida se llena al arrancar el asignador; aqui hace falta la aritmetica pura.
size_t class_for(size_t n) {
    for (uint32_t i = 0; i < util::kClasses; ++i)
        if (util::kSizes[i] >= n) return util::kSizes[i];
    return 0; // por encima de las clases: lo sirve un tramo
}

/// Cuanto se queda sin usar al final de un trozo, por clase.  Un trozo no se
/// devuelve hasta vaciarse entero, asi que lo que sobra al final esta retenido
/// mientras viva UN solo bloque de esa clase.
void memory_per_class() {
    std::printf("\nwhat a chunk wastes, per size class\n");
    std::printf("  %8s %7s %8s %10s %8s\n", "class", "chunk", "blocks",
                "left over", "of chunk");
    std::printf("  -------- ------- -------- ---------- --------\n");
    for (uint32_t i = 0; i < util::kClasses; ++i) {
        const size_t s = util::kSizes[i];
        /* Cada clase se sirve del trozo que le toca, y hay que preguntarlo aqui
         * en vez de dar por hecho el pequeno: las clases grandes viven en su
         * propia region con trozos de otro tamano, que es justamente el cambio
         * que esta tabla tiene que poder detectar. */
        const size_t chunk =
            s >= util::kBigClassMin ? util::kBigChunkBytes : util::kChunkBytes;
        const size_t usable = chunk - sizeof(util::ChunkHeader);
        const size_t blocks = usable / s;
        const size_t left = usable - blocks * s;
        const double pct = 100.0 * double(left) / double(chunk);
        // Solo lo que se nota: por debajo del 1% la lista entera seria ruido.
        if (pct >= 1.0)
            std::printf("  %8zu %6zuK %8zu %10zu %7.1f%%\n", s, chunk >> 10,
                        blocks, left, pct);
    }
}

/// Lo que cuesta de mas un tipo SOBRE-ALINEADO frente a uno normal del mismo
/// tamano.  Es el precio del relleno, y se compara contra la clase ideal para
/// que un cambio en la formula salte aqui aunque no mueva ningun tiempo.
void memory_over_aligned() {
    struct Caso {
        size_t n, align;
    };
    static const Caso kCasos[] = {{32, 32},     {64, 64},   {128, 64},
                                  {1000, 64},   {4096, 64}, {1024, 256},
                                  {4096, 4096}};

    std::printf("\nwhat over-alignment costs (padding, not time)\n");
    std::printf("  %8s %7s %9s %9s %8s\n", "size", "align", "ideal", "actual",
                "x ideal");
    std::printf("  -------- ------- --------- --------- --------\n");
    for (const Caso &c : kCasos) {
        void *p = util::host_alloc_aligned(c.n, c.align);
        if (p == nullptr) continue;
        /* La clase real se lee de la CABECERA del trozo del bloque original,
         * no se recalcula: repetir aqui la formula del relleno mediria la
         * formula y no el codigo. */
        void *raw = nullptr;
        vesta_memcpy(&raw, reinterpret_cast<const char *>(p) - sizeof(void *),
                     sizeof raw);
        const util::ChunkHeader *h = util::chunk_of(raw);
        const size_t actual = (h->magic == util::kChunkMagic)
                                  ? util::kSizes[h->cls]
                                  : 0; // lo sirvio un tramo
        const size_t ideal = class_for(c.n);
        std::printf("  %8zu %7zu %9zu %9zu %7.2fx\n", c.n, c.align, ideal,
                    actual, (ideal && actual) ? double(actual) / double(ideal)
                                              : 0.0);
        util::host_free_aligned(p);
    }
}

/// Fewer iterations as the blocks get bigger, or the large cases dominate the
/// wall time without adding information.
int rounds_for(size_t size) {
    if (size <= 1024) return 300000;
    if (size <= 65536) return 40000;
    return 4000;
}

} // namespace

int main() {
    std::printf("== head to head: this allocator vs malloc ==\n\n");
    if (!util::host_alloc_active()) {
        std::printf("The allocator is OFF (VESTA_NO_HOST_SLAB), so both sides\n"
                    "would be malloc.  Unset it and run again.\n");
        return 1;
    }
    std::printf("Both are called directly, in this process, interleaved A-B-B-A\n"
                "so that drift in the machine does not land on the difference.\n");

    // Warm both up: the first allocation registers a thread and asks the OS
    // for memory.  Timing that would measure startup, not steady state.
    for (int i = 0; i < 20000; ++i) {
        util::host_free(util::host_alloc(64));
        std::free(std::malloc(64));
    }

    header("allocate and free immediately (hot)");
    for (int i = 0; i < kSizeCount; ++i) {
        const size_t s = kSizes[i];
        const int n = rounds_for(s);
        row("hot", s, abba(HotCase{s, n}));
    }

    header("allocate a batch, free the batch (burst)");
    for (int i = 0; i < kSizeCount; ++i) {
        const size_t s = kSizes[i];
        const int n = rounds_for(s) / 512 + 1;
        row("burst", s, abba(BurstCase{s, n}));
    }

    header("live set with random replacement (churn)");
    for (int i = 0; i < kSizeCount; ++i) {
        const size_t s = kSizes[i];
        const int n = rounds_for(s);
        row("churn", s, abba(ChurnCase{s, n}));
    }

    header("zeroed (calloc)");
    for (int i = 0; i < kSizeCount; ++i) {
        const size_t s = kSizes[i];
        const int n = rounds_for(s);
        row("calloc", s, abba(ZeroedCase{s, n}));
    }

    header("grow from 64 bytes by doubling (realloc)");
    for (int i = 2; i < kSizeCount; ++i) {
        const size_t s = kSizes[i];
        const int n = rounds_for(s) / 8 + 1;
        row("grow to", s, abba(GrowCase{s, n}));
    }

    memory_per_class();
    memory_over_aligned();

    const util::OsProcessMemory pm = util::os_process_memory();
    std::printf("\nprocess peak %.1f MiB resident\n",
                pm.working_set_peak / (1024.0 * 1024.0));
    std::printf("\nA ratio above 1.00 means this allocator is that many times\n"
                "faster.  Anything marked \"system wins\" is a real result, not\n"
                "noise to be explained away.\n");
    return 0;
}
