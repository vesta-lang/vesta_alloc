/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file tests/test_span_race.cpp
 * @brief El camino de TRAMOS con varios hilos a la vez, a proposito.
 *
 * Los demas tests de tramos comprueban lo que hace el asignador; este comprueba
 * lo que hace cuando MUCHOS hilos lo usan al mismo tiempo, que es donde vivia
 * un fallo que ninguno de los otros veia:
 *
 *   `right_neighbour` usaba `g_chunk_next` como "hasta aqui se puede leer", y
 *   ese contador sube al REPARTIR un trozo -- antes de comprometerlo y antes de
 *   escribir su cabecera --.  En esa ventana, un hilo que soltaba un tramo
 *   miraba a su derecha, creia que habia vecino y leia memoria sin
 *   comprometer.  Acababa de dos formas: acceso invalido, o -- peor -- basura
 *   que pasaba por tramo libre y se absorbia.
 *
 * De ahi salen las dos mitades de este fichero, y las dos hacen falta:
 *
 *  1. **Un banco de tortura** que ataca a la vez por los seis caminos que tocan
 *     las listas de tramos o el reparto de trozos: reservar y soltar de tamano
 *     variable, CRECER en el sitio, soltar desde otro hilo, rafagas de reservas
 *     pequenas, reservas a cero y oleadas.  Cada bloque va FIRMADO -- lleva
 *     cuanto mide y con que patron esta escrito --, asi que dos bloques vivos
 *     que se solapen se pisan la firma y se cuentan.  Sin la firma, la unica
 *     corrupcion que se ve es la que ademas casca, y esa es la suerte.
 *  2. **Una comprobacion de que FUSIONAR SIGUE FUNCIONANDO.**  El arreglo de la
 *     carrera cambio la pregunta "esta repartida esa memoria?" por "hay un
 *     tramo libre que empieza justo ahi?".  Si ese mapa dejara de marcarse, no
 *     habria ningun error: simplemente no se fusionaria nunca, la memoria
 *     creceria y nadie se enteraria.  Es justo el modo de fallo mudo que este
 *     proyecto persigue, asi que se comprueba con nombres y direcciones.
 */

#include "util/alloc/host_allocator.h"
#include "util/alloc/host_allocator_layout.h"
/* Por el nivel del modo de comprobacion.  Sin el modo compilado la cabecera
 * declara las versiones que contestan cero. */
#include "util/alloc/sanitizer.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace {

int failures = 0;
int skipped = 0;

void check(bool ok, const char *what) {
    std::printf("  [%s] %s\n", ok ? "OK  " : "FALLO", what);
    if (!ok) ++failures;
}

/**
 * @brief Una fila que depende de DONDE coloca el asignador sus tramos.
 *
 * En el nivel de guarda del modo de comprobacion cada bloque sale de una
 * reserva propia del sistema, asi que dos que se piden seguidos no tienen por
 * que quedar pegados -- y no quedarlo no dice nada del asignador, que ahi no ha
 * colocado nada.
 *
 * Se declara y se cuenta APARTE, nunca como aprobada.
 */
void check_allocator_placed(bool ok, const char *what) {
    if (util::detail::g_san_level >= util::SanLevel::Guard) {
        std::printf("  [SALTA] %s -- el modo de comprobacion sirve cada bloque "
                    "de una reserva propia, asi que no los coloca el "
                    "asignador\n",
                    what);
        ++skipped;
        return;
    }
    check(ok, what);
}

// -------------------------------------------------------------------------
//  Numeros al azar, uno por hilo y sin compartir nada.
// -------------------------------------------------------------------------
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t v) noexcept : s(v | 1u) {}
    uint64_t next() noexcept {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    uint32_t below(uint32_t n) noexcept { return uint32_t((next() >> 32) % n); }
};

// -------------------------------------------------------------------------
//  La firma que lleva cada bloque.
// -------------------------------------------------------------------------

/// Va al principio del bloque.  Se escribe al reservar y se comprueba al
/// soltar, al crecer y al recogerlo de otro hilo.  Es autodescriptiva a
/// proposito: asi la puede verificar un hilo que no sepa quien la pidio.
struct Mark {
    uint64_t magic; ///< kMarkMagic ^ seed: delata que alguien escribio encima
    uint64_t seed;  ///< de aqui salen los testigos del medio y del final
    uint64_t bytes; ///< lo que se PIDIO, no lo que se entrego
};

constexpr uint64_t kMarkMagic = 0x5350414E53494721ull; // "SPANSIG!"

/// Lo mas pequeno que se puede firmar.  Por debajo de esto el testigo del medio
/// cae DENTRO de la propia firma y la destroza -- el banco se acusaria a si
/// mismo de corromper.
constexpr size_t kMinBlock = 2 * sizeof(Mark) + 2 * sizeof(uint64_t);

/// Lo mas grande que este banco llega a pedir.  Sirve para que una firma con un
/// tamano imposible se note.
constexpr size_t kMaxBlock =
    size_t(util::kMaxSpanChunks + 2) * util::kChunkBytes;

/// Hasta donde se deja crecer un bloque.  Un bloque crece repitiendo la
/// operacion de crecer, asi que sin tope subiria sin fin y el test acabaria
/// midiendo copias de memoria.  Diecisiete trozos pasan de largo el cache por
/// hilo y siguen siendo baratos de copiar.
constexpr size_t kMaxGrow = 17 * util::kChunkBytes;

/// Los tres testigos: al principio, a la mitad y pegado al final.  Tres puntos
/// bastan -- escribir el bloque entero convertiria esto en un medidor de ancho
/// de banda de memoria y taparia lo que se quiere ver.
void sign(void *p, size_t n, uint64_t seed) noexcept {
    Mark *m = static_cast<Mark *>(p);
    m->magic = kMarkMagic ^ seed;
    m->seed = seed;
    m->bytes = n;
    char *base = static_cast<char *>(p);
    const uint64_t mid = seed * 0x9E3779B97F4A7C15ull;
    const uint64_t tail = seed ^ 0xD1B54A32D192ED03ull;
    std::memcpy(base + n / 2, &mid, sizeof(mid));
    std::memcpy(base + n - sizeof(tail), &tail, sizeof(tail));
}

std::atomic<uint64_t> g_bad_magic{0};
std::atomic<uint64_t> g_bad_mid{0};
std::atomic<uint64_t> g_bad_tail{0};
std::atomic<uint64_t> g_bad_size{0};

/// @return el tamano pedido, o 0 si la firma no cuadra (y entonces ya esta
///         contado en el contador que corresponda).
size_t verify(void *p) noexcept {
    const Mark *m = static_cast<const Mark *>(p);
    const uint64_t seed = m->seed;
    if (m->magic != (kMarkMagic ^ seed)) {
        g_bad_magic.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    const size_t n = size_t(m->bytes);
    if (n < kMinBlock || n > kMaxBlock) {
        g_bad_size.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    const char *base = static_cast<const char *>(p);
    uint64_t mid = 0, tail = 0;
    std::memcpy(&mid, base + n / 2, sizeof(mid));
    std::memcpy(&tail, base + n - sizeof(tail), sizeof(tail));
    if (mid != seed * 0x9E3779B97F4A7C15ull)
        g_bad_mid.fetch_add(1, std::memory_order_relaxed);
    if (tail != (seed ^ 0xD1B54A32D192ED03ull))
        g_bad_tail.fetch_add(1, std::memory_order_relaxed);
    return n;
}

// -------------------------------------------------------------------------
//  El buzon para soltar desde otro hilo.
// -------------------------------------------------------------------------

/// Una linea entera por ranura: lo que se quiere probar es el asignador, no la
/// falsa comparticion del propio test.
struct alignas(64) SwapSlot {
    std::atomic<void *> p{nullptr};
    char pad[64 - sizeof(std::atomic<void *>)];
};
constexpr unsigned kSwapSlots = 32;
SwapSlot g_swap[kSwapSlots];

std::atomic<bool> g_go{false};
std::atomic<uint64_t> g_oom{0};

constexpr unsigned kLive = 12; ///< bloques vivos por hilo

/// Un tamano de tramo: por encima del tope de clases y de uno a @p max trozos.
/// Se resta la cabecera para caer JUSTO en ese numero de trozos y no en el
/// siguiente, que es lo que deja a los tramos pegados y hace posible fusionar.
size_t span_size(Rng &r, uint32_t max) noexcept {
    const uint32_t chunks = 1 + r.below(max);
    const size_t n = size_t(chunks) * util::kChunkBytes - 64;
    return n > util::kMaxSmall ? n : util::kMaxSmall + 1;
}

void worker(unsigned id, uint64_t steps, uint64_t seed) {
    while (!g_go.load(std::memory_order_acquire))
        std::this_thread::yield();

    Rng r(seed + id * 0x9E3779B97F4A7C15ull);
    void *live[kLive] = {};

    for (uint64_t i = 0; i < steps; ++i) {
        const uint32_t op = r.below(16);
        const unsigned slot = r.below(kLive);

        switch (op) {
        case 0:
        case 1:
        case 2:
        case 3:
        case 4:
        case 5: { // reservar un tramo
            if (live[slot] != nullptr) {
                verify(live[slot]);
                util::host_free(live[slot]);
                live[slot] = nullptr;
            }
            const size_t n = span_size(r, 6);
            void *p = util::host_alloc(n);
            if (p == nullptr) {
                g_oom.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            sign(p, n, r.next());
            live[slot] = p;
            break;
        }
        case 6:
        case 7:
        case 8: { // soltar
            if (live[slot] == nullptr) break;
            verify(live[slot]);
            util::host_free(live[slot]);
            live[slot] = nullptr;
            break;
        }
        case 9:
        case 10: { // crecer EN EL SITIO
            if (live[slot] == nullptr) break;
            const size_t old = verify(live[slot]);
            if (old == 0) { // ya esta contado; no se puede seguir con el
                live[slot] = nullptr;
                break;
            }
            const size_t want = old + util::kChunkBytes * (1 + r.below(3));
            if (want > kMaxGrow) { // ya crecio bastante: fuera y a empezar
                util::host_free(live[slot]);
                live[slot] = nullptr;
                break;
            }
            void *p = util::host_realloc(live[slot], want);
            if (p == nullptr) {
                g_oom.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            /* Lo que ya habia tiene que seguir ahi: crecer NO puede perder
             * datos, ni cuando absorbe al vecino ni cuando copia. */
            const Mark *m = static_cast<const Mark *>(p);
            if (m->magic != (kMarkMagic ^ m->seed) || size_t(m->bytes) != old)
                g_bad_magic.fetch_add(1, std::memory_order_relaxed);
            sign(p, want, r.next());
            live[slot] = p;
            break;
        }
        case 11: { // dejarlo para que lo suelte otro
            if (live[slot] == nullptr) break;
            SwapSlot &s = g_swap[r.below(kSwapSlots)];
            void *expected = nullptr;
            if (s.p.compare_exchange_strong(expected, live[slot],
                                            std::memory_order_acq_rel,
                                            std::memory_order_relaxed))
                live[slot] = nullptr;
            break;
        }
        case 12: { // recoger lo de otro y soltarlo AQUI
            SwapSlot &s = g_swap[r.below(kSwapSlots)];
            void *p = s.p.exchange(nullptr, std::memory_order_acq_rel);
            if (p == nullptr) break;
            verify(p);
            util::host_free(p);
            break;
        }
        case 13:
        case 14: { // rafaga de pequenas: el OTRO consumidor de trozos nuevos
            void *tmp[8];
            for (int k = 0; k < 8; ++k) {
                const size_t n = kMinBlock + r.below(4096);
                tmp[k] = util::host_alloc(n);
                if (tmp[k] != nullptr) sign(tmp[k], n, r.next());
            }
            for (int k = 0; k < 8; ++k) {
                if (tmp[k] == nullptr) continue;
                verify(tmp[k]);
                util::host_free(tmp[k]);
            }
            break;
        }
        default: { // a cero, oleada, y de tarde en tarde uno enorme
            const uint32_t pick = r.below(64);
            if (pick == 0) {
                /* Por encima del tope que se guarda: al soltarlo devuelve sus
                 * paginas al sistema, que es otra rama distinta -- y la que
                 * antes hacia esa llamada con el cerrojo cogido. */
                const size_t n =
                    size_t(util::kMaxSpanChunks + 1) * util::kChunkBytes - 64;
                void *p = util::host_alloc(n);
                if (p == nullptr) {
                    g_oom.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                sign(p, n, r.next());
                verify(p);
                util::host_free(p);
            } else if (pick < 16) {
                // Oleada: ocho de golpe y todos fuera.  Fuerza trozos nuevos en
                // unos hilos mientras otros fusionan.
                void *tmp[8];
                size_t sz[8];
                for (int k = 0; k < 8; ++k) {
                    sz[k] = span_size(r, 3);
                    tmp[k] = util::host_alloc(sz[k]);
                    if (tmp[k] != nullptr) sign(tmp[k], sz[k], r.next());
                }
                for (int k = 0; k < 8; ++k) {
                    if (tmp[k] == nullptr) continue;
                    verify(tmp[k]);
                    util::host_free(tmp[k]);
                }
            } else {
                const size_t n = span_size(r, 4);
                void *p = util::host_alloc_zeroed(n);
                if (p == nullptr) {
                    g_oom.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                /* Un bloque a cero tiene que venir a cero DE VERDAD, tambien
                 * cuando sale de la lista de libres y no del sistema. */
                const unsigned char *b = static_cast<const unsigned char *>(p);
                if (b[0] != 0 || b[n / 2] != 0 || b[n - 1] != 0)
                    g_bad_mid.fetch_add(1, std::memory_order_relaxed);
                sign(p, n, r.next());
                verify(p);
                util::host_free(p);
            }
            break;
        }
        }
    }

    for (void *p : live) {
        if (p == nullptr) continue;
        verify(p);
        util::host_free(p);
    }
}

/**
 * @brief Que fusionar hacia la derecha sigue funcionando.
 *
 * Un solo hilo, con direcciones a la vista.  Se piden dos tramos de UN trozo,
 * se comprueba que salieron pegados, se sueltan los dos y se pide uno de DOS:
 * si la fusion funciona, el grande tiene que caer en la direccion del primero,
 * porque los dos pequenos se volvieron uno.
 *
 * Sin esta prueba, un mapa de libres que dejara de marcarse no daria ningun
 * error: solo dejaria de fusionar, la memoria creceria y nadie lo notaria.
 */
void check_coalescing() {
    /* POR ENCIMA DEL CACHE POR HILO.  Un tramo de uno o dos trozos se lo queda
     * el propio hilo al soltarlo (`kSpanCacheSlots`) y no llega a las listas
     * compartidas, asi que nunca se fusionaria y esta prueba acusaria al
     * asignador de un fallo que no tiene.  Con trozos de mas se va por el
     * camino que se quiere probar. */
    const uint32_t k = util::kSpanCacheSlots + 1;
    const size_t one = size_t(k) * util::kChunkBytes - 64;
    const size_t two = size_t(2 * k) * util::kChunkBytes - 64;

    void *a = util::host_alloc(one);
    void *b = util::host_alloc(one);
    check(a != nullptr && b != nullptr, "dos tramos por encima del cache");
    if (a == nullptr || b == nullptr) return;

    const uintptr_t ua = reinterpret_cast<uintptr_t>(a);
    const uintptr_t ub = reinterpret_cast<uintptr_t>(b);
    const bool adjacent = (ub == ua + size_t(k) * util::kChunkBytes);
    check_allocator_placed(adjacent,
                           "salen pegados, que es lo que hace posible fusionar");

    util::host_free(b); // el de la derecha primero
    util::host_free(a); // al soltar este se absorbe al de al lado

    /* CON LAS POLITICAS SIN CERROJO NO BASTA CON SOLTARLOS.  Ahi un tramo
     * liberado se aparca donde ningun fusionador lo ve -- que es justo lo que
     * hace que llegar a el no cueste candado --, asi que los dos de arriba
     * siguen separados y la pregunta "se fusionaron?" no tiene respuesta
     * todavia.  Vaciar el aparcadero es lo que los devuelve al fondo comun y
     * los pone en condiciones de fusionarse.
     *
     * Con la politica con cerrojo no hay nada aparcado y esto no hace nada, asi
     * que la comprobacion es la MISMA para todas: no se debilita el test, se le
     * quita una suposicion que solo valia para una de ellas. */
    util::host_span_trim();

    void *big = util::host_alloc(two);
    check(big != nullptr, "y luego uno del doble de tamano");
    if (big == nullptr) return;
    /* Si no se hubiera fusionado, el doble no cabria en el hueco y habria que
     * ir a por region nueva: caeria en otra direccion. */
    check(!adjacent || reinterpret_cast<uintptr_t>(big) == ua,
          "el doble cae donde estaban los dos: SE FUSIONARON");
    util::host_free(big);
}

} // namespace

int main(int argc, char **argv) {
    std::printf("== el camino de tramos con varios hilos ==\n");
    if (!util::host_alloc_active()) {
        std::printf("  el asignador no esta en vigor: nada que probar\n");
        return 0;
    }

    check_coalescing();

    /* Mas hilos que procesadores a proposito: asi alguno se queda desalojado
     * dentro de la seccion critica, que es cuando el fallo aparecia antes. */
    unsigned threads = std::thread::hardware_concurrency();
    if (threads == 0) threads = 4;
    threads *= 2;
    if (threads > 48) threads = 48;
    uint64_t steps = 8000;
    if (argc > 1) threads = unsigned(std::atoi(argv[1]));
    if (argc > 2) steps = uint64_t(std::atoll(argv[2]));

    const util::HostAllocStats before = util::host_alloc_stats();

    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (unsigned i = 0; i < threads; ++i)
        pool.emplace_back(worker, i, steps, uint64_t(0x5EEDu));
    g_go.store(true, std::memory_order_release);
    for (auto &t : pool)
        t.join();

    for (auto &s : g_swap) {
        void *p = s.p.exchange(nullptr, std::memory_order_acq_rel);
        if (p != nullptr) util::host_free(p);
    }

    const util::HostAllocStats after = util::host_alloc_stats();
    const uint64_t did = after.large_allocs - before.large_allocs;

    std::printf("  %u hilos x %llu pasos -> %llu reservas grandes\n", threads,
                (unsigned long long)steps, (unsigned long long)did);

    check(did > 0, "el banco paso de verdad por el camino de tramos");
    check(g_bad_magic.load() == 0, "ninguna firma pisada");
    check(g_bad_mid.load() == 0, "ningun testigo del medio cambiado");
    check(g_bad_tail.load() == 0, "ningun testigo del final cambiado");
    check(g_bad_size.load() == 0, "ningun tamano imposible");
    check(after.large_allocs - after.large_frees ==
              before.large_allocs - before.large_frees,
          "todo tramo reservado acabo devuelto");

    if (g_oom.load() != 0)
        std::printf("  aviso: %llu reservas sin sitio (region llena)\n",
                    (unsigned long long)g_oom.load());

    /* Saltadas APARTE, nunca sumadas a los aciertos. */
    if (skipped != 0)
        std::printf("%d SALTADAS por el nivel del modo de comprobacion\n",
                    skipped);
    std::printf(failures == 0 ? "TODO OK\n" : "%d FALLOS\n", failures);
    return failures == 0 ? 0 : 1;
}
