/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file tests/util/test_thread_slot.cpp
 * @brief Comprueba que el puntero por hilo aisla de verdad, y cuanto cuesta.
 *
 * Lo que importa que no falle: que un hilo no vea el valor de otro.  Si eso se
 * rompe, el asignador que va encima reparte bloques de un hilo a otro y el
 * fallo aparece lejisimos de aqui.
 */
#include "util/os/thread_slot.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

util::ThreadSlot g_slot;
util::ThreadSlot g_other; // una segunda, para ver que no se pisan
util::ThreadSlot g_exit;  // una tercera, para el aviso de fin de hilo
int g_failures = 0;

void check(bool ok, const char *what) {
    std::printf("  [%s] %s\n", ok ? "OK  " : "FALLO", what);
    if (!ok) ++g_failures;
}

std::atomic<int> g_exit_count{0};
std::atomic<void *> g_exit_value{nullptr};

/// Lo que el sistema llama al morir un hilo.  Minimo a proposito: corre durante
/// el desmontaje del hilo.
void on_exit(void *value) {
    g_exit_value.store(value, std::memory_order_relaxed);
    g_exit_count.fetch_add(1, std::memory_order_relaxed);
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

    // --- el aviso de fin de hilo -----------------------------------------
    //
    // Es lo que permite reciclar el identificador de cache del asignador.  Sin
    // el, el tope de `kMaxThreads` deja de contar hilos VIVOS y pasa a contar
    // hilos que hayan existido alguna vez, y un programa que crea y destruye
    // hilos acaba sirviendose entero por las listas compartidas.
    {
        g_exit_count.store(0, std::memory_order_relaxed);
        g_exit_value.store(nullptr, std::memory_order_relaxed);
        const bool armed = g_exit.ensure() && g_exit.notify_on_exit(&on_exit);
        check(armed, "el sistema da un canal de aviso de fin de hilo");

        if (armed) {
            int marca = 0;
            std::thread([&marca] { g_exit.set(&marca); }).join();
            check(g_exit_count.load() == 1, "al morir un hilo con valor, avisa");
            check(g_exit_value.load() == &marca, "y avisa con SU valor");

            /* Un hilo que nunca puso nada no tiene de que avisar.  Importa:
             * si avisara con nulo, quien recicle identificadores tendria que
             * distinguirlo, y ese es el tipo de caso que se olvida. */
            g_exit_count.store(0, std::memory_order_relaxed);
            std::thread([] {}).join();
            check(g_exit_count.load() == 0, "un hilo sin valor no avisa nada");
        }
    }

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
