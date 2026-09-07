/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file tests/test_counters_agree.cpp
 * @brief Que las dos formas de contar una reserva digan lo mismo.
 *
 * EL FALLO QUE ESTE FICHERO PERSIGUE.  Al medir de donde vienen las reservas,
 * el informe de un compilador entero daba que los sitios sumaban el **224,5%**
 * de las reservas que el asignador decia haber servido.  Un porcentaje por
 * encima de cien no es una escala rara: significa que dos contadores que
 * cuentan LO MISMO no coinciden, y hasta saber cual falla el informe entero no
 * se puede leer.
 *
 * Lo que ya se sabe, medido con gdb sobre el binario y sin tocar una linea:
 *
 *   - por la puerta de `operator new` entraron 31.195 reservas y a la tabla de
 *     sitios llegaron 31.201 llamadas -- las mismas mas las seis de la variante
 *     `nothrow` --, o sea que **la tabla es exacta**;
 *   - en ese mismo instante, los contadores del asignador sumaban 10.996, en un
 *     solo cache y sin ninguno descartado.
 *
 * O sea que el que no cuadra es el contador, no la tabla.  Y falta la pregunta
 * que este test contesta: **pasa tambien con UN SOLO HILO?**  Si aqui cuadra,
 * lo que hay que mirar es lo que el compilador tiene y esto no; si no cuadra,
 * el fallo esta reproducido en algo que cabe en un fichero y se puede acorralar
 * sin arrancar un compilador entero.
 *
 * EL REPARTO IMITA AL DEL COMPILADOR y no es un bucle de tamano fijo: cuatro de
 * cada cinco reservas de 64 bytes o menos, cadenas, contenedores que crecen,
 * bloques sobre-alineados y algun tramo grande.  Un bucle de un solo tamano ya
 * se probo y cuadraba; lo que se busca es justamente lo que ese bucle no tiene.
 */

#include "util/alloc_sites.h"
#include "util/call_site.h"
#include "util/host_allocator.h"
#include "util/host_allocator_layout.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace util {
namespace detail {
extern bool g_measure;
}
} // namespace util

namespace {

int g_failures = 0;

void check(bool ok, const char *what) {
    std::printf("  [%s] %s\n", ok ? "OK  " : "FALLO", what);
    if (!ok) ++g_failures;
}

/// Las dos cifras que tienen que coincidir, leidas a la vez.
struct Shot {
    uint64_t entries = 0;   ///< entradas por la puerta de `operator new`
    uint64_t served = 0;    ///< reservas que el asignador dice haber servido
    uint64_t recorded = 0;  ///< lo que quedo en la tabla de sitios
};

static util::AllocSite g_snap[4096];

Shot take() {
    Shot s;
    s.entries = util::host_new_calls();
    const util::HostAllocStats st = util::host_alloc_stats();
    s.served = st.small_allocs + st.large_allocs;
    const unsigned n = util::alloc_sites_snapshot(g_snap, 4096);
    for (unsigned i = 0; i < n; ++i)
        s.recorded += g_snap[i].count - g_snap[i].over;
    return s;
}

/* Cada forma en su funcion, y sin meterla en linea: asi cada una es un SITIO
 * distinto en la tabla, que es como se ve cual de ellas descuadra si alguna lo
 * hace. */

/// Lo que domina en el compilador: bloques pequenos que nacen y mueren.
[[gnu::noinline]] void mix_small(int n) {
    std::vector<void *> live;
    live.reserve(64);
    for (int i = 0; i < n; ++i) {
        // Cuatro de cada cinco de 64 bytes o menos, como el reparto medido.
        const size_t sz = (i % 5 == 0) ? 32 + size_t(i % 900) : 16 + size_t(i % 48);
        live.push_back(::operator new(sz));
        if (live.size() == 64) {
            for (void *p : live)
                ::operator delete(p);
            live.clear();
        }
    }
    for (void *p : live)
        ::operator delete(p);
}

/// Cadenas: la frontera con lo ajeno, y donde mas se reserva al compilar.
[[gnu::noinline]] void mix_strings(int n) {
    std::vector<std::string> v;
    v.reserve(size_t(n));
    for (int i = 0; i < n; ++i)
        v.emplace_back(size_t(40 + (i % 300)), char('a' + (i & 15)));
}

/// Contenedores que CRECEN, que es el patron que reserva y abandona.
[[gnu::noinline]] void mix_growing(int n) {
    std::vector<int> v;
    for (int i = 0; i < n; ++i)
        v.push_back(i); // reserva, copia y suelta la anterior
    std::map<int, std::string> m;
    for (int i = 0; i < n / 8; ++i)
        m.emplace(i, "nodo"); // un nodo por elemento
}

/// Sobre-alineados, que entran por otra de las cuatro puertas parcheadas.
[[gnu::noinline]] void mix_aligned(int n) {
    std::vector<void *> v;
    v.reserve(size_t(n));
    for (int i = 0; i < n; ++i)
        v.push_back(::operator new(96, std::align_val_t(64)));
    for (void *p : v)
        ::operator delete(p, std::align_val_t(64));
}

/**
 * @brief Impide que el compilador se lleve por delante lo que se mide.
 *
 * HACE FALTA, Y ESTE TEST YA SE EQUIVOCO SIN ELLA.  El lenguaje deja OMITIR un
 * `new` seguido de su `delete`, y GCC lo hace: el caso de los tramos salia con
 * cero entradas y cero reservas, que se lee como "aqui no pasa nada" cuando lo
 * que pasaba es que no habia codigo.
 */
[[gnu::always_inline]] inline void keep(void *p) noexcept {
    __asm__ __volatile__("" : : "r"(p) : "memory");
}

/// Y algun tramo, que no pasa por las listas de clases.
[[gnu::noinline]] void mix_spans(int n) {
    for (int i = 0; i < n; ++i) {
        void *p = ::operator new(util::kMaxSmall + 1 + size_t(i % 4096));
        keep(p);
        ::operator delete(p);
    }
}

void report(const char *que, const Shot &a, const Shot &b) {
    const long long entries = (long long)(b.entries - a.entries);
    const long long served = (long long)(b.served - a.served);
    const long long recorded = (long long)(b.recorded - a.recorded);
    std::printf("  %-18s entradas %8lld   servidas %8lld   apuntadas %8lld",
                que, entries, served, recorded);
    if (served != 0)
        std::printf("   razon %.3f", double(entries) / double(served));
    std::printf("\n");
}

} // namespace

int main() {
    std::printf("== las dos formas de contar una reserva, con UN SOLO HILO ==\n");
    if (!util::host_alloc_active()) {
        std::printf("  asignador apagado (VESTA_NO_HOST_SLAB): nada que contar\n");
        return 0;
    }

    /* Se enciende aqui, no por el entorno: asi el test se vale por si mismo y no
     * depende de como lo lancen.  El parche es lo que hace que `operator new`
     * pase por el camino que cuenta; sin el no hay nada que comparar, y eso se
     * DICE en vez de dar un verde que no ha comprobado nada. */
    util::detail::g_measure = true;
    if (!util::install_call_site_patch()) {
        std::printf("  no se pudo parchear `operator new` en esta maquina: sin "
                    "eso no hay entradas que contar, asi que no se comprueba "
                    "nada.  No es un aprobado.\n");
        return 0;
    }
    check(util::call_site_patch_installed(), "el parche esta puesto");

    /* Una vuelta en vacio antes de medir: la primera reserva de cada clase
     * monta cosas -- la fila de sitios, el primer trozo -- y eso ensuciaria la
     * primera ventana. */
    mix_small(200);
    mix_strings(50);

    struct Caso {
        const char *nombre;
        void (*fn)(int);
        int n;
    };
    const Caso casos[] = {
        {"pequenas", mix_small, 20000},   {"cadenas", mix_strings, 4000},
        {"que crecen", mix_growing, 8000}, {"alineadas", mix_aligned, 3000},
        {"tramos", mix_spans, 300},
    };

    Shot total_a = take();
    for (const Caso &c : casos) {
        const Shot a = take();
        c.fn(c.n);
        const Shot b = take();
        report(c.nombre, a, b);
    }
    const Shot total_b = take();

    std::printf("\n");
    report("TOTAL", total_a, total_b);

    /* Y LO MISMO CON HILOS, que es la unica diferencia de bulto entre esto y un
     * compilador de verdad.  Los hilos NACEN Y MUEREN a proposito: al morir
     * devuelven su identificador y otro lo hereda, y eso mueve de sitio tanto la
     * fila de la tabla como el cache donde se cuenta.  Si el descuadre sale
     * aqui, ya no hay que arrancar un compilador para acorralarlo. */
    const Shot hilos_a = take();
    {
        std::vector<std::thread> pool;
        for (int r = 0; r < 8; ++r)
            pool.emplace_back([] {
                mix_small(4000);
                mix_strings(500);
                mix_growing(1000);
            });
        for (auto &t : pool)
            t.join();
    }
    /* Y otra tanda DESPUES de que mueran, para que los identificadores se
     * reciclen y las cuentas caigan sobre caches heredados. */
    {
        std::vector<std::thread> pool;
        for (int r = 0; r < 8; ++r)
            pool.emplace_back([] { mix_small(4000); });
        for (auto &t : pool)
            t.join();
    }
    const Shot hilos_b = take();
    std::printf("\n");
    report("CON HILOS", hilos_a, hilos_b);

    const long long h_entries = (long long)(hilos_b.entries - hilos_a.entries);
    const long long h_served = (long long)(hilos_b.served - hilos_a.served);
    const double h_razon =
        h_served == 0 ? 0.0 : double(h_entries) / double(h_served);
    check(h_served > 0, "la tanda con hilos tambien reservo");
    check(h_razon > 0.98 && h_razon < 1.02,
          "y con hilos cada entrada sigue siendo UNA reserva servida");
    if (!(h_razon > 0.98 && h_razon < 1.02))
        std::printf("  razon %.3f con hilos frente a la de un hilo: AHI esta la "
                    "diferencia\n",
                    h_razon);

    const long long entries = (long long)(total_b.entries - total_a.entries);
    const long long served = (long long)(total_b.served - total_a.served);

    /* LA COMPROBACION.  No se exige igualdad exacta: leer las dos cifras no es
     * atomico y entre una y otra el propio test reserva para su vector de
     * resultados.  Se exige que no haya un DESVIO, que es lo que se vio en el
     * compilador: alli las entradas eran 2,25 veces las servidas.  Un 2% de
     * margen deja pasar el ruido de la medida y no un factor. */
    const double razon = served == 0 ? 0.0 : double(entries) / double(served);
    check(served > 0, "el banco paso de verdad por el asignador");
    check(razon > 0.98 && razon < 1.02,
          "cada entrada por `operator new` es UNA reserva servida");

    if (!(razon > 0.98 && razon < 1.02))
        std::printf("  razon %.3f: con un solo hilo TAMBIEN descuadra, asi que "
                    "no son los hilos\n",
                    razon);

    util::detail::g_measure = false;
    std::printf(g_failures == 0 ? "TODO OK\n" : "%d FALLOS\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
