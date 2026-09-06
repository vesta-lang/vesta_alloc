/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file bench/bench_memcpy.cpp
 * @brief `util::vesta_memcpy` head to head against the C library's `memcpy`.
 *
 * WHY THIS EXISTS.  Writing your own `memcpy` is easy to justify on paper and
 * easy to get wrong in practice: the C library's is the most tuned routine on
 * most systems, and beating it is NOT the default outcome.  So the number goes
 * here, in the open, including the sizes where we lose.
 *
 * WHAT THE ANSWER DEPENDS ON, and why the table has these rows:
 *
 *   1. **Size.**  Three different regimes, not one.  Under ~16 bytes the whole
 *      cost is the call itself, and that is where copying inline wins by a lot.
 *      In the middle the vector loop decides.  Above the last level of cache
 *      nobody wins: it is memory bandwidth, and both sides are the same.
 *   2. **Alignment.**  Our moves are unaligned on purpose; the row with an odd
 *      offset is there to prove that costs nothing on a modern core -- and to
 *      catch it if it ever starts to.
 *   3. **The platform.**  On Linux, glibc dispatches on CPU and is a serious
 *      opponent.  On Windows, the MinGW CRT copies byte by byte, which is the
 *      whole reason this code exists.
 *
 * INTERLEAVED, AND IN BOTH ORDERS.  Every measurement runs A-B-B-A and averages
 * each side.  Two runs in a row do not distinguish a change from drift: if the
 * machine speeds up or slows down mid-benchmark, a plain A-then-B blames the
 * routine for it.  A-B-B-A cancels a linear drift.
 *
 * The buffers are TOUCHED after every call and the pointers are laundered
 * through an empty asm, so the compiler cannot decide the copy is dead and
 * delete the thing being measured -- which it will, given the chance.
 */

#include "util/vesta_memcpy.h"

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

/// Hace opaco un puntero para el optimizador, sin emitir ni una instruccion.
/// Sin esto, el compilador VE que la copia no se usa y la borra: se estaria
/// midiendo un bucle vacio.
template <class T> [[gnu::always_inline]] inline void escape(T &p) noexcept {
    asm volatile("" : "+r"(p) : : "memory");
}

// ---------------------------------------------------------------------------
//  Las dos rutinas, tras la misma forma para que los bucles sean identicos.
// ---------------------------------------------------------------------------

struct Ours {
    static const char *name() noexcept { return "vesta_memcpy"; }
    [[gnu::always_inline]] static void run(void *d, const void *s,
                                           size_t n) noexcept {
        util::vesta_memcpy(d, s, n);
    }
};

struct Libc {
    static const char *name() noexcept { return "memcpy"; }
    [[gnu::always_inline]] static void run(void *d, const void *s,
                                           size_t n) noexcept {
        std::memcpy(d, s, n);
    }
};

// ---------------------------------------------------------------------------
//  Patrones.  Cada uno devuelve nanosegundos por copia.
// ---------------------------------------------------------------------------

/**
 * @brief Copia el MISMO bloque una y otra vez.
 *
 * Con los dos bufers residentes en cache, lo que se mide es la rutina y nada
 * mas.  Para tamanos grandes deja de caber y la medida pasa a ser de ancho de
 * banda, que es justo lo que hay que ver ahi.
 *
 * @param n       Bytes por copia.
 * @param rounds  Cuantas copias.
 * @param off_d   Desalineamiento del destino.
 * @param off_s   Desalineamiento del origen.
 */
template <class R>
double hot(size_t n, int rounds, size_t off_d, size_t off_s) {
    std::vector<uint8_t> dbuf(n + 64), sbuf(n + 64);
    for (size_t i = 0; i < sbuf.size(); ++i)
        sbuf[i] = uint8_t(i * 31u + 7u);

    uint8_t *d = dbuf.data() + off_d;
    const uint8_t *s = sbuf.data() + off_s;
    uint64_t sink = 0;

    R::run(d, s, n); // una fuera de la medida: calienta y decide el despacho

    const auto t0 = Clock::now();
    for (int i = 0; i < rounds; ++i) {
        escape(d);
        escape(s);
        R::run(d, s, n);
        sink += d[0]; // tocar el destino: la copia NO es codigo muerto
    }
    const auto dt = Clock::now() - t0;

    escape(sink);
    return double(std::chrono::duration_cast<std::chrono::nanoseconds>(dt)
                      .count()) /
           double(rounds);
}

/**
 * @brief Copias pequenas repartidas por una region de 16 MiB.
 *
 * El caso realista para un asignador: los bloques NO estan en cache, porque
 * entre una copia y la siguiente ha pasado el resto del programa.  El anterior
 * mide la rutina; este mide la rutina cuando hay que ir a buscar los datos.
 */
template <class R> double scattered(size_t n, int rounds) {
    constexpr size_t kSpan = 16u << 20;
    std::vector<uint8_t> area(kSpan + 4096);
    for (size_t i = 0; i < area.size(); i += 64)
        area[i] = uint8_t(i);

    uint64_t seed = 0x9E3779B97F4A7C15ull;
    uint64_t sink = 0;
    const size_t reach = kSpan - n - 64;

    const auto t0 = Clock::now();
    for (int i = 0; i < rounds; ++i) {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        // Dos posiciones al azar, que no se solapen con seguridad.
        const size_t a = size_t(seed >> 33) % (reach / 2);
        const size_t b = reach / 2 + (size_t(seed >> 11) % (reach / 2));
        uint8_t *d = area.data() + a;
        const uint8_t *s = area.data() + b;
        escape(d);
        escape(s);
        R::run(d, s, n);
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
    int rounds;
    size_t off_d, off_s;
    template <class R> double operator()() const {
        return hot<R>(n, rounds, off_d, off_s);
    }
};

struct ScatteredCase {
    size_t n;
    int rounds;
    template <class R> double operator()() const {
        return scattered<R>(n, rounds);
    }
};

template <class F> Pair abba(F run) {
    const double a1 = run.template operator()<Ours>();
    const double b1 = run.template operator()<Libc>();
    const double b2 = run.template operator()<Libc>();
    const double a2 = run.template operator()<Ours>();
    return {(a1 + a2) / 2.0, (b1 + b2) / 2.0};
}

void size_name(char *out, size_t cap, const char *label, size_t n) {
    if (n >= (1u << 20))
        std::snprintf(out, cap, "%s %zuM", label, n >> 20);
    else if (n >= 1024)
        std::snprintf(out, cap, "%s %zuK", label, n >> 10);
    else
        std::snprintf(out, cap, "%s %zu", label, n);
}

void row(const char *label, size_t n, Pair p) {
    char name[48];
    size_name(name, sizeof(name), label, n);

    const double ratio = p.ours > 0.0 ? p.libc / p.ours : 0.0;
    // GB/s de lo nuestro: en los tamanos grandes es el numero que importa,
    // porque dice si se esta llegando al techo de la memoria.
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

const size_t kSizes[] = {8,    16,   32,   64,      128,
                         256,  1024, 4096, 65536,   1u << 20};
const int kSizeCount = int(sizeof(kSizes) / sizeof(kSizes[0]));

/// Menos vueltas segun crece el bloque, o los casos grandes se comen el tiempo
/// de pared sin anadir informacion.
int rounds_for(size_t n) {
    if (n <= 256) return 2000000;
    if (n <= 4096) return 400000;
    if (n <= 65536) return 40000;
    return 3000;
}

} // namespace

int main() {
    std::printf("== vesta_memcpy vs the C library ==\n\n");
    std::printf("Both are called in this process, interleaved A-B-B-A, and the\n"
                "numbers are nanoseconds per copy.  A ratio above 1.00 means\n"
                "ours is that many times faster.\n");

    header("aligned, cache resident");
    for (int i = 0; i < kSizeCount; ++i) {
        const size_t n = kSizes[i];
        row("hot", n, abba(HotCase{n, rounds_for(n), 0, 0}));
    }

    header("misaligned by 3 and 1 (our moves are unaligned on purpose)");
    for (int i = 0; i < kSizeCount; ++i) {
        const size_t n = kSizes[i];
        row("odd", n, abba(HotCase{n, rounds_for(n), 3, 1}));
    }

    header("scattered over 16 MiB (not in cache: the realistic case)");
    for (int i = 0; i < kSizeCount - 2; ++i) { // 1 MiB no cabe repartido
        const size_t n = kSizes[i];
        row("cold", n, abba(ScatteredCase{n, rounds_for(n) / 8}));
    }

    std::printf("\nWhere ours wins is the small sizes, and the reason is that\n"
                "under 16 bytes it does not call anybody: overlapping blocks,\n"
                "no loop, no call.  Above the last level of cache the two are\n"
                "the same by construction -- that is memory bandwidth, and no\n"
                "routine can be faster than the memory.\n");
    return 0;
}
