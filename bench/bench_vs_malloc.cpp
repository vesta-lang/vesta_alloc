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
 * WHY A SECOND BENCHMARK.  It used to be that `bench_allocator` compared by
 * re-running the same binary with the allocator switched off, and the two
 * numbers came from different runs -- so anything that drifted between them
 * (CPU frequency, page cache, another process waking up) landed on the
 * difference.  That switch is gone and `bench_allocator` now interleaves both
 * columns in one process as well, through `support/system_alloc.h`.
 *
 * What still separates the two: there the patterns are whole-program shapes
 * (bursts, threads, cross-thread), here each call is measured on its own,
 * including `realloc`, which the other one does not exercise at all.
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
 *   4. calloc  the same, zeroed, and measured TWICE: once with the caller
 *              reading the whole block and once reading a sixty-fourth.  It has
 *              to be both, because a large `calloc` does not zero anything --
 *              it maps pages the kernel already holds zeroed and defers the
 *              cost to page faults.  Measured with a single byte touched, that
 *              deferral looks like speed; measured with the block actually
 *              used, it is 8x slower.  Neither row alone is the answer.
 *   5. realloc growth from small to large, which is where a bad realloc shows
 *              up as a copy it did not need to make.
 *
 * Sizes go from 16 bytes to 1 MiB, because the answer changes completely along
 * the way: small allocations are dominated by the free-list path and large ones
 * by whatever the allocator does with the operating system.
 */

#include "util/alloc/host_allocator.h"
#include "util/alloc/host_allocator_c.h"
#include "util/os/os_memory.h"
#include "util/alloc/host_allocator_layout.h"
#include "util/mem/vesta_memcpy.h"
#include "util/mem/vesta_memset.h"

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
//  Every way in, behind the same shape so the loops are identical.
//
//  A column is a TYPE, which is also how a caller picks a specialisation in
//  real code -- resolved at compile time, never a flag.  Each one carries its
//  own name and says whether it is one of ours, so the driver below can walk
//  them as a list instead of being hand-unrolled once per column.
// ---------------------------------------------------------------------------

struct Ours {
    static void *alloc(size_t n) noexcept { return util::host_alloc(n); }
    static void *zeroed(size_t n) noexcept { return vesta_host_calloc(1, n); }
    static void *grow(void *p, size_t n) noexcept {
        return vesta_host_realloc(p, n);
    }
    static void release(void *p) noexcept { util::host_free(p); }
    static const char *name() noexcept { return "shared"; }
    static constexpr bool is_ours = true;
};

/// A cache per thread and no lock.  Here it measures the SAME single-threaded
/// work as the others on purpose: this is where it must not be paying for the
/// thing it buys.  What it buys only shows up in `bench_contention`, with more
/// threads alive than the shared policy can name.
struct PerThread {
    static void *alloc(size_t n) noexcept {
        return util::PerThreadAllocator::alloc(n);
    }
    static void *zeroed(size_t n) noexcept {
        void *p = util::PerThreadAllocator::alloc(n);
        if (p != nullptr) vesta_memset(p, 0, n);
        return p;
    }
    static void *grow(void *p, size_t n) noexcept {
        return util::host_realloc(p, n);
    }
    static void release(void *p) noexcept { util::PerThreadAllocator::free(p); }
    static const char *name() noexcept { return "per-thread"; }
    static constexpr bool is_ours = true;
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
    static const char *name() noexcept { return "owned"; }
    static constexpr bool is_ours = true;
};

struct System {
    static void *alloc(size_t n) noexcept { return std::malloc(n); }
    static void *zeroed(size_t n) noexcept { return std::calloc(1, n); }
    static void *grow(void *p, size_t n) noexcept {
        return std::realloc(p, n);
    }
    static void release(void *p) noexcept { std::free(p); }
    static const char *name() noexcept { return "malloc"; }
    static constexpr bool is_ours = false;
};

/**
 * @brief The columns, as a type list.
 *
 * Adding a way in is a TYPE here.  It used to be a field in the result struct,
 * a pair of lines in the A-B-B-A driver and a column in two printf formats,
 * which is three places to forget and no error when you do -- the column simply
 * would not appear.
 *
 * Order matters: the driver walks this forwards and then backwards, so the
 * system allocator goes last and lands first on the way back.
 */
template <class...> struct Columns {};
using AllColumns = Columns<Ours, PerThread, Owned, System>;
constexpr int kCols = 4;

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

/**
 * @brief Stops the compiler from deleting the work being measured.
 *
 * NEEDED, and this benchmark got it wrong without it.  With `std::free` right
 * after the block, GCC knows it dies and DELETES the memset; with our `free`,
 * an opaque call, it cannot.  So the two columns were not measuring the same
 * thing: 4 MiB "filled" in 9 us is 447 GB/s, which no single core can do.
 */
[[gnu::always_inline]] inline void keep(void *p) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    __asm__ __volatile__("" : : "r"(p) : "memory");
#else
    (void)*static_cast<volatile char *>(p);
#endif
}

/**
 * @brief Zeroed allocation, measured by how much of it the caller USES.
 *
 * WHY THIS IS TWO CASES AND NOT ONE.  A large `calloc` does not zero anything:
 * it maps pages the kernel already holds zeroed and defers the cost to page
 * faults.  If nobody touches the memory, that cost is never paid.  We zero for
 * real, up front.  Measuring with a single byte touched therefore does not
 * compare two ways of doing the same job -- it rewards whoever does less work,
 * without asking whether the work was needed.
 *
 * Measured here, 1 MiB on Windows: zeroing eagerly and then reading the whole
 * block costs 28,7 us; deferring costs 4,7 us plus 292 us of page faults.
 * Deferring only wins when the caller touches under ~4% of what it asked for.
 *
 * @param used  Fraction of the block the caller reads, 1..N (1 = all of it).
 */
template <class A> double zeroed(size_t size, int rounds, size_t used) {
    const auto t0 = Clock::now();
    for (int i = 0; i < rounds; ++i) {
        void *p = A::zeroed(size);
        if (p != nullptr) {
            std::memset(p, 1, size / used);
            keep(p);
        }
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

struct Row {
    double ns[kCols] = {};
    /* And what each one KEEPS.  Time alone does not settle any of these rows:
     * an allocator that defers the work also defers the memory, and one that
     * holds on to a block is faster next time for a reason that has a price.
     * These are the committed bytes this process gained across the run. */
    double mem[kCols] = {};
};

/**
 * @brief How much memory each column had to COMMIT for its run.
 *
 * The two allocators live in the same process, so a process-level number
 * cannot tell them apart.  Each is therefore read with the instrument that
 * attributes it:
 *
 *   - ours (both columns) by our own counter of bytes committed out of the
 *     region, which is exact and counts only us;
 *   - malloc by what the process gained in committed private bytes while only
 *     its column was running.
 *
 * Reading ours with the process counter instead would print zeros and mean
 * nothing: by the time a case runs, the region already holds what the earlier
 * cases committed.
 */
uint64_t committed_by_us() noexcept {
    return util::host_alloc_stats().bytes_reserved;
}
uint64_t committed_by_process() noexcept {
    return util::os_process_memory().commit;
}

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
    size_t used; ///< 1 = the caller reads all of it; 64 = a sixty-fourth
    template <class A> double operator()() const {
        return zeroed<A>(size, rounds, used);
    }
};
struct GrowCase {
    size_t size;
    int rounds;
    template <class A> double operator()() const {
        return grow<A>(size, rounds);
    }
};

/// One column's run, behind a pointer so the driver can hold them in an array.
/// The call happens once per RUN -- thousands of operations later -- so the
/// inner loop is still `A`'s code, fully typed and fully inlined.
template <class F, class A> double call_one(const F &run) {
    return run.template operator()<A>();
}

/// Whether a column is served by this library, which decides WHICH instrument
/// attributes its memory.
template <class A> bool column_is_ours() { return A::is_ours; }
template <class A> const char *column_name() { return A::name(); }

/// @param run  Called as `run.template operator()<Alloc>()`, returns ns/op.
template <class F, class... A> Row abba(const F &run, Columns<A...>) {
    using Fn = double (*)(const F &);
    static const Fn fns[] = {&call_one<F, A>...};
    static const bool ours[] = {A::is_ours...};
    constexpr int n = int(sizeof...(A));

    Row r;
    /* FORWARDS AND THEN BACKWARDS, which is A-B-B-A generalised: each column
     * lands as often near the start as near the end, so drift in the machine
     * -- thermal, another process, the scheduler settling -- does not land on
     * the difference between them. */
    for (int i = 0; i < n; ++i) {
        /* The memory each one keeps is read around its FIRST run only: by the
         * second the heap has already grown and the difference would read as
         * zero for everybody.  Ours is read with our own committed-bytes
         * counter, which is exact and counts only us; the system one with what
         * the process gained, which is the only instrument that sees it. */
        const uint64_t before =
            ours[i] ? committed_by_us() : committed_by_process();
        r.ns[i] = fns[i](run);
        const uint64_t after =
            ours[i] ? committed_by_us() : committed_by_process();
        /* Signed on purpose: a negative reading means that column gave memory
         * back to the system, which is a real answer and not an error to
         * hide. */
        r.mem[i] = double(int64_t(after) - int64_t(before));
    }
    for (int i = n; i-- > 0;) r.ns[i] = (r.ns[i] + fns[i](run)) / 2.0;
    return r;
}

void row(const char *label, size_t size, const Row &r) {
    char name[48];
    if (size >= (1u << 20))
        std::snprintf(name, sizeof(name), "%s %zuM", label, size >> 20);
    else if (size >= 1024)
        std::snprintf(name, sizeof(name), "%s %zuK", label, size >> 10);
    else
        std::snprintf(name, sizeof(name), "%s %zu", label, size);

    std::printf("  %-16s", name);
    for (int i = 0; i < kCols; ++i) std::printf(" %10.2f", r.ns[i]);

    /* The ratio is against the BEST of ours, because the question is "what can
     * be had from this library", not "which of its forms did we pick".  Every
     * column stays on show so the cost of sharing is visible. */
    double best = 0.0;
    for (int i = 0; i < kCols - 1; ++i)
        if (r.ns[i] > 0.0 && (best == 0.0 || r.ns[i] < best)) best = r.ns[i];
    const double sys = r.ns[kCols - 1];
    const double ratio = best > 0.0 ? sys / best : 0.0;
    std::printf("  %6.2fx  ", ratio);

    const double mib = 1024.0 * 1024.0;
    for (int i = 0; i < kCols; ++i) std::printf(" %7.2f", r.mem[i] / mib);
    std::printf("  %s\n", ratio >= 1.0 ? "" : "<- system wins");
}

void header(const char *title) {
    static const char *names[] = {Ours::name(), PerThread::name(),
                                  Owned::name(), System::name()};
    std::printf("\n%s\n", title);
    std::printf("  %-16s", "case");
    for (int i = 0; i < kCols; ++i) std::printf(" %10s", names[i]);
    std::printf("  %8s   %s\n", "best/sys", "MiB committed by the run");
    std::printf("  %-16s", "");
    for (int i = 0; i < kCols; ++i) std::printf(" %10s", "ns/op");
    std::printf("  %8s  ", "");
    for (int i = 0; i < kCols; ++i) std::printf(" %7s", names[i]);
    std::printf("\n  ----------------");
    for (int i = 0; i < kCols; ++i) std::printf(" ----------");
    std::printf("  --------  ");
    for (int i = 0; i < kCols; ++i) std::printf(" -------");
    std::printf("\n");
}

const size_t kSizes[] = {16, 64, 256, 1024, 4096, 65536, 1u << 20};
const int kSizeCount = int(sizeof(kSizes) / sizeof(kSizes[0]));

/**
 * @brief Sizes only the zeroed cases use, and why they are only there.
 *
 * Above a few mebibytes the interesting question stops being the free list and
 * becomes WHO puts the zeros there -- and the answer flips: past a threshold it
 * is cheaper to hand the pages back to the system and let it zero what the
 * caller actually touches.  Without a size up here, that path would not be
 * exercised by anything, and a path nothing measures is a path nobody knows is
 * working.
 *
 * They are not in `kSizes` because the other cases keep a live set of hundreds
 * of blocks: at eight mebibytes each that is gigabytes of working set, which
 * would measure the machine's memory and not the allocator.
 */
const size_t kBigZeroedSizes[] = {size_t(8) << 20};
const int kBigZeroedCount =
    int(sizeof(kBigZeroedSizes) / sizeof(kBigZeroedSizes[0]));

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
        std::printf("This allocator is NOT in force, so both sides would be\n"
                    "the same malloc.  Check that the static archive really "
                    "linked in.\n");
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
        row("hot", s, abba(HotCase{s, n}, AllColumns{}));
    }

    header("allocate a batch, free the batch (burst)");
    for (int i = 0; i < kSizeCount; ++i) {
        const size_t s = kSizes[i];
        const int n = rounds_for(s) / 512 + 1;
        row("burst", s, abba(BurstCase{s, n}, AllColumns{}));
    }

    header("live set with random replacement (churn)");
    for (int i = 0; i < kSizeCount; ++i) {
        const size_t s = kSizes[i];
        const int n = rounds_for(s);
        row("churn", s, abba(ChurnCase{s, n}, AllColumns{}));
    }

    header("zeroed (calloc), and the caller reads ALL of it");
    for (int i = 0; i < kSizeCount; ++i) {
        const size_t s = kSizes[i];
        const int n = rounds_for(s);
        row("calloc", s, abba(ZeroedCase{s, n, 1}, AllColumns{}));
    }
    for (int i = 0; i < kBigZeroedCount; ++i) {
        const size_t s = kBigZeroedSizes[i];
        row("calloc", s, abba(ZeroedCase{s, rounds_for(s), 1}, AllColumns{}));
    }

    std::printf("\nThe other half of the answer, not a second opinion: whoever\n"
                "defers the zeroing wins below and loses above.  A row where a\n"
                "1 MiB request is barely read says more about the caller asking\n"
                "for the wrong size than about the allocator.\n");
    header("zeroed (calloc), and the caller reads a SIXTY-FOURTH");
    for (int i = 0; i < kSizeCount; ++i) {
        const size_t s = kSizes[i];
        const int n = rounds_for(s);
        row("calloc", s, abba(ZeroedCase{s, n, 64}, AllColumns{}));
    }
    for (int i = 0; i < kBigZeroedCount; ++i) {
        const size_t s = kBigZeroedSizes[i];
        row("calloc", s, abba(ZeroedCase{s, rounds_for(s), 64}, AllColumns{}));
    }

    header("grow from 64 bytes by doubling (realloc)");
    for (int i = 2; i < kSizeCount; ++i) {
        const size_t s = kSizes[i];
        const int n = rounds_for(s) / 8 + 1;
        row("grow to", s, abba(GrowCase{s, n}, AllColumns{}));
    }

    memory_per_class();
    memory_over_aligned();

    /* What the whole run cost in MEMORY, which the ns/op columns cannot show.
     * It belongs next to the times: an allocator that is faster because it
     * keeps everything it was ever given is not faster, it is trading. */
    const util::OsProcessMemory pm = util::os_process_memory();
    const util::HostAllocStats hs = util::host_alloc_stats();
    const double mib = 1024.0 * 1024.0;
    std::printf("\nmemory for the whole run\n");
    std::printf("  address space reserved by us     %8.1f MiB\n",
                double(util::host_region_reserved()) / mib);
    std::printf("  committed by us                  %8.1f MiB   (%llu chunks)\n",
                double(hs.bytes_reserved) / mib,
                (unsigned long long)hs.chunks);
    /* El pico de comprometido no lo da todo el mundo: en Linux vendria de
     * `/proc/self/status` y la capa de sistema no lo lee.  Se dice, en vez de
     * imprimir un cero que parece un dato. */
    if (pm.commit_peak != 0)
        std::printf("  committed by the whole process   %8.1f MiB   (peak %.1f)\n",
                    pm.commit / mib, pm.commit_peak / mib);
    else
        std::printf("  committed by the whole process   %8.1f MiB   (peak: this "
                    "system does not report it)\n",
                    pm.commit / mib);
    std::printf("  resident now / peak              %8.1f / %.1f MiB\n",
                pm.working_set / mib, pm.working_set_peak / mib);
    std::printf("\n  Reserved is address space and costs nothing until it is\n"
                "  committed; committed is what the system has to back.  We\n"
                "  reserve once and keep what we commit, which is why our\n"
                "  per-row figures fall to zero after the first case: the memory\n"
                "  is already ours.  That is the price of the speed above.\n");
    std::printf("\nA ratio above 1.00 means this allocator is that many times\n"
                "faster.  Anything marked \"system wins\" is a real result, not\n"
                "noise to be explained away.\n");
    return 0;
}
