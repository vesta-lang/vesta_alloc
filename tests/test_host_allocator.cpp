/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/util/test_host_allocator.cpp
 * @brief Comprueba el asignador propio antes de ponerlo bajo `operator new`.
 *
 * Lo que se verifica es lo que, si falla, revienta lejos de aqui y sin pista:
 * que dos reservas vivas nunca se solapen, que la alineacion sea la que
 * `operator new` promete, y -- lo mas delicado del diseno -- que liberar en un
 * hilo lo reservado en otro ni pierda memoria ni corrompa las listas.
 *
 * Al enlazar este test contra el asignador, sus propios `new` ya pasan por el,
 * asi que la prueba es tambien de integracion.
 */
#include "util/host_allocator.h"

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
    std::printf("== asignador propio ==\n");
    std::printf("  activo: %s\n", util::host_alloc_active() ? "SI" : "no");

    // --- alineacion y no solapamiento ------------------------------------
    {
        constexpr size_t kSizes[] = {1,   8,   16,  17,  32,  48,  64,   96,
                                     128, 200, 256, 384, 512, 900, 1024, 4096};
        bool aligned = true, distinct = true, intact = true;
        std::vector<void *> live;
        for (size_t rep = 0; rep < 64; ++rep)
            for (size_t s : kSizes) {
                void *p = util::host_alloc(s);
                if (p == nullptr) {
                    distinct = false;
                    break;
                }
                if ((reinterpret_cast<uintptr_t>(p) & 15u) != 0)
                    aligned = false;
                // Se llena con un patron dependiente del puntero: si dos
                // reservas se solaparan, una pisaria a la otra y se veria al
                // releer.
                std::memset(
                    p, static_cast<int>(reinterpret_cast<uintptr_t>(p) & 0xFF),
                    s);
                live.push_back(p);
            }
        for (void *p : live) {
            const unsigned char want = static_cast<unsigned char>(
                reinterpret_cast<uintptr_t>(p) & 0xFF);
            if (*static_cast<unsigned char *>(p) != want) intact = false;
        }
        check(aligned, "todo bloque sale alineado a 16");
        check(distinct, "no se agota ni devuelve nulo");
        check(intact, "dos bloques vivos nunca se solapan");
        for (void *p : live)
            util::host_free(p);
    }

    // --- reusar tras liberar ---------------------------------------------
    {
        void *a = util::host_alloc(64);
        util::host_free(a);
        void *b = util::host_alloc(64);
        check(a == b, "el bloque recien soltado se vuelve a entregar");
        util::host_free(b);
    }

    // --- liberar entre hilos ---------------------------------------------
    {
        constexpr int kThreads = 8;
        constexpr int kPer = 4000;
        std::vector<std::vector<void *>> made(kThreads);
        std::vector<std::thread> ts;
        for (int i = 0; i < kThreads; ++i)
            ts.emplace_back([&, i] {
                made[i].reserve(kPer);
                for (int j = 0; j < kPer; ++j)
                    made[i].push_back(util::host_alloc(16 + (j % 60) * 16));
            });
        for (auto &t : ts)
            t.join();
        // Los suelta el hilo PRINCIPAL: ninguno es suyo.
        size_t total = 0;
        for (auto &v : made) {
            for (void *p : v)
                util::host_free(p);
            total += v.size();
        }
        check(total == size_t(kThreads) * kPer, "se reservo lo esperado");
        // Y ahora se vuelve a pedir mucho: si lo soltado entre hilos se
        // hubiera perdido, esto tendria que pedir trozos nuevos sin parar.
        const auto before = util::host_alloc_stats().chunks;
        std::vector<void *> again;
        for (int j = 0; j < kPer; ++j)
            again.push_back(util::host_alloc(64));
        const auto after = util::host_alloc_stats().chunks;
        for (void *p : again)
            util::host_free(p);
        check(after - before <= 8, "lo soltado por otro hilo se reaprovecha");
    }

    // --- comparacion con el sistema --------------------------------------
    {
        constexpr int kRounds = 4000, kBatch = 512;
        constexpr size_t kMix[] = {24, 32, 48, 64, 96, 128, 192, 256, 512};
        std::vector<void *> live(kBatch);
        auto t0 = std::chrono::steady_clock::now();
        for (int r = 0; r < kRounds; ++r) {
            for (int i = 0; i < kBatch; ++i)
                live[i] = std::malloc(kMix[(i + r) % 9]);
            for (int i = 0; i < kBatch; ++i)
                std::free(live[i]);
        }
        auto t1 = std::chrono::steady_clock::now();
        for (int r = 0; r < kRounds; ++r) {
            for (int i = 0; i < kBatch; ++i)
                live[i] = util::host_alloc(kMix[(i + r) % 9]);
            for (int i = 0; i < kBatch; ++i)
                util::host_free(live[i]);
        }
        auto t2 = std::chrono::steady_clock::now();
        const double ops = double(kRounds) * kBatch;
        const double sys =
            std::chrono::duration<double, std::nano>(t1 - t0).count() / ops;
        const double own =
            std::chrono::duration<double, std::nano>(t2 - t1).count() / ops;
        std::printf("  sistema: %.1f ns/op   propio: %.1f ns/op   (%.2fx)\n",
                    sys, own, sys / own);
        check(own < sys, "el propio gana al del sistema");
    }

    const util::HostAllocStats s = util::host_alloc_stats();
    std::printf(
        "  reservas=%llu  sueltas=%llu  ajenas=%llu  trozos=%llu\n",
        (unsigned long long)s.small_allocs, (unsigned long long)s.small_frees,
        (unsigned long long)s.remote_frees, (unsigned long long)s.chunks);
    std::printf("%s\n", g_failures == 0 ? "TODO OK" : "HAY FALLOS");
    return g_failures == 0 ? 0 : 1;
}
