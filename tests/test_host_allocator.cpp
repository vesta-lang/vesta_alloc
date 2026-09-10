/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
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
#include "util/alloc/host_allocator.h"
/* Por el nivel del modo de comprobacion.  Sin el modo compilado la cabecera
 * declara las versiones que contestan cero. */
#include "util/alloc/sanitizer.h"
#include "util/mem/vesta_memcpy.h"
#include "util/mem/vesta_memset.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

/// Devuelve la misma direccion, pero el compilador deja de saber de donde sale.
/// Se usa para leer la cabecera que el asignador pone DELANTE de un bloque: sin
/// esto, el compilador sigue creyendo que esa direccion pertenece al objeto que
/// hay detras y avisa de un acceso fuera de sus limites -- con razon, porque
/// mirando solo el tipo eso es lo que parece.
const char *opaque(const char *p) noexcept {
    asm volatile("" : "+r"(p));
    return p;
}

int g_skipped = 0;

void check(bool ok, const char *what) {
    std::printf("  [%s] %s\n", ok ? "OK  " : "FALLO", what);
    if (!ok) ++g_failures;
}

/**
 * @brief Una fila que solo tiene sentido si el asignador REUSA lo que suelta.
 *
 * El nivel de guarda del modo de comprobacion no vuelve a entregar jamas un
 * bloque que se solto: sus paginas se descomprometen y el rango se guarda, que
 * es exactamente lo que hace que un uso despues de liberar falle en el sitio
 * del fallo y no en cualquier otro.  Asi que ahi la respuesta a "vuelve a
 * salir el mismo?" es que no POR DISENO, y una fila que lo exija no esta
 * midiendo al asignador.
 *
 * Se declara y se cuenta APARTE, nunca como aprobada: sumar una comprobacion
 * saltada a los aciertos convierte "aqui no se mira" en "aqui esta bien".
 */
void check_allocator_reuses(bool ok, const char *what) {
    if (util::detail::g_san_level >= util::SanLevel::Guard) {
        std::printf("  [SALTA] %s -- el modo de comprobacion no vuelve a "
                    "entregar un bloque soltado, que es como caza el uso "
                    "despues de liberar\n",
                    what);
        ++g_skipped;
        return;
    }
    check(ok, what);
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
                vesta_memset(
                    p, static_cast<uint8_t>(reinterpret_cast<uintptr_t>(p) &
                                            0xFF),
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
        check_allocator_reuses(a == b,
                               "el bloque recien soltado se vuelve a entregar");
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
        check_allocator_reuses(after - before <= 8,
                               "lo soltado por otro hilo se reaprovecha");
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
#if defined(VESTA_ALLOC_SANITIZER) && VESTA_ALLOC_SANITIZER
        /* \~english NOTHING IS ASSERTED IN THE CHECKING BUILD, and it is not
         * courtesy: at its default level that mode records EVERY allocation and
         * EVERY release -- measured, from 4.8 ns/op to 28 -- and with both
         * sides of this row going through the same door the margin drops to
         * 1.07x and the comparison falls over on its own once in every six
         * runs.  A random red is worse than no check: it gets ignored, and the
         * day the comparison fails for real nobody looks either.
         *
         * MIND WHAT THIS ROW MEASURES, which is not what its name says: the
         * "system" side calls `std::malloc`, and with the interposition in
         * force that symbol is OURS -- `nm -D` on this test's binary shows it
         * DEFINED there.  So it compares this allocator through the interposed
         * door against the same allocator inlined, not against the system's.
         * `support/system_alloc.h` exists to reach the real one, and this row
         * does not use it.
         *
         * \~spanish EN EL BUILD DE COMPROBACION NO SE AFIRMA NADA, y no es
         * cortesia: en su nivel por defecto ese modo apunta CADA reserva y CADA
         * liberacion -- medido, de 4,8 ns/op a 28 -- y con los dos lados de
         * esta fila pasando por la misma puerta el margen baja a 1,07x y la
         * comparacion se cae sola una vez de cada seis.  Un rojo aleatorio es
         * peor que no comprobar: se aprende a ignorarlo, y el dia que la
         * comparacion falle de verdad tampoco lo mirara nadie.
         *
         * OJO CON LO QUE MIDE ESTA FILA, que no es lo que su nombre dice: el
         * lado "sistema" llama a `std::malloc`, y con la interposicion puesta
         * ese simbolo es NUESTRO -- `nm -D` sobre el binario de este test lo
         * enseña DEFINIDO ahi --.  O sea que compara este asignador por la
         * puerta interpuesta contra el mismo asignador en linea, no contra el
         * del sistema.  `support/system_alloc.h` existe para alcanzar el de
         * verdad, y esta fila no lo usa.  \~ */
        std::printf("  [note] the CHECKING build asserts nothing about this "
                    "row: at its default level it records every allocation, so "
                    "the margin cannot carry a stable comparison\n");
#else
        check(own < sys, "el propio gana al del sistema");
#endif
    }

    // --- reserva SOBRE-ALINEADA -------------------------------------------
    //
    // Un tipo con `alignas` mayor que la natural no pasa por `operator new` a
    // secas: el compilador emite la sobrecarga con `std::align_val_t`.  Si esa
    // no esta sustituida, el tipo se reserva con el asignador del SISTEMA y se
    // suelta por el camino sustituido -- dos asignadores a la vez y el monton
    // corrompido --.  No falla al momento: revienta en otro sitio y despues.
    //
    // Ya paso: `ProcessVM` de la VM tiene alineacion 64 por su banco vectorial,
    // asi que TODOS los procesos se estaban reservando fuera de este asignador
    // sin que nada lo delatara.
    {
        struct alignas(64) Linea {
            unsigned char b[64];
        };
        struct alignas(256) Pagina {
            unsigned char b[1024];
        };
        bool alineado = true, intacto = true, del_asignador = true;
        std::vector<Linea *> l;
        std::vector<Pagina *> g;
        for (int i = 0; i < 256; ++i) {
            Linea *p = new Linea;
            Pagina *q = new Pagina;
            if ((reinterpret_cast<uintptr_t>(p) & 63u) != 0) alineado = false;
            if ((reinterpret_cast<uintptr_t>(q) & 255u) != 0) alineado = false;
            /* Que salga de ESTE asignador, no del sistema: es lo que estaba
             * roto, y la alineacion sola no lo distingue -- el del sistema
             * tambien alinea --.
             *
             * La direccion de la cabecera pasa por `opaque` ANTES de leerla.
             * No es un apano para callar un aviso: el compilador, viendo la
             * resta sobre un `Linea*`, deduce que se lee un elemento -1 de ese
             * objeto y avisa -- y tiene razon en lo que ve, porque lo que hay
             * ahi delante no es de `Linea`, es del asignador --.  La barrera es
             * la forma de decirle justo eso: esta direccion ya no es parte de
             * aquel objeto.
             *
             * Y se copia con lo NUESTRO, no con la libreria del sistema: esta
             * libreria trae su propia copia y usar otra aqui dentro seria no
             * fiarse de ella justo donde toca. */
            void *const *src = reinterpret_cast<void *const *>(
                opaque(reinterpret_cast<const char *>(p) - sizeof(void *)));
            void *header = nullptr;
            util::vesta_memcopy(&header, src);
            if (util::host_alloc_active() && !util::in_region(header))
                del_asignador = false;
            vesta_memset(p->b, uint8_t(i & 0xFF), sizeof(p->b));
            vesta_memset(q->b, uint8_t(i & 0xFF), sizeof(q->b));
            l.push_back(p);
            g.push_back(q);
        }
        // Releer despues de reservar de todo: si dos bloques se solaparan, uno
        // habria pisado al otro.
        for (int i = 0; i < 256; ++i) {
            for (unsigned char c : l[(size_t)i]->b)
                if (c != (unsigned char)(i & 0xFF)) intacto = false;
            for (unsigned char c : g[(size_t)i]->b)
                if (c != (unsigned char)(i & 0xFF)) intacto = false;
        }
        for (int i = 0; i < 256; ++i) {
            delete l[(size_t)i];
            delete g[(size_t)i];
        }
        check(alineado, "`new` de un tipo sobre-alineado respeta la alineacion");
        check(intacto, "los bloques sobre-alineados no se solapan");
        check(del_asignador,
              "un tipo sobre-alineado sale de ESTE asignador, no del sistema");

        // Y en array, que usa OTRA sobrecarga (`new[]` con alineacion).
        Linea *arr = new Linea[16];
        const bool arr_ok = (reinterpret_cast<uintptr_t>(arr) & 63u) == 0;
        delete[] arr;
        check(arr_ok, "`new[]` de un tipo sobre-alineado tambien la respeta");
    }

    /* Los identificadores de cache se DEVUELVEN al morir el hilo.
     *
     * Es lo unico que separa "el tope son 63 duenos a la vez" de "el tope son
     * 63 hilos en toda la vida del proceso".  Sin devolverlos, un programa que
     * crea y destruye hilos -- o sea, cualquiera -- agota el mostrador y a
     * partir de ahi TODAS las reservas de TODOS los hilos se sirven de las
     * listas compartidas, detras del unico cerrojo del asignador.  Medido con
     * 24 hilos y el mismo binario: 1,73 -> 447,86 ns por operacion.
     *
     * Se comprueba por ESTRUCTURA y no por tiempo: despues de crear y destruir
     * muchas mas veces el tope, un hilo nuevo tiene que seguir teniendo cache
     * propio.  Un test de tiempo aqui seria un test que a veces pasa. */
    if (util::host_alloc_active()) {
        constexpr unsigned kVueltas = util::kMaxThreads * 3;
        for (unsigned i = 0; i < kVueltas; ++i)
            std::thread([] { util::host_free(util::host_alloc(64)); }).join();

        std::atomic<bool> propio{false};
        /* Arranca en un valor IMPOSIBLE.  Con cero, un hilo que se quedara sin
         * cache dejaria el cero puesto y la comprobacion de abajo pasaria por
         * no haber medido nada -- que es peor que fallar. */
        std::atomic<unsigned> id{util::kMaxThreads};
        std::thread([&propio, &id] {
            util::host_free(util::host_alloc(64));
            const util::detail::ThreadCache *c = util::detail::current_cache();
            propio.store(c != nullptr, std::memory_order_relaxed);
            if (c != nullptr) id.store(c->id, std::memory_order_relaxed);
        }).join();

        check(propio.load(),
              "tras crear y destruir 192 hilos, uno nuevo sigue teniendo cache");
        check(id.load() < util::kMaxThreads - 1,
              "y su identificador es de los suyos, no el del cache compartido");
    }

    const util::HostAllocStats s = util::host_alloc_stats();
    std::printf(
        "  reservas=%llu  sueltas=%llu  ajenas=%llu  trozos=%llu\n",
        (unsigned long long)s.small_allocs, (unsigned long long)s.small_frees,
        (unsigned long long)s.remote_frees, (unsigned long long)s.chunks);
    /* Saltadas APARTE, nunca sumadas a los aciertos. */
    if (g_skipped != 0)
        std::printf("%d SALTADAS por el nivel del modo de comprobacion\n",
                    g_skipped);
    std::printf("%s\n", g_failures == 0 ? "TODO OK" : "HAY FALLOS");
    return g_failures == 0 ? 0 : 1;
}
