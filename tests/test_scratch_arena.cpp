/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/util/test_scratch_arena.cpp
 * @brief Comprueba la arena de fase: que reparte bien, que la vuelta atras
 *        reaprovecha y que dos hilos no se pisan.
 *
 * Lo que mas importa que no falle es la vuelta atras: si al soltar una fase la
 * arena no reaprovechara sus bloques, pediria memoria sin parar y el programa
 * se comeria la maquina en vez de ir mas rapido.
 */
#include "util/scratch_arena.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const char *what) {
    std::printf("  [%s] %s\n", ok ? "OK  " : "FALLO", what);
    if (!ok) ++g_failures;
}

} // namespace

int main() {
    std::printf("== arena de fase ==\n");
    util::ScratchArena &a = util::scratch_arena();

    // --- reparto y alineacion --------------------------------------------
    {
        bool aligned = true, distinct = true;
        std::vector<void *> got;
        for (int i = 0; i < 5000; ++i) {
            const size_t n = 8 + (i % 200);
            const size_t al = (i % 4 == 0) ? 32 : 8;
            void *p = a.allocate(n, al);
            if (p == nullptr) {
                distinct = false;
                break;
            }
            if (reinterpret_cast<uintptr_t>(p) % al != 0) aligned = false;
            std::memset(p, i & 0xFF, n);
            got.push_back(p);
        }
        check(aligned, "respeta la alineacion pedida");
        check(distinct, "no se queda sin memoria");
    }

    // --- la vuelta atras reaprovecha -------------------------------------
    {
        const size_t before = a.reserved_bytes();
        for (int fase = 0; fase < 200; ++fase) {
            util::ScratchScope scope;
            for (int i = 0; i < 2000; ++i)
                (void)a.allocate(64, 8);
        }
        const size_t after = a.reserved_bytes();
        check(after == before,
              "200 fases iguales no piden ni un byte mas al sistema");
    }

    // --- anidamiento -------------------------------------------------------
    {
        util::ScratchScope outer;
        void *p1 = a.allocate(128, 8);
        {
            util::ScratchScope inner;
            (void)a.allocate(4096, 8);
        }
        void *p2 = a.allocate(128, 8);
        check(p1 != p2, "lo de la fase exterior sobrevive a la interior");
    }

    // --- un hilo no ve la arena de otro -----------------------------------
    {
        constexpr int kThreads = 6;
        std::atomic<int> shared{0};
        std::vector<util::ScratchArena *> seen(kThreads, nullptr);
        std::vector<std::thread> ts;
        for (int i = 0; i < kThreads; ++i)
            ts.emplace_back([&, i] { seen[i] = &util::scratch_arena(); });
        for (auto &t : ts)
            t.join();
        for (int i = 0; i < kThreads; ++i)
            for (int j = i + 1; j < kThreads; ++j)
                if (seen[i] == seen[j]) ++shared;
        check(shared.load() == 0, "cada hilo tiene la suya");
    }

    // --- coste frente a pedir al asignador general ------------------------
    {
        constexpr int kIter = 2000000;
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kIter; ++i) {
            void *p = ::operator new(48);
            ::operator delete(p);
        }
        auto t1 = std::chrono::steady_clock::now();
        {
            util::ScratchScope scope;
            for (int i = 0; i < kIter; ++i)
                (void)a.allocate(48, 8);
        }
        auto t2 = std::chrono::steady_clock::now();
        const double gen =
            std::chrono::duration<double, std::nano>(t1 - t0).count() / kIter;
        const double arena =
            std::chrono::duration<double, std::nano>(t2 - t1).count() / kIter;
        std::printf("  general: %.2f ns/op   arena: %.2f ns/op   (%.1fx)\n",
                    gen, arena, gen / arena);
        check(arena < gen, "la arena gana al asignador general");
    }

    std::printf("  memoria pedida por la arena: %.1f KB\n",
                double(a.reserved_bytes()) / 1024.0);
    std::printf("%s\n", g_failures == 0 ? "TODO OK" : "HAY FALLOS");
    return g_failures == 0 ? 0 : 1;
}
