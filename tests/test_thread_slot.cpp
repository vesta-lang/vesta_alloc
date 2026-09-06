/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/util/test_thread_slot.cpp
 * @brief Comprueba que el puntero por hilo aisla de verdad, y cuanto cuesta.
 *
 * Lo que importa que no falle: que un hilo no vea el valor de otro.  Si eso se
 * rompe, el asignador que va encima reparte bloques de un hilo a otro y el
 * fallo aparece lejisimos de aqui.
 */
#include "util/thread_slot.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

util::ThreadSlot g_slot;
util::ThreadSlot g_other; // una segunda, para ver que no se pisan
int g_failures = 0;

void check(bool ok, const char *what) {
    std::printf("  [%s] %s\n", ok ? "OK  " : "FALLO", what);
    if (!ok) ++g_failures;
}

} // namespace

int main() {
    std::printf("== puntero por hilo ==\n");
    check(g_slot.ensure(), "se reserva la ranura");
    check(g_other.ensure(), "se reserva una segunda ranura");
    std::printf("  lectura directa del TEB validada: %s\n",
                util::thread_slot_direct_ok() ? "SI" : "no (se usa la API)");

    check(g_slot.get() == nullptr, "sin poner nada, vale nulo");

    int a = 1, b = 2;
    g_slot.set(&a);
    g_other.set(&b);
    check(g_slot.get() == &a, "guarda y devuelve lo puesto");
    check(g_other.get() == &b, "dos ranuras no se pisan");

    // --- aislamiento entre hilos ----------------------------------------
    constexpr int kThreads = 8;
    std::atomic<int> wrong{0};
    std::vector<std::thread> workers;
    std::vector<int> values(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        values[i] = 1000 + i;
        workers.emplace_back([&, i] {
            // Cada hilo entra virgen: nadie ha puesto nada en SU ranura.
            if (g_slot.get() != nullptr) ++wrong;
            g_slot.set(&values[i]);
            // Se da tiempo a que los demas escriban, para que un fallo de
            // aislamiento se note en vez de pasar por casualidad.
            std::this_thread::yield();
            for (int r = 0; r < 1000; ++r)
                if (g_slot.get() != &values[i]) {
                    ++wrong;
                    break;
                }
        });
    }
    for (auto &t : workers)
        t.join();
    check(wrong.load() == 0, "cada hilo ve SOLO lo suyo");
    check(g_slot.get() == &a, "el hilo principal conserva el suyo");

    // --- coste -----------------------------------------------------------
    constexpr long kIters = 20000000;
    const auto t0 = std::chrono::steady_clock::now();
    unsigned long sink = 0;
    for (long i = 0; i < kIters; ++i)
        sink += reinterpret_cast<uintptr_t>(g_slot.get());
    const auto t1 = std::chrono::steady_clock::now();
    const double ns =
        std::chrono::duration<double, std::nano>(t1 - t0).count() / kIters;
    std::printf("  coste por acceso: %.2f ns\n", ns);
    if (sink == 7) std::printf("(imposible)\n");

    std::printf("%s\n", g_failures == 0 ? "TODO OK" : "HAY FALLOS");
    return g_failures == 0 ? 0 : 1;
}
