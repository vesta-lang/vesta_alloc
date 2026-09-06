/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file bench/bench_memset.cpp
 * @brief `util::vesta_memset` head to head against the C library's `memset`.
 *
 * WHY THIS EXISTS.  Same reason as `bench_memcpy`: replacing a C library
 * routine is a claim, and a claim needs a number -- including the sizes where
 * we lose, which are printed marked rather than left out.
 *
 * WHY IT IS A SEPARATE BENCHMARK from the copy, and not one table with both:
 * they do not share a bottleneck.  A copy reads and writes, so it is limited by
 * the load unit and by two streams competing for cache.  A fill only WRITES,
 * which on a modern core is a different limit entirely -- and at large sizes it
 * runs into write-allocate: the CPU reads a line it is about to overwrite whole.
 * Mixing the two rows in one table would invite comparing numbers that do not
 * answer the same question.
 *
 * WHAT THE ANSWER DEPENDS ON:
 *
 *   1. **Size.**  Under 16 bytes the whole cost is the call, and filling inline
 *      wins by a lot.  In the middle the vector loop decides.  Past the last
 *      level of cache it is write bandwidth and both sides tie.
 *   2. **Alignment**, because our stores are unaligned on purpose.
 *   3. **Zero versus any other byte.**  Kept apart on purpose: zeroing is the
 *      overwhelmingly common case -- it is what `calloc` does -- and it is the
 *      one a C library is most likely to have a special path for.
 *
 * Interleaved A-B-B-A, and the buffer is read after every call so the compiler
 * cannot delete the fill it is being asked to measure.
 */

#include "util/vesta_memset.h"

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

/// Hace opaco un puntero para el optimizador, sin emitir ni una instruccion.
/// Sin esto, el compilador VE que el relleno no se usa y lo borra.
template <class T> [[gnu::always_inline]] inline void escape(T &p) noexcept {
    asm volatile("" : "+r"(p) : : "memory");
}

// ---------------------------------------------------------------------------
//  Las dos rutinas, tras la misma forma para que los bucles sean identicos.
// ---------------------------------------------------------------------------

struct Ours {
    static const char *name() noexcept { return "vesta_memset"; }
    [[gnu::always_inline]] static void run(void *d, uint8_t v,
                                           size_t n) noexcept {
        util::vesta_memset(d, v, n);
    }
};

struct Libc {
    static const char *name() noexcept { return "memset"; }
    [[gnu::always_inline]] static void run(void *d, uint8_t v,
                                           size_t n) noexcept {
        std::memset(d, v, n);
    }
};

// ---------------------------------------------------------------------------
//  Patrones.  Cada uno devuelve nanosegundos por relleno.
// ---------------------------------------------------------------------------

/**
 * @brief Rellena el MISMO bloque una y otra vez.
 *
 * Con el bufer residente en cache se mide la rutina y nada mas.  Para tamanos
 * grandes deja de caber y pasa a medirse el ancho de banda de ESCRITURA, que es
 * lo que hay que ver ahi.
 *
 * @param n      Bytes por relleno.
 * @param v      Byte a escribir.
 * @param rounds Cuantos rellenos.
 * @param off    Desalineamiento del destino.
 */
template <class R> double hot(size_t n, uint8_t v, int rounds, size_t off) {
    std::vector<uint8_t> buf(n + 64, 0);
    uint8_t *d = buf.data() + off;
    uint64_t sink = 0;

    R::run(d, v, n); // una fuera de la medida: calienta y decide el despacho

    const auto t0 = Clock::now();
    for (int i = 0; i < rounds; ++i) {
        escape(d);
        R::run(d, v, n);
        sink += d[0]; // leer el destino: el relleno NO es codigo muerto
    }
    const auto dt = Clock::now() - t0;

    escape(sink);
    return double(std::chrono::duration_cast<std::chrono::nanoseconds>(dt)
                      .count()) /
           double(rounds);
}

/**
 * @brief Rellenos repartidos por una region de 16 MiB.
 *
 * El caso realista para un asignador: el bloque que hay que zerificar NO esta
 * en cache, porque acaba de salir de una lista de libres donde llevaba un rato.
 * Es exactamente lo que hace `host_alloc_zeroed` al reciclar.
 */
template <class R> double scattered(size_t n, uint8_t v, int rounds) {
    constexpr size_t kSpan = 16u << 20;
    std::vector<uint8_t> area(kSpan + 4096, 0);

    uint64_t seed = 0x9E3779B97F4A7C15ull;
    uint64_t sink = 0;
    const size_t reach = kSpan - n - 64;

    const auto t0 = Clock::now();
    for (int i = 0; i < rounds; ++i) {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        uint8_t *d = area.data() + (size_t(seed >> 33) % reach);
        escape(d);
        R::run(d, v, n);
        sink += d[0];
    }
    const auto dt = Clock::now() - t0;

    escape(sink);
    return double(std::chrono::duration_cast<std::chrono::nanoseconds>(dt)
                      .count()) /
           double(rounds);
}

// ---------------------------------------------------------------------------
//  Medida y presentacion
// ---------------------------------------------------------------------------

struct Pair {
    double ours, libc;
};

struct HotCase {
    size_t n;
    uint8_t v;
    int rounds;
    size_t off;
    template <class R> double operator()() const {
        return hot<R>(n, v, rounds, off);
    }
};

struct ScatteredCase {
    size_t n;
    uint8_t v;
    int rounds;
    template <class R> double operator()() const {
        return scattered<R>(n, v, rounds);
    }
};

template <class F> Pair abba(F run) {
    const double a1 = run.template operator()<Ours>();
    const double b1 = run.template operator()<Libc>();
    const double b2 = run.template operator()<Libc>();
    const double a2 = run.template operator()<Ours>();
    return {(a1 + a2) / 2.0, (b1 + b2) / 2.0};
}

void row(const char *label, size_t n, Pair p) {
    char name[48];
    if (n >= (1u << 20))
        std::snprintf(name, sizeof(name), "%s %zuM", label, n >> 20);
    else if (n >= 1024)
        std::snprintf(name, sizeof(name), "%s %zuK", label, n >> 10);
    else
        std::snprintf(name, sizeof(name), "%s %zu", label, n);

    const double ratio = p.ours > 0.0 ? p.libc / p.ours : 0.0;
    // GB/s de lo nuestro: en los tamanos grandes dice si se esta llegando al
    // techo de escritura de la memoria.
    const double gbs = p.ours > 0.0 ? double(n) / p.ours : 0.0;
    std::printf("  %-18s %10.2f %10.2f %8.2f  %7.2fx  %s\n", name, p.ours,
                p.libc, gbs, ratio, ratio >= 1.0 ? "" : "<- libc wins");
}

void header(const char *title) {
    std::printf("\n%s\n", title);
    std::printf("  %-18s %10s %10s %8s  %8s\n", "case", "ours", "libc", "GB/s",
                "libc/ours");
    std::printf(
        "  ------------------ ---------- ---------- --------  --------\n");
}

const size_t kSizes[] = {8,   16,   32,   64,    128,
                         256, 1024, 4096, 65536, 1u << 20};
const int kSizeCount = int(sizeof(kSizes) / sizeof(kSizes[0]));

int rounds_for(size_t n) {
    if (n <= 256) return 2000000;
    if (n <= 4096) return 400000;
    if (n <= 65536) return 40000;
    return 3000;
}

} // namespace

int main() {
    std::printf("== vesta_memset vs the C library ==\n\n");
    std::printf("Both are called in this process, interleaved A-B-B-A, and the\n"
                "numbers are nanoseconds per fill.  A ratio above 1.00 means\n"
                "ours is that many times faster.\n");

    header("zeroing, aligned, cache resident (what calloc does)");
    for (int i = 0; i < kSizeCount; ++i) {
        const size_t n = kSizes[i];
        row("zero", n, abba(HotCase{n, 0, rounds_for(n), 0}));
    }

    header("filling with a non-zero byte (no special path can apply)");
    for (int i = 0; i < kSizeCount; ++i) {
        const size_t n = kSizes[i];
        row("fill", n, abba(HotCase{n, 0x5C, rounds_for(n), 0}));
    }

    header("misaligned by 3 (our stores are unaligned on purpose)");
    for (int i = 0; i < kSizeCount; ++i) {
        const size_t n = kSizes[i];
        row("odd", n, abba(HotCase{n, 0, rounds_for(n), 3}));
    }

    header("scattered over 16 MiB (not in cache: recycled blocks)");
    for (int i = 0; i < kSizeCount - 2; ++i) {
        const size_t n = kSizes[i];
        row("cold", n, abba(ScatteredCase{n, 0, rounds_for(n) / 8}));
    }

    std::printf("\nWhere ours wins is the small sizes, and the reason is that\n"
                "under 16 bytes it does not call anybody.  Past the last level\n"
                "of cache the two tie by construction: that is write bandwidth,\n"
                "and no routine can be faster than the memory.\n");
    return 0;
}
