/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/host_allocator.cpp
 * @brief Implementacion del asignador propio.  Los motivos, en la cabecera.
 */
/* NO se incluye el registro de mandos del proyecto (`util/env_flags.h`), y no
 * es un descuido: guarda el valor de sus mandos en cadenas, asi que la primera
 * consulta PIDE MEMORIA -- y pedir memoria entra aqui.  Con su estatico a medio
 * construir, esa reentrada se queda esperando su guarda para siempre y el
 * proceso cuelga antes de llegar a `main`.  Costo encontrarlo.
 *
 * Por eso las dos variables de entorno que mira este fichero se leen con
 * `getenv` a pelo.  Y de paso es lo que permite que esta libreria no dependa de
 * nada del compilador. */
#include "util/host_allocator.h"

#include "util/os_memory.h"
#include "util/thread_slot.h"

#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <new>

/* Ya no hace falta `windows.h` ni `sys/mman.h`: todo el trato con el sistema
 * pasa por `util/os_memory.h`, que ademas es lo que usaran la arena de golpe y
 * el camino de reservas grandes.  Un solo sitio que hable con el sistema. */

namespace util {

/* La GEOMETRIA -- region, trozos, clases de tamano, cabecera de trozo -- vive
 * en `util/host_allocator_layout.h`, porque no la usa solo esto: tambien la
 * arena de golpe, los metadatos de diagnostico y la capa en C.  Aqui quedan las
 * definiciones de lo que la cabecera declara y todo lo que NO va en linea. */
namespace detail {

std::atomic<uintptr_t> g_region_base{0};
std::atomic<uintptr_t> g_region_end{0};
uint8_t g_class_of[(kMaxSmall / kAlign) + 1];

/// La ranura por hilo con el puntero al cache.  Global normal, no de hilo: lo
/// que cambia por hilo es el CONTENIDO de la ranura.
ThreadSlot g_cache_slot;

bool g_measure = false;

} // namespace detail

using detail::g_cache_slot;
using detail::g_class_of;
using detail::g_measure;
using detail::g_region_base;
using detail::g_region_end;
using detail::ThreadCache;

namespace {

// =========================================================================
//  Parametros
// =========================================================================

/**
 * @brief Topes del reparto de tamanos (solo con VESTA_HOST_ALLOC_STATS=1).
 *
 * Llegan hasta arriba a proposito.  Se cortaban en `>16K`, y ahi caia una CUARTA
 * PARTE de las reservas grandes metida en un solo cajon: con eso no se puede
 * decidir como servirlas.  Si la cola se queda en decenas de KiB lo que sirve
 * son clases por paginas; si llega a megas, lo que sirve es mapear cada bloque
 * por su cuenta, donde la llamada al sistema se diluye en el tamano.  Son
 * diseños distintos y la unica forma de elegir es mirando.
 */
constexpr size_t kBucketLimit[] = {
    64,     256,     1024,      2048,     // lo que servimos por clases
    4096,   8192,    16384,     65536,    // paginas y trozos
    262144, 1048576, 16777216,  ~size_t(0)};
constexpr uint32_t kSizeBuckets =
    sizeof(kBucketLimit) / sizeof(kBucketLimit[0]);

// =========================================================================
//  Estado
// =========================================================================

ThreadCache g_caches[kMaxThreads];
std::atomic<uint32_t> g_next_id{0};

/**
 * @brief El ultimo cache es COMPARTIDO, no de un hilo.
 *
 * Lo usan los hilos que se quedan sin identificador propio.  Antes esos hilos
 * se salian del asignador y se iban a `malloc` PARA SIEMPRE -- y como los
 * identificadores no se reciclan, bastaba con que hubieran existido sesenta y
 * cuatro hilos a lo largo de la vida del proceso, vivos o no.
 *
 * Compartido significa con cerrojo, y por eso solo se llega aqui por el camino
 * lento: el rapido sigue sin sincronizar nada.  Un hilo desbordado va mas
 * despacio; antes se salia del asignador entero, que es mucho peor.
 *
 * Tiene id como los demas, asi que la cabecera de trozo, las liberaciones
 * cruzadas y las estadisticas funcionan sin ningun caso especial.
 */
constexpr uint32_t kSharedCacheId = kMaxThreads - 1;
std::atomic_flag g_shared_lock = ATOMIC_FLAG_INIT;

struct SharedLock {
    SharedLock() noexcept {
        while (g_shared_lock.test_and_set(std::memory_order_acquire)) {
        }
    }
    ~SharedLock() { g_shared_lock.clear(std::memory_order_release); }
};

/**
 * @brief Lo que otros hilos han soltado y su dueno todavia no ha recogido.
 *
 * Una pila atomica por (dueno, clase).  Quien libera algo ajeno empuja aqui con
 * un `compare_exchange` -- sin bloquear a nadie -- y el dueno se lleva la pila
 * ENTERA de un golpe cuando se queda sin bloques.  Es lo que hace que liberar
 * entre hilos no cueste ni fugue.
 */
std::atomic<void *> g_remote[kMaxThreads][kClasses];

std::atomic<size_t> g_chunk_next{0};

/// Cuanto se consiguio apalabrar de verdad, que no tiene por que ser lo pedido.
std::atomic<size_t> g_region_reserved{0};

/**
 * @brief Reservas que tuvimos que ceder al sistema por no poder servirlas.
 *
 * Separado de `large_allocs` a proposito: una reserva grande va al sistema por
 * DISENO, y esto es lo contrario -- nos rendimos.  Mientras no fuera un
 * contador aparte, agotar la region o pasarse de hilos degradaba en silencio y
 * lo unico que se notaba era que iba mas lento.
 */
std::atomic<uint64_t> g_gave_up{0};

/// Tramos tan grandes que no merece la pena guardarlos.  Sus paginas si vuelven
/// al sistema; lo que se pierde es el rango de direcciones.  Se cuenta para que
/// deje de ser raro con aviso, y no en silencio.
std::atomic<uint64_t> g_spans_dropped{0};

/// Liberaciones de algo que no reconocemos: puntero malo, doble liberacion o
/// memoria pisada.  Nunca deberia subir de cero.
std::atomic<uint64_t> g_corrupt_frees{0};
/// 0 sin tocar, 1 montandose, 2 lista, 3 no se pudo.
std::atomic<int> g_region_state{0};

/// 0 sin mirar, 1 activo, 2 apagado por entorno.
std::atomic<int> g_active_state{0};

/**
 * @brief Vuelca el reparto al terminar, si se pidio.
 *
 * Sin destructor no hay donde imprimirlo: los contadores viven en memoria
 * estatica que nadie recorre.  No esta en ningun camino caliente.
 */
struct StatsDump {
    ~StatsDump();
};
StatsDump g_stats_dump;

// =========================================================================
//  Utilidades
// =========================================================================

/// Llena la tabla de tamano -> clase.  La consulta esta en la cabecera, en
/// linea; construirla es cosa de una vez y por eso se queda aqui.
void build_class_table() noexcept {
    for (size_t step = 0; step <= kMaxSmall / kAlign; ++step) {
        const size_t want = step * kAlign;
        uint8_t k = 0;
        while (k + 1 < kClasses && kSizes[k] < want)
            ++k;
        g_class_of[step] = k;
    }
}

bool allocator_active() noexcept {
    const int s = g_active_state.load(std::memory_order_acquire);
    if (s != 0) return s == 1;
    /* Se deja APAGADO antes de preguntar al entorno: si `getenv` pidiera
     * memoria por dentro, esa peticion entraria aqui otra vez y se quedaria
     * dando vueltas.  Asi lo peor que pasa es que la primera reserva vaya al
     * sistema. */
    g_active_state.store(2, std::memory_order_release);
    /* A PELO, y no por el registro de mandos (`util/env_flags.h`), aunque los
     * dos esten declarados alli.
     *
     * El registro guarda el valor de los 167 mandos en cadenas, asi que
     * consultarlo la primera vez PIDE MEMORIA -- y pedir memoria entra aqui.
     * Con el estatico del registro a medio construir, esa reentrada se queda
     * esperando su guarda para siempre: el proceso cuelga antes de llegar a
     * `main`.  Costo encontrarlo.
     *
     * Es el mismo motivo que ya decia el comentario de arriba sobre `getenv`,
     * llevado un paso mas: quien resuelve las reservas no puede apoyarse en
     * nada que reserve. */
    const char *no_slab = std::getenv("VESTA_NO_HOST_SLAB");
    const bool off = no_slab != nullptr && no_slab[0] != '\0' &&
                     !(no_slab[0] == '0' && no_slab[1] == '\0');
    const char *stats = std::getenv("VESTA_HOST_ALLOC_STATS");
    g_measure = stats != nullptr && stats[0] != '\0' &&
                !(stats[0] == '0' && stats[1] == '\0');
    if (!off) {
        build_class_table();
        g_active_state.store(1, std::memory_order_release);
    }
    return !off;
}

/// Reserva (que no gasta) la region de la que salen todos los trozos.
bool ensure_region() noexcept {
    int s = g_region_state.load(std::memory_order_acquire);
    if (s == 2) return true;
    if (s == 3) return false;
    int expected = 0;
    if (!g_region_state.compare_exchange_strong(expected, 1,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
        // Otro lo esta montando: se espera a que acabe.
        while ((s = g_region_state.load(std::memory_order_acquire)) == 1) {
        }
        return s == 2;
    }
    /* Apalabrar sin gastar, que es lo contrario de lo que hace
     * `vm::allocate_memory` (esa entrega la memoria ya comprometida).  De esta
     * reserva sale la propiedad que hace barato liberar -- saber si un puntero
     * es nuestro son dos comparaciones --, asi que no es un detalle cedible.
     *
     * El trato con el sistema esta en `util/os_memory.h`, que es el UNICO sitio
     * del proyecto que lo tiene, y esta escrito para poder llamarse desde aqui:
     * no reserva, no imprime y no lanza.  Antes esto hablaba con
     * `VirtualAlloc`/`mmap` por su cuenta. */
    /* Se PIDE un maximo y se acepta lo que haya, en vez de escribir un numero y
     * rendirse si no entra.  Cuanto se puede apalabrar depende de la maquina,
     * del sistema y de lo que ya haya hecho el proceso, asi que una constante
     * seria una suposicion -- y una suposicion de mas aqui degrada en silencio:
     * el asignador entero se apaga y todo cae al sistema sin que se entere
     * nadie.  Bajando a la mitad, en una maquina apretada se consigue MENOS
     * pero se consigue. */
    size_t reserved = 0;
    void *base = os_reserve_largest(kRegionBytes, kRegionMinBytes, &reserved);
    if (base == nullptr) {
        g_region_state.store(3, std::memory_order_release);
        return false;
    }
    g_region_reserved.store(reserved, std::memory_order_relaxed);
    /* Los trozos se localizan enmascarando el puntero, asi que la base tiene
     * que estar alineada al tamano de trozo.  Windows ya reserva alineado a
     * 64 KiB (`os_reserve_granularity`); fuera de Windows se redondea hacia
     * arriba y se pierde como mucho un trozo. */
    uintptr_t b = reinterpret_cast<uintptr_t>(base);
    const uintptr_t aligned =
        (b + kChunkBytes - 1) & ~(uintptr_t)(kChunkBytes - 1);
    g_region_base.store(aligned, std::memory_order_relaxed);
    g_region_end.store(b + reserved, std::memory_order_relaxed);
    g_region_state.store(2, std::memory_order_release);
    return true;
}

/**
 * @brief El cache de ESTE hilo; lo da de alta la primera vez.
 *
 * Solo se llama desde los caminos LENTOS.  El rapido usa
 * @c detail::current_cache, que se limita a leer la ranura: dar de alta ocurre
 * una vez por hilo y no tiene por que estar en linea en cada reserva.
 *
 * Aqui se sostiene el invariante que documenta la cabecera -- si un hilo tiene
 * cache, el asignador esta activo y la tabla de clases construida --: quien
 * llega hasta aqui ya paso por @c allocator_active, que es quien la construye.
 */
ThreadCache *cache() noexcept {
    if (!g_cache_slot.ensure()) return nullptr;
    ThreadCache *c = static_cast<ThreadCache *>(g_cache_slot.get());
    if (c != nullptr) return c;
    const uint32_t id = g_next_id.fetch_add(1, std::memory_order_acq_rel);
    /* Sin listas propias.  Devolver nulo NO significa "al sistema": significa
     * "por el camino lento", que es donde estan las listas COMPARTIDAS.  Ver
     * @c alloc_shared.
     *
     * Y ojo con el contador: solo sube, no se recicla al morir un hilo, asi que
     * el tope no es de hilos VIVOS sino de hilos que hayan existido alguna vez.
     * Reciclarlos exige un aviso de fin de hilo, y la ranura rapida (`TlsAlloc`)
     * no lo da.  Mientras tanto, pasarse cuesta un cerrojo -- no cuesta salirse
     * del asignador, que es lo que costaba antes. */
    if (id >= kSharedCacheId) return nullptr;
    c = &g_caches[id];
    c->id = id;
    c->used = true;
    g_cache_slot.set(c);
    return c;
}

// =========================================================================
//  Tramos: las reservas grandes, servidas por nosotros
// =========================================================================
//
// Un tramo son N trozos seguidos que forman UNA reserva.  Medido, el 90% de las
// grandes son de 64 KiB o menos y la cola llega a 16 MiB (ver
// `doc/PLAN_RESERVAS.md`), asi que no compensa ni tratarlas como clases -- la
// fragmentacion se come la ganancia -- ni pedir cada una al sistema, que seria
// una llamada por reserva.
//
// POR QUE UN CERROJO AQUI Y NO EN EL RESTO.  Porque no cuesta: en una
// compilacion entera hay 564 reservas grandes frente a 62 millones de pequenas,
// o sea una de cada cien mil.  Una pila atomica exigiria resolver el problema
// del ABA -- varios hilos sacando a la vez -- y eso son mas instrucciones y mas
// formas de equivocarse por un ahorro que no se puede ni medir.  El camino
// rapido de verdad, el de las pequenas, no toca esto ni de lejos.
std::atomic_flag g_span_lock = ATOMIC_FLAG_INIT;

/**
 * @brief Cuanta memoria de tramos LIBRES se conserva sin devolver al sistema.
 *
 * POR QUE UN PRESUPUESTO Y NO UN UMBRAL POR TAMANO.  Porque lo segundo se probo
 * y es carisimo.  Habia aqui un tope por tramo -- por encima de 1 MiB se
 * devolvian sus paginas al soltarlo --, y eso convierte cada par
 * reservar/liberar de un bloque grande en tres llamadas al sistema.  Medido
 * contra `malloc` en Linux, por operacion:
 *
 *     reservar y soltar 1 MiB      nuestro 1.438 ns    malloc 9,6 ns
 *     lo mismo con vida y relevo   nuestro 1.959 ns    malloc 17,4 ns
 *
 * Cien veces peor, y por una constante que no estaba midiendo nada.  Con el
 * presupuesto: se conserva lo soltado mientras quepa, y solo se devuelve al
 * sistema lo que se pase.  Asi el caso normal -- soltar y volver a pedir el
 * mismo tamano, que es lo que hacen las fases -- no toca el sistema NI UNA vez,
 * y la memoria retenida sigue acotada.
 *
 * 64 MiB es de AFINADO, no de capacidad: pasarse no rompe nada, solo hace que
 * a partir de ahi los tramos que se suelten devuelvan sus paginas.
 */
/**
 * @brief Marca en `ChunkHeader::_pad` de un tramo que esta LIBRE.
 *
 * Hace falta para poder mirar si el vecino de al lado se puede absorber, que es
 * de lo que va todo lo de abajo.
 */
constexpr uint32_t kSpanFree = 0x46524545u; // 'FREE'

/**
 * @brief Enlaces de la lista de tramos libres.
 *
 * Viven DENTRO del propio tramo, justo detras de su cabecera: mientras esta
 * libre nadie usa esos bytes, asi que la lista no gasta memoria aparte.
 *
 * Doblemente enlazada, y no es un capricho: al absorber un vecino hay que
 * sacarlo de SU lista, y con una lista simple eso seria recorrerla entera.
 */
struct SpanNode {
    SpanNode *prev;
    SpanNode *next;
};

/**
 * @brief Listas de tramos libres, por numero de trozos.
 *
 * ANTES ERAN DE AJUSTE EXACTO Y SIN PARTIR NI JUNTAR, y eso tenia un modo de
 * fallo que solo se ve con un banco: un tramo de diecisiete trozos no puede
 * servir una peticion de uno, asi que un bufer que crece -- reservar, duplicar,
 * duplicar -- dejaba en la lista de 17 lo que la siguiente vuelta pedia de 1, y
 * tenia que ir a por region NUEVA cada vez.  Medido: crecer hasta 1 MiB costaba
 * 3.583 ns por paso y el pico de memoria subia de 21 a 144 MiB.
 *
 * Con partir y juntar:
 *
 *   - al reservar, si no hay del tamano exacto se coge uno MAYOR y se parte; el
 *     resto vuelve a su lista;
 *   - al soltar, si el tramo de al lado tambien esta libre se ABSORBE, de modo
 *     que los trozos vuelven a estar juntos y sirven para lo que venga.
 *
 * Y de ahi sale gratis lo que de verdad importaba: un `realloc` que crece puede
 * tragarse al vecino libre y NO COPIAR NADA.  Es lo que hace `mremap` en
 * `glibc`, que aqui no se puede usar porque moveria el bloque fuera de nuestra
 * region.
 */
SpanNode *g_span_free[kMaxSpanChunks + 1];

struct SpanLock {
    SpanLock() noexcept {
        while (g_span_lock.test_and_set(std::memory_order_acquire)) {
        }
    }
    ~SpanLock() { g_span_lock.clear(std::memory_order_release); }
};

// -------------------------------------------------------------------------
//  Listas de tramos libres.  TODO lo de aqui exige tener el cerrojo.
// -------------------------------------------------------------------------

/// Los enlaces de un tramo libre viven detras de su cabecera.
inline SpanNode *node_of(ChunkHeader *h) noexcept {
    return reinterpret_cast<SpanNode *>(reinterpret_cast<char *>(h) +
                                        sizeof(ChunkHeader));
}
inline ChunkHeader *header_of(SpanNode *n) noexcept {
    return reinterpret_cast<ChunkHeader *>(reinterpret_cast<char *>(n) -
                                           sizeof(ChunkHeader));
}

/// Si @p h es un tramo nuestro que ahora mismo esta libre.
inline bool is_free_span(const ChunkHeader *h) noexcept {
    return h->magic == kSpanMagic && h->_pad == kSpanFree;
}

void list_insert(ChunkHeader *h) noexcept {
    const uint32_t k = h->cls;
    SpanNode *n = node_of(h);
    n->prev = nullptr;
    n->next = g_span_free[k];
    if (n->next != nullptr) n->next->prev = n;
    g_span_free[k] = n;
    h->_pad = kSpanFree;
}

void list_remove(ChunkHeader *h) noexcept {
    SpanNode *n = node_of(h);
    if (n->prev != nullptr)
        n->prev->next = n->next;
    else
        g_span_free[h->cls] = n->next;
    if (n->next != nullptr) n->next->prev = n->prev;
    h->_pad = 0;
}

/// El vecino de la derecha, si esta DENTRO de lo ya repartido.  nullptr si el
/// tramo termina donde acaba lo repartido, o si se sale de la region.
ChunkHeader *right_neighbour(ChunkHeader *h) noexcept {
    const uintptr_t base = g_region_base.load(std::memory_order_relaxed);
    const uintptr_t addr = reinterpret_cast<uintptr_t>(h);
    const uintptr_t next = addr + size_t(h->cls) * kChunkBytes;
    const uintptr_t handed =
        base + g_chunk_next.load(std::memory_order_relaxed) * kChunkBytes;
    if (next >= handed) return nullptr;
    return reinterpret_cast<ChunkHeader *>(next);
}

/**
 * @brief Absorbe vecinos libres a la derecha mientras los haya.
 *
 * Solo hacia la derecha: hacerlo tambien hacia la izquierda exigiria un pie de
 * pagina en cada tramo para poder retroceder, y el caso que importa -- un bufer
 * que crece -- avanza siempre hacia delante.
 */
void coalesce_right(ChunkHeader *h) noexcept {
    for (;;) {
        ChunkHeader *r = right_neighbour(h);
        if (r == nullptr || !is_free_span(r)) return;
        if (size_t(h->cls) + r->cls > kMaxSpanChunks) return;
        list_remove(r);
        h->cls += r->cls;
        r->magic = 0; // deja de ser una cabecera: ahora es parte de `h`
    }
}

/// Parte @p h dejandole @p want trozos y devolviendo el resto a su lista.
void split_span(ChunkHeader *h, uint32_t want) noexcept {
    const uint32_t rest = h->cls - want;
    if (rest == 0) return;
    ChunkHeader *r = reinterpret_cast<ChunkHeader *>(
        reinterpret_cast<char *>(h) + size_t(want) * kChunkBytes);
    r->magic = kSpanMagic;
    r->cls = rest;
    r->owner = h->owner;
    h->cls = want;
    list_insert(r);
}

/// Saca de las listas un tramo de al menos @p want trozos, partiendolo si hace
/// falta.  nullptr si no hay ninguno.
ChunkHeader *take_from_free_lists(uint32_t want) noexcept {
    for (uint32_t k = want; k <= kMaxSpanChunks; ++k) {
        SpanNode *n = g_span_free[k];
        if (n == nullptr) continue;
        ChunkHeader *h = header_of(n);
        list_remove(h);
        split_span(h, want);
        return h;
    }
    return nullptr;
}

/// Trozos ya entregados por la region y nunca devueltos, para no repartir dos
/// veces el mismo.  Es el mismo contador que usa `grow`.
/**
 * @brief Estira un tramo SIN moverlo, si se puede.
 * @return true si se consiguio; entonces @p h ya cubre @p want trozos.
 *
 * Se intenta por dos vias, y hay que tener el cerrojo de tramos:
 *
 *  1. **Absorbiendo al vecino de la derecha si esta libre.**  Es la que
 *     importa: un bufer que crece suelta su version anterior justo delante de
 *     donde va a crecer, asi que el vecino suele ser suyo.
 *  2. **Tomando mas region**, si el tramo termina justo donde va el reparto.
 *     Vale para el primer crecimiento, cuando aun no hay vecino que absorber.
 *
 * POR QUE NO `mremap`, que es lo que usa glibc para no copiar: moveria el
 * bloque FUERA de nuestra region, y con eso `in_region` dejaria de
 * reconocerlo.  Ese reconocimiento de dos comparaciones es de donde sale que
 * liberar sea barato, asi que no es negociable.
 */
bool try_extend_span(ChunkHeader *h, uint32_t want) noexcept {
    // 1. Tragarse vecinos libres mientras no baste.
    while (h->cls < want) {
        ChunkHeader *r = right_neighbour(h);
        if (r == nullptr || !is_free_span(r)) break;
        list_remove(r);
        h->cls += r->cls;
        r->magic = 0;
    }
    if (h->cls >= want) {
        // Puede haber sobrado: lo que sobre vuelve a la lista.
        split_span(h, want);
        return true;
    }

    // 2. Si estamos al final de lo repartido, se toma mas region.
    const uintptr_t addr = reinterpret_cast<uintptr_t>(h);
    const uintptr_t base = g_region_base.load(std::memory_order_relaxed);
    const size_t idx = (addr - base) / kChunkBytes;
    size_t expected = idx + h->cls;
    if (!g_chunk_next.compare_exchange_strong(expected, idx + want,
                                              std::memory_order_acq_rel,
                                              std::memory_order_relaxed))
        return false; // hay algo detras: no queda mas que copiar

    if (addr + size_t(want) * kChunkBytes >
        g_region_end.load(std::memory_order_relaxed))
        return false;
    const uintptr_t fresh_addr = addr + size_t(h->cls) * kChunkBytes;
    if (!os_commit(reinterpret_cast<void *>(fresh_addr),
                   size_t(want - h->cls) * kChunkBytes))
        return false;
    h->cls = want;
    return true;
}

uintptr_t take_chunks(size_t count) noexcept {
    if (!ensure_region()) return 0;
    const size_t idx = g_chunk_next.fetch_add(count, std::memory_order_acq_rel);
    const uintptr_t base = g_region_base.load(std::memory_order_relaxed);
    const uintptr_t addr = base + idx * kChunkBytes;
    if (addr + count * kChunkBytes >
        g_region_end.load(std::memory_order_relaxed))
        return 0; // region llena
    return addr;
}

/**
 * @brief Sirve una reserva grande.  nullptr si la region no da mas.
 * @param fresh Si no es nulo, sale `true` cuando el tramo viene RECIEN del
 *        sistema y por tanto esta a cero.  Lo usa @c host_alloc_zeroed para
 *        saltarse un `memset` que no hace falta.
 *
 * Un tramo recien comprometido esta garantizado a cero, tanto en Windows como
 * en POSIX: entregar paginas de otro proceso sin limpiarlas seria una fuga de
 * datos, asi que el sistema no lo hace.  Uno REUSADO no lo esta, aunque se le
 * hayan devuelto las paginas: su primera pagina se conserva -- ahi viven la
 * cabecera y el enlace de la lista -- y lleva lo que hubiera.
 */
void *alloc_span(ThreadCache *c, size_t n, bool *fresh = nullptr) noexcept {
    if (fresh != nullptr) *fresh = false;
    const uint32_t chunks = chunks_for(n);
    const size_t bytes = size_t(chunks) * kChunkBytes;
    uintptr_t addr = 0;
    bool reused = false;

    bool needs_commit = false;
    if (chunks <= kMaxSpanChunks) {
        SpanLock lk;
        /* Del tamano exacto si lo hay, y si no de uno mayor PARTIENDOLO.  Sin
         * esto, un tramo grande libre no puede servir una peticion pequena y
         * hay que ir a por region nueva; ver la nota de `g_span_free`. */
        ChunkHeader *h = take_from_free_lists(chunks);
        if (h != nullptr) {
            addr = reinterpret_cast<uintptr_t>(h);
            reused = true;
        }
    }
    if (addr == 0) {
        addr = take_chunks(chunks);
        if (addr == 0) return nullptr;
        needs_commit = true;
        // Nunca usado: lo que entregue el sistema viene a cero.
        if (fresh != nullptr) *fresh = true;
    }
    if (needs_commit &&
        !os_commit(reinterpret_cast<void *>(addr), bytes))
        return nullptr;

    ChunkHeader *h = reinterpret_cast<ChunkHeader *>(addr);
    h->magic = kSpanMagic;
    h->cls = chunks;  // en un tramo, `cls` guarda CUANTOS trozos ocupa
    h->owner = c != nullptr ? c->id : 0;
    h->_pad = 0;
    if (c != nullptr) {
        c->stats.large_allocs++;
        // Solo se apunta lo que se pide POR PRIMERA VEZ; reusar no compromete
        // memoria nueva y contarlo otra vez inflaria el total.
        if (!reused) c->stats.bytes_reserved += bytes;
    }
    return reinterpret_cast<void *>(addr + sizeof(ChunkHeader));
}

/**
 * @brief Devuelve un tramo.  @p h es su cabecera, la del primer trozo.
 *
 * @par Hilos
 * Segura.  Toma el cerrojo de tramos para tocar las listas de libres.
 */
void free_span(ChunkHeader *h) noexcept {
    ThreadCache *c = cache();
    if (c != nullptr) c->stats.large_frees++;

    SpanLock lk;
    /* Se absorbe a los vecinos libres de la derecha ANTES de entrar en ninguna
     * lista.  Asi los trozos vuelven a estar juntos y sirven para lo que venga
     * despues, en vez de quedarse atrapados en la lista de su tamano exacto.
     *
     * UN TRAMO QUE SE GUARDA, SE GUARDA CON SU MEMORIA.  Aqui hubo dos intentos
     * de devolver paginas al soltarlo y los dos se midieron y se retiraron: por
     * tamano, convierte cada par reservar/soltar en tres llamadas al sistema
     * (1.438 ns por operacion frente a 9,6 de `malloc`); por presupuesto, peor
     * todavia y mas dificil de ver -- una fase que suelta 512 tramos de 1 MiB
     * deja sesenta y cuatro pinchados, nadie vuelve a pedir ese tamano, y a
     * partir de ahi CUALQUIER liberacion ve el presupuesto lleno.
     *
     * Lo retenido esta acotado por el pico de tramos libres a la vez, que es
     * memoria que el programa ya llego a tener.  Devolverla, si hace falta,
     * es cosa de una llamada explicita entre fases. */
    h->_pad = 0;
    coalesce_right(h);

    if (h->cls > kMaxSpanChunks) {
        /* Demasiado grande para guardarlo.  Sus paginas SI vuelven al sistema
         * -- quedarse con el rango y ademas con la memoria seria regalar las
         * dos cosas --, y se cuenta, para que no sea mudo si deja de ser raro. */
        const size_t bytes = size_t(h->cls) * kChunkBytes;
        const size_t page = os_page_size();
        if (bytes > page)
            os_decommit(reinterpret_cast<char *>(h) + page, bytes - page);
        h->magic = 0;
        g_spans_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    list_insert(h);
}

/// Recoge de un golpe lo que otros hilos soltaron de esta clase.
void *take_remote(ThreadCache *c, uint32_t k) noexcept {
    void *head =
        g_remote[c->id][k].exchange(nullptr, std::memory_order_acq_rel);
    return head;
}

/// Pide un trozo nuevo y lo parte en bloques de la clase @p k.
void *grow(ThreadCache *c, uint32_t k) noexcept {
    if (!ensure_region()) return nullptr;
    const size_t idx = g_chunk_next.fetch_add(1, std::memory_order_acq_rel);
    const uintptr_t base = g_region_base.load(std::memory_order_relaxed);
    const uintptr_t addr = base + idx * kChunkBytes;
    if (addr + kChunkBytes > g_region_end.load(std::memory_order_relaxed))
        return nullptr; // region agotada: se sigue con el sistema
    if (!os_commit(reinterpret_cast<void *>(addr), kChunkBytes)) return nullptr;
    ChunkHeader *h = reinterpret_cast<ChunkHeader *>(addr);
    h->magic = kChunkMagic;
    h->cls = k;
    h->owner = c->id;
    h->_pad = 0;

    const size_t slot = kSizes[k];
    const size_t count = (kChunkBytes - sizeof(ChunkHeader)) / slot;
    uintptr_t first = addr + sizeof(ChunkHeader);
    // Se encadenan al reves para que el primero que se entregue sea el de
    // menor direccion: recorre la memoria hacia delante, que es lo que le
    // gusta al prefetcher.
    void *head = nullptr;
    for (size_t i = count; i-- > 0;) {
        void *s = reinterpret_cast<void *>(first + i * slot);
        *reinterpret_cast<void **>(s) = head;
        head = s;
    }
    c->stats.chunks++;
    c->stats.bytes_reserved += kChunkBytes;
    return head;
}

} // namespace

// =========================================================================
//  Caminos lentos
// =========================================================================
//
// El rapido esta en la cabecera y en linea; aqui queda lo que ocurre pocas
// veces.  El reparto no es estetico: ver la nota de `util/host_allocator.h`.

namespace detail {

void record_size(ThreadCache *c, size_t n) noexcept {
    // Reparto de tamanos pedidos.  Sirve para UNA pregunta concreta: si lo que
    // se pide cae fuera de las clases, subir el tope da mas de lo que cuesta;
    // si no, no.
    uint32_t b = 0;
    while (b + 1 < kSizeBuckets && n > kBucketLimit[b])
        ++b;
    c->stats.size_hist[b]++;
}

ThreadCache *ensure_cache() noexcept {
    if (!allocator_active()) return nullptr;
    return cache();
}

void *host_alloc_refill(ThreadCache *c, uint32_t k, size_t n) noexcept {
    void *chain = take_remote(c, k);
    if (chain == nullptr) chain = grow(c, k);
    if (chain == nullptr) {
        // Region agotada.  Se sigue con el sistema, pero SE CUENTA: esto es
        // rendirse, no una decision de diseno, y hasta ahora era mudo.
        g_gave_up.fetch_add(1, std::memory_order_relaxed);
        return std::malloc(n);
    }
    /* Lo recogido es una cadena entera: se cuelga de la lista y se entrega el
     * primero por la via de siempre, para que el reparto por etiqueta se lleve
     * en UN solo sitio. */
    c->free_list[k] = chain;
    return pop_block(c, k);
}

/**
 * @brief Sirve a un hilo que se quedo sin listas propias, con cerrojo.
 *
 * Es lo que hace que quedarse sin identificador cueste UN CERROJO en vez de
 * salirse del asignador.  Todo lo que sale de aqui lleva el id compartido en la
 * cabecera de su trozo, asi que liberarlo -- desde este hilo o desde otro --
 * funciona por el camino de siempre, sin ningun caso especial: el que libera ve
 * que el bloque no es suyo y lo empuja a la pila del dueno, que es justamente
 * el cache compartido.
 */
void *alloc_shared(size_t n) noexcept {
    ThreadCache *c = &g_caches[kSharedCacheId];
    SharedLock lk;
    if (!c->used) {
        c->id = kSharedCacheId;
        c->used = true;
    }
    if (g_measure) record_size(c, n);
    if (n > kMaxSmall) {
        void *p = alloc_span(c, n);
        if (p != nullptr) return p;
        g_gave_up.fetch_add(1, std::memory_order_relaxed);
        return std::malloc(n);
    }
    const uint32_t k = class_of(n);
    void *p = pop_block(c, k);
    if (p != nullptr) return p;
    return host_alloc_refill(c, k, n);
}

/* Cubre a la vez el alta del hilo, el asignador apagado, la reserva vacia, la
 * reserva grande y el desbordamiento de hilos.  Son casos distintos pero todos
 * raros, y juntarlos deja el camino rapido con una sola comparacion para los
 * cinco. */
void *host_alloc_slow(size_t n) noexcept {
    if (!allocator_active()) return std::malloc(n);
    if (n == 0) n = 1;
    ThreadCache *c = cache();
    if (c != nullptr && g_measure) record_size(c, n);
    if (n > kMaxSmall) {
        /* Grande: la sirve un TRAMO de trozos de la region.  Ya no se le pide
         * al asignador del sistema; ver `doc/PLAN_RESERVAS.md`. */
        void *p = alloc_span(c, n);
        if (p != nullptr) return p;
        /* La region no dio.  Se cede al sistema, pero SE CUENTA: esto es
         * rendirse, no una decision de diseno. */
        g_gave_up.fetch_add(1, std::memory_order_relaxed);
        return std::malloc(n);
    }
    if (c == nullptr) return alloc_shared(n);
    const uint32_t k = class_of(n);
    void *p = pop_block(c, k);
    if (p == nullptr) return host_alloc_refill(c, k, n);
    return p;
}

void host_free_not_small(void *p, ChunkHeader *h) noexcept {
    if (h->magic == kSpanMagic) {
        free_span(h);
        return;
    }
    /* Dentro de la region y sin marca conocida.  Esto NO es un caso previsto:
     * es memoria pisada, un puntero al medio de un bloque, o una doble
     * liberacion.  Antes se volvia en silencio, que es la peor opcion posible
     * -- el programa sigue como si nada con la estructura ya rota --.  Se
     * cuenta y se avisa una vez; no se aborta, porque quien libera puede estar
     * en mitad de una salida y matar el proceso ahi no ayuda a nadie. */
    static std::atomic<bool> ya_avisado{false};
    g_corrupt_frees.fetch_add(1, std::memory_order_relaxed);
    if (!ya_avisado.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr,
                     "[allocator] CORRUPTION: free() of %p, whose chunk header "
                     "at %p has magic 0x%08x (expected 0x%08x or 0x%08x). "
                     "Bad pointer, double free, or overwritten memory.\n",
                     p, (void *)h, h->magic, kChunkMagic, kSpanMagic);
}

void host_free_remote(void *p, ChunkHeader *h) noexcept {
    // De otro hilo: a su pila, sin bloquear a nadie.
    std::atomic<void *> &head = g_remote[h->owner][h->cls];
    void *old = head.load(std::memory_order_relaxed);
    do {
        *reinterpret_cast<void **>(p) = old;
    } while (!head.compare_exchange_weak(old, p, std::memory_order_release,
                                         std::memory_order_relaxed));
    /* Se da de alta el hilo aunque solo libere: si no, un hilo que unicamente
     * suelta cosas ajenas no contaria ni una y el reparto mentiria por omision.
     */
    ThreadCache *c = cache();
    if (c != nullptr) c->stats.remote_frees++;
}

} // namespace detail

// =========================================================================
//  Interfaz
// =========================================================================

HostAllocStats host_alloc_stats() {
    HostAllocStats t;
    for (uint32_t i = 0; i < kMaxThreads; ++i) {
        if (!g_caches[i].used) continue;
        const HostAllocStats &s = g_caches[i].stats;
        for (uint32_t g = 0; g < AllocTag::kSlots; ++g) {
            t.by_tag[g] += s.by_tag[g];
            t.small_allocs += s.by_tag[g]; // el total ES la suma del reparto
        }
        t.small_frees += s.small_frees;
        t.remote_frees += s.remote_frees;
        t.large_allocs += s.large_allocs;
        t.large_frees += s.large_frees;
        t.chunks += s.chunks;
        t.bytes_reserved += s.bytes_reserved;
        for (uint32_t b = 0; b < kSizeBuckets; ++b)
            t.size_hist[b] += s.size_hist[b];
    }
    return t;
}

bool host_alloc_active() {
    return allocator_active();
}

/* Un almacen propio se pide igual que el de un hilo -- mismo array, mismo
 * contador --, y por eso todo lo demas funciona sin ningun caso especial: su id
 * va en la cabecera de cada trozo, asi que las liberaciones desde otro hilo y
 * las estadisticas lo tratan como a cualquier otro. */
SingleOwnerAllocator::SingleOwnerAllocator() noexcept : cache_(nullptr) {
    if (!allocator_active()) return;
    const uint32_t id = g_next_id.fetch_add(1, std::memory_order_acq_rel);
    if (id >= kSharedCacheId) return; // sin almacen: se ira por el general
    ThreadCache *c = &g_caches[id];
    c->id = id;
    c->used = true;
    cache_ = c;
}

HostAllocStats SingleOwnerAllocator::stats() const noexcept {
    /* Sin almacen propio no hay nada suyo que contar: todo a cero, que es la
     * respuesta correcta, no una inventada. */
    if (cache_ == nullptr) return HostAllocStats{};
    HostAllocStats s = cache_->stats;
    // El total no se lleva en el camino caliente: ES la suma del reparto.
    for (uint32_t g = 0; g < AllocTag::kSlots; ++g)
        s.small_allocs += s.by_tag[g];
    return s;
}

size_t host_usable_size(const void *p) noexcept {
    if (p == nullptr || !in_region(p)) return 0;
    const ChunkHeader *h = chunk_of(const_cast<void *>(p));
    if (h->magic == kChunkMagic) return kSizes[h->cls];
    if (h->magic == kSpanMagic)
        return size_t(h->cls) * kChunkBytes - sizeof(ChunkHeader);
    return 0; // marca desconocida: no inventamos un tamano
}

void *host_realloc(void *p, size_t n) noexcept {
    if (p == nullptr) return host_alloc(n);
    if (n == 0) {
        host_free(p);
        return nullptr;
    }
    if (!in_region(p)) {
        /* No es nuestro: vino del sistema y tiene que volver al sistema.
         * Mezclar los dos asignadores seria pasarle a `free` un puntero que no
         * reconoce. */
        return std::realloc(p, n);
    }

    const size_t old = host_usable_size(p);
    if (old >= n) return p; // ya cabe; el redondeo a clase juega a favor

    /* Estirar en su sitio si es un tramo y esta al final de lo repartido.  Es
     * lo que evita la copia en un bufer que crece; ver la cabecera. */
    ChunkHeader *h = chunk_of(p);
    if (h->magic == kSpanMagic) {
        const uint32_t want = chunks_for(n);
        if (want > h->cls) {
            // CON EL CERROJO: estirar toca las listas de libres al absorber al
            // vecino, y sin el dos hilos creciendo a la vez las corromperian.
            SpanLock lk;
            if (try_extend_span(h, want)) return p;
        }
    }

    void *fresh = host_alloc(n);
    if (fresh == nullptr) return nullptr; // `p` sigue valido, como manda
    std::memcpy(fresh, p, old < n ? old : n);
    host_free(p);
    return fresh;
}

void *host_alloc_zeroed(size_t n) noexcept {
    /* Solo los TRAMOS pueden venir limpios del sistema, y solo si son nuevos.
     * Todo lo demas sale de una lista de libres con lo que dejara el inquilino
     * anterior, asi que hay que limpiarlo.  El motivo de que esto no sea
     * `host_alloc` + `memset`, con la medida, esta en la cabecera. */
    if (n > kMaxSmall && allocator_active()) {
        ThreadCache *c = detail::current_cache();
        if (c == nullptr) c = detail::ensure_cache();
        if (c != nullptr) {
            bool already_zero = false;
            void *p = alloc_span(c, n, &already_zero);
            if (p != nullptr) {
                if (!already_zero) std::memset(p, 0, n);
                return p;
            }
        }
    }
    void *p = host_alloc(n);
    if (p != nullptr) std::memset(p, 0, n);
    return p;
}

/* Una tabla y no dos trozos pegados en un buffer: asi es reentrante, vale entre
 * hilos y se pueden poner dos en el mismo `printf`.  Las cuatro combinaciones
 * imposibles (forma 3) estan por no dejar un hueco que indexar mal. */
const char *alloc_tag_name(AllocTag t) noexcept {
    static const char *const kNames[AllocTag::kSlots] = {
        "unknown",     "unknown/fixed", "unknown/growing", "-",
        "instant",     "instant/fixed", "instant/growing", "-",
        "medium",      "medium/fixed",  "medium/growing",  "-",
        "long",        "long/fixed",    "long/growing",    "-"};
    return kNames[t.raw() & (AllocTag::kSlots - 1)];
}

namespace {

StatsDump::~StatsDump() {
    if (!g_measure) return;
    const HostAllocStats s = host_alloc_stats();
    std::fprintf(
        stderr,
        "[allocator] small=%llu freed=%llu freed-by-others=%llu "
        "large=%llu chunks=%llu\n",
        (unsigned long long)s.small_allocs, (unsigned long long)s.small_frees,
        (unsigned long long)s.remote_frees, (unsigned long long)s.large_allocs,
        (unsigned long long)s.chunks);
    /* CUANTA memoria se ha quedado, que es la pregunta que la cuenta de
     * reservas no contesta.  `bytes_reserved` se llevaba desde siempre y no lo
     * enseñaba nadie: sin el, para saber a donde iba mas de un giga habia que
     * multiplicar trozos por su tamano a mano.
     *
     * Y lo que queda VIVO al terminar: un trozo solo se puede devolver cuando
     * se vacia entero, asi que unos pocos objetos vivos repartidos lo dejan
     * comprometido.  La distancia entre "vivo" y "reservado" es la
     * fragmentacion, y es lo que hay que mirar cuando el pico no baja. */
    const long long alive = (long long)s.small_allocs -
                            (long long)s.small_frees -
                            (long long)s.remote_frees;
    std::fprintf(stderr,
                 "[allocator] committed=%.1f MiB in %zu KiB chunks | "
                 "small blocks ALIVE at exit=%lld\n",
                 (double)s.bytes_reserved / (1024.0 * 1024.0),
                 (size_t)(kChunkBytes / 1024), alive);
    /* Cuanta region se consiguio apalabrar, y cuantas veces nos rendimos.  Las
     * dos cifras existen porque las dos degradaban en SILENCIO: pedir 256 GiB
     * y conseguir 64 MiB, o quedarse sin region a mitad de camino, dejaban el
     * asignador sirviendo mucho menos de lo que parece sin decir nada. */
    const uint64_t gave_up = g_gave_up.load(std::memory_order_relaxed);
    std::fprintf(stderr, "[allocator] address space reserved=%.1f GiB",
                 (double)g_region_reserved.load(std::memory_order_relaxed) /
                     (1024.0 * 1024.0 * 1024.0));
    if (gave_up != 0)
        std::fprintf(stderr,
                     "  | GAVE UP TO THE SYSTEM: %llu  <- the allocator ran "
                     "short, these were NOT served here",
                     (unsigned long long)gave_up);
    std::fprintf(stderr, "\n");
    /* Reparto por PROPOSITO.  Es la cifra que dice cuanto queda por migrar: lo
     * que sale como "no se" es exactamente lo que todavia no declara nada.  Se
     * imprime siempre que haya reservas, no solo lo que no sea cero, para que
     * un cero se vea como cero y no como una linea que falta. */
    if (s.small_allocs != 0) {
        std::fprintf(stderr, "[allocator] breakdown by purpose:\n");
        for (uint32_t g = 0; g < AllocTag::kSlots; ++g) {
            if (s.by_tag[g] == 0) continue;
            std::fprintf(stderr, "    %-20s %12llu  %5.1f%%\n",
                         alloc_tag_name(AllocTag::from_raw((uint8_t)g)),
                         (unsigned long long)s.by_tag[g],
                         100.0 * double(s.by_tag[g]) /
                             double(s.small_allocs));
        }
    }
    static const char *kNames[kSizeBuckets] = {
        "<=64",   "<=256", "<=1K",  "<=2K", "<=4K",  "<=8K",
        "<=16K",  "<=64K", "<=256K", "<=1M", "<=16M", ">16M"};
    static_assert(sizeof(kNames) / sizeof(kNames[0]) == kSizeBuckets,
                  "cada tramo tiene que tener su nombre");
    uint64_t total = 0;
    for (uint32_t b = 0; b < kSizeBuckets; ++b)
        total += s.size_hist[b];
    if (total == 0) return;
    std::fprintf(stderr, "[allocator] breakdown of requested sizes:\n");
    for (uint32_t b = 0; b < kSizeBuckets; ++b)
        std::fprintf(stderr, "    %-6s %10llu  %5.1f%%\n", kNames[b],
                     (unsigned long long)s.size_hist[b],
                     100.0 * double(s.size_hist[b]) / double(total));
}

} // namespace

} // namespace util

// =========================================================================
//  operator new / delete globales
// =========================================================================
//
// Se reemplazan los del sistema para que TODO el proceso -- cada `std::vector`,
// cada cadena, cada nodo de tabla -- pase por aqui.  Es lo que hace que el
// cambio valga: en el perfil las reservas no estaban concentradas en ningun
// sitio, asi que solo se ganaba tocandolas todas a la vez.

void *operator new(size_t n) {
    void *p = util::host_alloc(n);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void *operator new[](size_t n) {
    void *p = util::host_alloc(n);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void *operator new(size_t n, const std::nothrow_t &) noexcept {
    return util::host_alloc(n);
}
void *operator new[](size_t n, const std::nothrow_t &) noexcept {
    return util::host_alloc(n);
}

void operator delete(void *p) noexcept {
    util::host_free(p);
}
void operator delete[](void *p) noexcept {
    util::host_free(p);
}
void operator delete(void *p, size_t) noexcept {
    util::host_free(p);
}
void operator delete[](void *p, size_t) noexcept {
    util::host_free(p);
}
void operator delete(void *p, const std::nothrow_t &) noexcept {
    util::host_free(p);
}
void operator delete[](void *p, const std::nothrow_t &) noexcept {
    util::host_free(p);
}
