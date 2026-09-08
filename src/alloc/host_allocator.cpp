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
 * Y tampoco con `getenv`, que tiene un problema distinto: no lee el entorno,
 * lee una COPIA que el runtime de C monta al arrancar, y esto puede correr
 * antes de que exista.  Las variables de este fichero se leen del bloque que
 * dio el sistema, por `util/os/os_env.h`.  De paso es lo que permite que esta
 * libreria no dependa de nada del compilador. */
#include "util/alloc/host_allocator.h"

#include "util/report/alloc_sites.h"
#include "util/interpose/call_site.h"

#include "util/os/os_env.h"
#include "util/os/os_memory.h"
#include "util/alloc/size_buckets.h"
#include "util/os/thread_slot.h"
#include "util/mem/vesta_memcpy.h"
#include "util/mem/vesta_memset.h"

#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <new>

/* Ya no hace falta `windows.h` ni `sys/mman.h`: todo el trato con el sistema
 * pasa por `util/os/os_memory.h`, que ademas es lo que usaran la arena de golpe y
 * el camino de reservas grandes.  Un solo sitio que hable con el sistema. */

namespace util {

/* La GEOMETRIA -- region, trozos, clases de tamano, cabecera de trozo -- vive
 * en `util/alloc/host_allocator_layout.h`, porque no la usa solo esto: tambien la
 * arena de golpe, los metadatos de diagnostico y la capa en C.  Aqui quedan las
 * definiciones de lo que la cabecera declara y todo lo que NO va en linea. */
namespace detail {

std::atomic<uintptr_t> g_region_base{0};
std::atomic<uintptr_t> g_region_end{0};

/* La region de las clases GRANDES.  A cero mientras nadie pida una: quien no
 * reserve nunca 2 KiB o mas no paga ni el apalabrado.  Ver `kBigChunkBytes`. */
std::atomic<uintptr_t> g_big_base{0};
std::atomic<uintptr_t> g_big_end{0};
uint8_t g_class_of[(kMaxSmall / kAlign) + 1];

/// La ranura por hilo con el puntero al cache.  Global normal, no de hilo: lo
/// que cambia por hilo es el CONTENIDO de la ranura.
ThreadSlot g_cache_slot;

#if VESTA_ALLOC_DIRECT_CACHE_TLS
/// El atajo del camino caliente.  Ver la nota en la cabecera; se mantiene en
/// paso con la ranura de arriba, que sigue siendo la fuente de verdad.
__thread ThreadCache *g_cache_direct __attribute__((tls_model("initial-exec")));
#endif

/// The same thing for the lock-free per-thread policy.  A separate slot because
/// a thread can use both allocators, and each has to find its own cache.
ThreadSlot g_per_thread_slot;

bool g_measure = false;

} // namespace detail

using detail::g_cache_slot;
#if VESTA_ALLOC_DIRECT_CACHE_TLS
using detail::g_cache_direct;
#endif
using detail::g_per_thread_slot;
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
 *
 * Los VALORES viven en `util/alloc/size_buckets.h`, y ahi por una razon: estaban
 * escritos DOS veces -- aqui y en el exportador -- y las dos copias ya habian
 * empezado a separarse.
 */
using util::bucket_of;
using util::kBucketLimit;
using util::kSizeBuckets;

// =========================================================================
//  Estado
// =========================================================================

/* Both policies index this: ids below `kMaxThreads` belong to the shared
 * allocator, the rest to `PerThreadAllocator`.  One table means `h->owner`
 * stays a plain index -- no indirection on the remote-free path -- and a block
 * can be freed through either door without a special case. */
ThreadCache g_caches[kTotalCaches];

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
/**
 * @brief A breather for the processor inside a spin.
 *
 * It neither sleeps nor gives up the core: it tells the execution unit that
 * this is a wait, so it stops filling the window with speculative loads that
 * have to be squashed on the way out, and it hands the sibling thread over to
 * the neighbour.
 *
 * It lives up here because BOTH locks use it -- the span one and the shared
 * cache one -- and the latter is declared first.
 */
[[gnu::always_inline]] inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#endif
}

/**
 * @brief The shared cache lock, with a cache line all to itself.
 *
 * On its own it shared a line with whatever the linker put next to it, so
 * anybody spinning on it also invalidated unrelated data.
 */
struct alignas(64) SharedLockWord {
    std::atomic<uint32_t> v;
    char pad[64 - sizeof(std::atomic<uint32_t>)];
};
SharedLockWord g_shared_lock{{0}, {}};

/**
 * @brief Takes and releases the shared cache lock.
 *
 * LOOK BEFORE YOU TRY, same as @c SpanLock: spinning on a bare exchange is one
 * read-modify-write per turn, and that steals the line in exclusive state from
 * the holder -- precisely the one that needs it in order to release.
 *
 * AND ALSO YIELD, which is what this lock needs and the span one does not.  The
 * difference is how many contend: spans are touched once in a while, whereas
 * every thread that ran out of an owner id comes through here, on every single
 * allocation.  Once those outnumber the cores, spinning without yielding stops
 * being a wait and becomes a convoy: the holder loses its processor and
 * everybody else burns a whole quantum waiting on a thread that is not running.
 *
 * MEASURED, not assumed.  With 20,000 allocations per thread on a 24-core
 * machine, the knee landed exactly when the shared path went past 24 threads:
 *
 *     threads   on shared   CPU ns per operation
 *          84          21                   46.5
 *          88          25                  167.4   <- the knee
 *         128          65                  537.1
 *
 * The useful work is the SAME in all three rows; what grows 19x is processor
 * time burned spinning.  That is why the spin count before yielding is small:
 * if the holder is running it releases quickly and the yield is never reached;
 * if it is not, spinning harder will not wake it up.
 *
 * @note This is the bounded-MEMORY policy.  A caller that would rather bound
 *       LATENCY takes no lock at all -- see the per-thread allocator, which
 *       gives every thread its own cache instead of sharing one.
 */
constexpr uint32_t kSharedSpins = 64;

struct SharedLock {
    SharedLock() noexcept {
        for (;;) {
            for (uint32_t i = 0; i < kSharedSpins; ++i) {
                if (g_shared_lock.v.exchange(1, std::memory_order_acquire) == 0)
                    return;
                // Mirar sin tocar mientras siga cogido.
                while (g_shared_lock.v.load(std::memory_order_relaxed) != 0)
                    cpu_relax();
            }
            os_yield();
        }
    }
    ~SharedLock() { g_shared_lock.v.store(0, std::memory_order_release); }
};

/**
 * @brief Lo que otros hilos han soltado y su dueno todavia no ha recogido.
 *
 * Una pila atomica por (dueno, clase).  Quien libera algo ajeno empuja aqui con
 * un `compare_exchange` -- sin bloquear a nadie -- y el dueno se lleva la pila
 * ENTERA de un golpe cuando se queda sin bloques.  Es lo que hace que liberar
 * entre hilos no cueste ni fugue.
 *
 * UNA LINEA DE CACHE ENTERA POR DUENO, y esto es lo unico de aqui que hay que
 * respetar al tocarlo.  Era un array de dos dimensiones a secas, y con 36
 * clases la fila de un dueno medía 288 bytes -- CUATRO LINEAS Y MEDIA --, asi
 * que la mitad de los duenos compartia linea con el siguiente.
 *
 * Y esta es justo la estructura que escriben OTROS hilos: cada liberacion ajena
 * es un `compare_exchange` aqui.  Dos hilos soltando bloques de duenos
 * distintos que cayeran en la misma linea se peleaban por ella sin tener nada
 * que ver el uno con el otro.  La firma en VTune es inconfundible y estaba
 * puesta: `Machine Clears` 3,6% de las ranuras de cauce con `L3 Bound` al 16,3%
 * y `DRAM Bound` al 5,3% -- lineas rebotando entre nucleos, que acaban en L3 y
 * no en memoria.
 *
 * `alignas(64)` en la fila hace dos cosas a la vez: la empieza en linea y
 * redondea su tamano a 320, de modo que la siguiente tambien empieza en linea.
 * Cuesta 2 KiB en todo el proceso.
 */
struct alignas(64) RemoteLists {
    std::atomic<void *> head[kClasses];
};
RemoteLists g_remote[kTotalCaches];

/**
 * @brief Cuantas veces ha entrado `operator new` POR HILO, solo al medir.
 *
 * POR QUE EXISTE.  Porque sin el, "cuantas veces se pidio reservar" y "cuantas
 * reservas conto el asignador" no se pueden separar: si no cuadran, no hay
 * forma de saber cual de las dos miente.  Es la tercera pata de la medida, y
 * este es su sitio -- `new_measured` solo existe cuando se ha pedido medir --.
 *
 * UNA LINEA POR DUENO Y SIN ATOMICOS, que es lo unico que hay que respetar al
 * tocarlo.  Un contador global con `fetch_add` seria una linea de cache que se
 * pelean todos los hilos EN CADA RESERVA, o sea que mediria mas despacio de lo
 * que hay -- el instrumento cambiando lo que mide --.  Aqui cada hilo escribe
 * en la suya, sin sincronizar nada, igual que sus listas.
 */
struct alignas(64) NewCalls {
    uint64_t n;
    char pad[64 - sizeof(uint64_t)];
};
NewCalls g_new_calls[kMaxThreads + 1]; ///< la ultima, para los hilos sin cache

} // namespace

namespace detail {

/**
 * @brief Apunta una entrada de `operator new`.  Solo se llama al medir.
 *
 * La ranura sale del dueno, asi que la escritura es de un solo hilo y no hace
 * falta sincronizar.  La EXCEPCION es la ultima -- los hilos que se quedaron
 * sin cache propio --, que si pueden pisarse: ahi la cifra es aproximada y se
 * dice al ensenarla, en vez de fingir exactitud sobre un caso que ademas es
 * raro.
 */
void note_new_call(const ThreadCache *c) noexcept {
    /* `have_cache` y no un nulo: un hilo pasado su aviso de fin lleva la marca
     * en la ranura, y preguntar solo por el nulo desreferenciaria el 1.  Sus
     * llamadas se cuentan con las de los hilos sin cache, que es donde se
     * sirven. */
    g_new_calls[have_cache(c) ? c->id : kMaxThreads].n++;
}


} // namespace detail

namespace {

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
/// Lo mismo para la region grande, que se monta aparte y mas tarde.
std::atomic<int> g_big_state{0};
/// Siguiente trozo grande libre, en unidades de `kBigChunkBytes`.
std::atomic<size_t> g_big_next{0};

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
        /* El contador tiene el MISMO tipo que la cota con la que se compara.
         * Con un `uint8_t`, `k + 1` se promociona a `int` con signo y la
         * comparacion contra `kClasses`, que no lo tiene, es un aviso -- y en
         * cuanto las clases pasaran de 127 dejaria de ser solo un aviso. */
        uint32_t k = 0;
        while (k + 1 < kClasses && kSizes[k] < want)
            ++k;
        g_class_of[step] = uint8_t(k);
    }
}

bool allocator_active() noexcept {
    const int s = g_active_state.load(std::memory_order_acquire);
    if (s != 0) return s == 1;
    /* Se deja APAGADO mientras se decide: si preguntar pidiera memoria por
     * dentro, esa peticion entraria aqui otra vez y se quedaria dando vueltas.
     * La ventana es esta funcion y nada mas; quien caiga dentro se va por el
     * camino de "no somos el asignador", que la capa de interposicion sabe
     * servir -- ver `stand_aside` en `malloc_interpose.cpp`.
     *
     * ESTE ESTADO YA NO SIGNIFICA DOS COSAS.  Mientras existio
     * `VESTA_NO_HOST_SLAB`, el mismo 2 queria decir "decidiendo" y "decidido
     * que no", y eso se pagaba: el diagnostico decia "todavia no ha decidido"
     * cuando ya habia decidido, y la negativa mataba el proceso donde habia una
     * respuesta honesta que dar. */
    g_active_state.store(2, std::memory_order_release);
    /* Al SISTEMA (`util/os/os_env.h`), ni por el registro de mandos del
     * compilador (`util/env_flags.h`) ni por `getenv`.  Son dos fallos
     * distintos y los dos ya mordieron:
     *
     *  - El registro guarda el valor de sus mandos en cadenas, asi que
     *    consultarlo la primera vez PIDE MEMORIA -- y pedir memoria entra
     *    aqui.  Con su estatico a medio construir, esa reentrada se queda
     *    esperando su guarda para siempre y el proceso cuelga antes de llegar
     *    a `main`.
     *  - `getenv` no lee el entorno: lee una COPIA que el runtime de C monta
     *    al arrancar.  Y esto corre en la PRIMERA reserva, que puede caer
     *    durante la inicializacion de estaticos -- cualquier global cuyo
     *    constructor pida memoria llega antes que `main` --, donde esa copia
     *    puede no existir todavia.  Entonces `getenv` devuelve nulo y TODOS
     *    los mandos salen apagados, sin fallar y sin decirlo.
     *
     * Es el mismo principio en los dos casos: quien resuelve las reservas no
     * puede apoyarse en nada que reserve, ni en nada que haya que montar
     * antes. */
    /* AQUI SE LEIA `VESTA_NO_HOST_SLAB`, y ya no.
     *
     * Existia para tener con que comparar -- sin poder apagar el asignador no
     * hay forma de saber si mejora algo --, y era la unica forma que habia.  Ya
     * no lo es: `support/system_alloc.h` alcanza el asignador del sistema DESDE
     * DENTRO del mismo proceso, cargando una copia propia del runtime de C, asi
     * que las dos columnas se miden intercaladas en la misma vuelta.  Eso es
     * mejor instrumento por una razon concreta: dos procesos no se pueden
     * intercalar, y sin intercalar cada columna se mide en segundos de reloj
     * distintos -- si la maquina se frena en uno de los dos, la comparacion se
     * lo traga sin enterarse.
     *
     * Y el interruptor no salia gratis.  El renombrado de `malloc` es de tiempo
     * de ENLACE y no se deshace en ejecucion, asi que con el puesto toda reserva
     * seguia entrando aqui, se encontraba con que no estabamos en vigor y se
     * negaba -- y la negativa no retorna.  Medido: el runtime de C pedia 105
     * bytes al arrancar y el proceso moria ahi, antes de `main`.  El interruptor
     * que existia para poder correr el control impedia correr nada.
     *
     * Se quita entero en vez de arreglarse porque arreglarlo pedia darle al
     * nucleo un respaldo al sistema, y eso es justo lo que este fichero se niega
     * a tener: un respaldo silencioso hace que las cifras dejen de coincidir con
     * la realidad sin que nadie lo note. */
    /* Dos formas de pedir lo mismo.  `..._STATS` pide el reparto de tamanos y
     * `..._SITES` pide de donde vienen las reservas, y las dos necesitan que se
     * apunte -- si no, no hay nada que ensenar.  Se miran las dos aqui porque
     * esto corre ANTES de la primera reserva, y encenderlo despues dejaria
     * fuera todo lo de arranque sin decirlo.
     *
     * Cada variable se lee UNA vez y se guarda: `..._SITES` decide dos cosas
     * -- si se apunta y si se parchea `operator new` -- y preguntarla dos veces
     * abre la puerta a que las dos decisiones no coincidan. */
    const bool sites = os_env_flag("VESTA_HOST_ALLOC_SITES");
    g_measure = os_env_flag("VESTA_HOST_ALLOC_STATS") || sites;
    /* And if SITES are going to be recorded, make the `operator new` family say
     * where they came from.  Here and not earlier: this is the only moment when
     * we know it is needed, there is still a single thread, and nothing has
     * been allocated yet -- the three conditions writing over code asks for.
     *
     * It looks at SITES and not at `g_measure`: the SIZE histogram does not
     * need to know where an allocation came from, so asking for `..._STATS`
     * alone has no reason to pay for the jump. */
    if (sites) util::install_call_site_patch();
    build_class_table();
    g_active_state.store(1, std::memory_order_release);
    return true;
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
     * El trato con el sistema esta en `util/os/os_memory.h`, que es el UNICO sitio
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
 * @brief Apalabra la region de las clases GRANDES, la primera vez que hace
 *        falta.
 *
 * Aparte de la pequena, y no por simetria: la mascara que localiza la cabecera
 * de un puntero depende del tamano de trozo, asi que dos tamanos de trozo
 * necesitan dos rangos.  Mezclados, el camino de liberar tendria que averiguar
 * de cual es antes de enmascarar y eso lo pagarian tambien las reservas
 * pequenas, que son el 81%.
 *
 * Y PEREZOSA: un programa que nunca reserve 2 KiB o mas no llega aqui, asi que
 * no paga ni las direcciones.  Montarla al arrancar seria cobrarle a todos por
 * algo que usa el 0,9% de las reservas.
 */
bool ensure_big_region() noexcept {
    int s = g_big_state.load(std::memory_order_acquire);
    if (s == 2) return true;
    if (s == 3) return false;
    int expected = 0;
    if (!g_big_state.compare_exchange_strong(expected, 1,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
        while ((s = g_big_state.load(std::memory_order_acquire)) == 1) {
        }
        return s == 2;
    }
    size_t reserved = 0;
    void *base =
        os_reserve_largest(kBigRegionBytes, kBigRegionMinBytes, &reserved);
    if (base == nullptr) {
        /* Sin region grande NO se apaga nada: las clases grandes se siguen
         * sirviendo del trozo pequeno como siempre, solo que desperdiciando.
         * Degradar aqui es aceptable porque es reversible y no cambia el
         * resultado; lo que no seria aceptable es que fuera mudo, y por eso el
         * estado 3 se consulta al decidir. */
        g_big_state.store(3, std::memory_order_release);
        return false;
    }
    const uintptr_t b = reinterpret_cast<uintptr_t>(base);
    /* La base tiene que estar alineada al trozo GRANDE, que es lo que la
     * mascara da por hecho.  Se pierde como mucho un trozo. */
    const uintptr_t aligned =
        (b + kBigChunkBytes - 1) & ~(uintptr_t)(kBigChunkBytes - 1);
    detail::g_big_base.store(aligned, std::memory_order_relaxed);
    detail::g_big_end.store(b + reserved, std::memory_order_relaxed);
    g_big_state.store(2, std::memory_order_release);
    return true;
}

/// Aparta @p count trozos grandes seguidos.  Cero si la region no da mas.
uintptr_t take_big_chunks(size_t count) noexcept {
    if (!ensure_big_region()) return 0;
    const size_t idx = g_big_next.fetch_add(count, std::memory_order_acq_rel);
    const uintptr_t base = detail::g_big_base.load(std::memory_order_relaxed);
    const uintptr_t addr = base + idx * kBigChunkBytes;
    if (addr + count * kBigChunkBytes >
        detail::g_big_end.load(std::memory_order_relaxed))
        return 0;
    return addr;
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
/// Lo que devuelve @c take_cache_id cuando no queda ninguno.
constexpr uint32_t kNoCacheId = 0xFFFFFFFFu;

/**
 * @brief Que identificadores estan LIBRES, un bit cada uno.
 *
 * UN MAPA DE BITS Y NO UN CONTADOR, y las dos razones importan:
 *
 *  - **Se pueden devolver.**  Un contador que solo sube no admite reciclar, y
 *    sin reciclar el tope no cuenta hilos vivos sino hilos que hayan existido
 *    alguna vez.  Ver @c kMaxThreads.
 *  - **Agotado no cuesta nada.**  Repartir con `fetch_add` es un
 *    read-modify-write: se lleva la linea a Exclusive y se la quita a los demas
 *    nucleos.  Con el mostrador vacio eso lo hacian TODOS los hilos en CADA
 *    reserva -- y en cada liberacion ajena, porque @c host_free_remote pide el
 *    cache solo para sumar un contador --, moviendo esa linea entre nucleos sin
 *    que nadie sacara nada.  Medido con VTune, 24 hilos, mostrador agotado:
 *    `cache` 3,17 s y `host_free_remote` 2,33 s de CPU, todo ahi.  Con el mapa,
 *    vacio es `m == 0` y se sale con una LECTURA de una linea que ya nadie
 *    escribe -- Shared en todos los nucleos, acierto local.
 *
 * Cabe en una palabra porque @c kMaxThreads son 64, que es justo lo que mide.
 * Si alguien la cambia, el @c static_assert de abajo lo dice en vez de dejar
 * identificadores inalcanzables en silencio.
 */
static_assert(kMaxThreads == 64,
              "el reparto de identificadores es un mapa de bits de UNA palabra;"
              " con otro kMaxThreads hay que cambiar el mapa, no el tope");
/// Todos libres menos @c kSharedCacheId, que es del cache compartido.
std::atomic<uint64_t> g_free_ids{~(uint64_t(1) << kSharedCacheId)};

/// Cuantas veces se pidio identificador y no quedaba.  Ver @c take_cache_id.
std::atomic<uint64_t> g_no_cache_id{0};

/// El bit mas bajo puesto.  Sin `<bit>`, que es de C++20 y esto es C++17.
[[gnu::always_inline]] inline uint32_t lowest_set(uint64_t m) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return uint32_t(__builtin_ctzll(m));
#elif defined(_MSC_VER)
    unsigned long i;
    _BitScanForward64(&i, m);
    return uint32_t(i);
#else
    uint32_t i = 0;
    while ((m & 1u) == 0u) {
        m >>= 1;
        ++i;
    }
    return i;
#endif
}

/**
 * @brief Un identificador libre, o @c kNoCacheId si no queda.
 *
 * EN LINEA a proposito, aunque parezca camino frio.  Lo es mientras queden
 * identificadores -- una vez por hilo --, pero en cuanto se agotan @c cache lo
 * llama en CADA reserva, y entonces lo unico que se ejecuta es la carga y la
 * comparacion de arriba.  Dejarlo fuera convertiria eso en una llamada.
 */
[[gnu::always_inline]] inline uint32_t take_cache_id() noexcept {
    uint64_t m = g_free_ids.load(std::memory_order_relaxed);
    while (__builtin_expect(m != 0, 1)) {
        /* `m & (m - 1)` apaga justo el bit mas bajo, que es el que se lleva.
         * Si el intercambio falla, `m` se queda con lo que habia de verdad y se
         * reintenta con otro bit; no hace falta releer. */
        if (g_free_ids.compare_exchange_weak(m, m & (m - 1),
                                             std::memory_order_acq_rel,
                                             std::memory_order_relaxed))
            return lowest_set(m);
    }
    /* SE CUENTA, porque si no esta cota se cruza en silencio: quien se queda
     * sin identificador no falla, pasa a servirse de las listas compartidas
     * detras del unico cerrojo que hay, y desde fuera lo unico que se nota es
     * que todo va mas lento sin ninguna razon visible.  Es la misma regla que
     * `g_gave_up`, y por el mismo motivo. */
    g_no_cache_id.fetch_add(1, std::memory_order_relaxed);
    return kNoCacheId;
}

/**
 * @brief Devuelve un identificador al reparto.
 *
 * Lo que queda dentro de ese cache -- listas libres, tramos guardados, y la
 * pila de lo que otros le soltaron -- NO se toca: son bloques validos de trozos
 * que siguen siendo nuestros, y quien tome el identificador despues los HEREDA.
 * Tirarlos seria devolverlos al sistema uno a uno; dejarlos sin dueno era lo
 * que se hacia antes, y es una fuga.
 *
 * Es una sola escritura atomica a proposito: esto corre durante el desmontaje
 * de un hilo.  Ver `util/thread_exit.h`.
 */
[[gnu::cold]] void give_cache_id(uint32_t id) noexcept {
    if (__builtin_expect(id >= kSharedCacheId, 0)) return;
    g_free_ids.fetch_or(uint64_t(1) << id, std::memory_order_release);
}

/**
 * @brief Lo que corre al morir un hilo que tenia cache.
 *
 * FRIO Y MINIMO, y lo segundo no es estilo: esto se ejecuta DURANTE el
 * desmontaje del hilo, que es donde este proyecto ya se colgo una vez.  Una
 * escritura de ranura y una operacion atomica; ni reservar, ni liberar, ni
 * tomar el cerrojo, ni llamar al sistema.
 *
 * Se limpia la ranura ANTES de soltar el identificador.  Si no, algo que
 * reservara despues del aviso -- un destructor estatico, otra devolucion de
 * llamada del desmontaje -- seguiria viendo el cache por la ranura mientras
 * otro hilo ya lo tiene: dos duenos para una sola estructura.  Limpiandola
 * primero, ese caso pide un identificador nuevo, que es correcto.
 */
[[gnu::cold]] void on_thread_exit(void *value) noexcept {
    ThreadCache *c = static_cast<ThreadCache *>(value);
    /* Nulo, o la marca que este mismo aviso deja puesta.  Lo segundo pasa si el
     * sistema vuelve a avisar del mismo hilo despues de que la ranura se haya
     * rearmado con la marca; devolver aqui es lo correcto y evita tratar el 1
     * como un puntero. */
    if (c == nullptr ||
        reinterpret_cast<uintptr_t>(c) == detail::kDyingCache)
        return;
    ThreadCache *const dying =
        reinterpret_cast<ThreadCache *>(detail::kDyingCache);
#if VESTA_ALLOC_DIRECT_CACHE_TLS
    g_cache_direct = dying; // en paso con la ranura, siempre
#endif
    /* LA MARCA, y no un nulo.  Con un nulo, lo que reserve despues de este
     * aviso -- y en Windows el desmontaje del hilo reserva SIEMPRE, ver
     * `kDyingCache` -- no encontraria cache y pediria un identificador nuevo
     * que ya nadie puede devolver, porque el unico aviso es este.  Con la
     * marca, ese caso se va por las listas compartidas y no se lleva nada. */
    /* `set_quiet` y no `set`: estamos DENTRO del aviso, y `set` lo rearmaria
     * dejando el canal del sistema ocupado mientras el sistema lo recorre.  Eso
     * cuelga el desmontaje del hilo -- comprobado, cinco de cinco corridas. */
    g_cache_slot.set_quiet(dying);
    give_cache_id(c->id);
}

// =========================================================================
//  The lock-free per-thread policy: one cache per thread, no shared fallback
// =========================================================================

/**
 * @brief Which per-thread ids are TAKEN.  One bit each, set means taken.
 *
 * The meaning is inverted with respect to @c g_free_ids on purpose.  "Set means
 * taken" makes the all-zero state mean "everything free", and all-zero is what
 * `.bss` gives for nothing: no initialiser runs, so this cannot repeat the
 * static-initialisation-order trap that already wiped the cache table once --
 * an array that initialises itself AFTER the allocator is already serving.
 */
std::atomic<uint64_t> g_per_thread_taken[kPerThreadCaches / 64];

/// How many times a thread asked for a per-thread cache and none was left.
std::atomic<uint64_t> g_no_per_thread_id{0};

/**
 * @brief Takes a per-thread id, or @c kNoCacheId when the pool is exhausted.
 *
 * Cold by construction: a thread comes through here ONCE, on its first
 * allocation, and from then on the id lives in the thread slot.  That is what
 * lets this scan words instead of being a single-word test like
 * @c take_cache_id -- and it is also why running out is not a fallback here.
 */
[[gnu::cold]] uint32_t take_per_thread_id() noexcept {
    for (uint32_t w = 0; w < kPerThreadCaches / 64; ++w) {
        uint64_t taken = g_per_thread_taken[w].load(std::memory_order_relaxed);
        while (taken != ~uint64_t(0)) {
            // The lowest bit that is still zero is the first free id.
            const uint32_t bit = lowest_set(~taken);
            if (g_per_thread_taken[w].compare_exchange_weak(
                    taken, taken | (uint64_t(1) << bit),
                    std::memory_order_acq_rel, std::memory_order_relaxed))
                return kMaxThreads + w * 64 + bit;
            // `taken` now holds what was really there; retry with another bit.
        }
    }
    /* IT IS COUNTED, because otherwise this bound is crossed in silence.  This
     * policy has no shared fallback by design, so running out is a real limit
     * and has to be visible -- the same rule as `g_gave_up`. */
    g_no_per_thread_id.fetch_add(1, std::memory_order_relaxed);
    return kNoCacheId;
}

/// Gives a per-thread id back.  One atomic write: this runs at thread teardown.
[[gnu::cold]] void give_per_thread_id(uint32_t id) noexcept {
    if (id < kMaxThreads || id >= kTotalCaches) return;
    const uint32_t i = id - kMaxThreads;
    g_per_thread_taken[i / 64].fetch_and(~(uint64_t(1) << (i % 64)),
                                         std::memory_order_release);
}

/// Returns the id when the thread dies, exactly like @c on_thread_exit does for
/// the shared policy.  What the cache still holds is inherited by whoever takes
/// the id next: they are valid blocks of chunks that are still ours.
[[gnu::cold]] void on_per_thread_exit(void *value) noexcept {
    ThreadCache *c = static_cast<ThreadCache *>(value);
    if (c == nullptr ||
        reinterpret_cast<uintptr_t>(c) == detail::kDyingCache)
        return;
    /* La misma marca y por el mismo motivo que en @c on_thread_exit: lo que
     * reserve despues del aviso no puede llevarse un identificador, porque este
     * hilo ya no tiene forma de devolverlo. */
    g_per_thread_slot.set_quiet(
        reinterpret_cast<ThreadCache *>(detail::kDyingCache));
    give_per_thread_id(c->id);
}

ThreadCache *cache() noexcept {
    if (!g_cache_slot.ensure()) return nullptr;
    ThreadCache *c = static_cast<ThreadCache *>(g_cache_slot.get());
    if (have_cache(c)) return c;
    /* PAST ITS EXIT NOTICE, so no id and no cache: whatever this thread still
     * allocates goes down the shared path and takes nothing with it to the
     * grave.  Handing it a fresh id here is what used to drain the pool one
     * thread at a time; see @c kDyingCache. */
    if (c != nullptr) return nullptr;
    /* Sin listas propias.  Devolver nulo NO significa "al sistema": significa
     * "por el camino lento", que es donde estan las listas COMPARTIDAS.  Ver
     * @c alloc_shared.
     *
     * Y pasarse NO cuesta "un cerrojo", que es lo que ponia aqui: cuesta que
     * todas las reservas de todos los hilos desbordados pasen por el MISMO
     * cerrojo de giro.  Medido con 24 hilos y el mismo binario, con la unica
     * diferencia de 70 hilos que nacieron, reservaron una vez y murieron:
     * 1,73 -> 447,86 ns por operacion, 259 veces mas, con 69,58 s de CPU
     * girando dentro de `SharedLock`.  Con los identificadores reciclados ese
     * caso deja de ser el de cualquier programa que use hilos y pasa a ser lo
     * que dice ser: mas de 63 duenos a la vez. */
    const uint32_t id = take_cache_id();
    if (__builtin_expect(id == kNoCacheId, 0)) return nullptr;
    c = &g_caches[id];
    c->id = id;
    c->used = true;
    /* Lo que hubiera dejado el dueno anterior se HEREDA -- bloques validos de
     * trozos nuestros --, menos su etiqueta: el proposito con el que reservaba
     * otro hilo no es el nuestro, y arrastrarla haria mentir al reparto por
     * etiqueta desde la primera reserva. */
    c->tag = 0;
    /* El aviso se pide ANTES de dejar el valor: es @c set quien lo arma, asi
     * que al reves este hilo se quedaria sin avisar y su identificador no
     * volveria. */
    g_cache_slot.notify_on_exit(&on_thread_exit);
    g_cache_slot.set(c);
#if VESTA_ALLOC_DIRECT_CACHE_TLS
    /* DESPUES de la ranura, no antes: quien mire la ranura tiene que ver lo
     * mismo, y este es el orden en el que no hay un instante con el atajo
     * puesto y la ranura todavia vacia. */
    g_cache_direct = c;
#endif
    return c;
}

// =========================================================================
//  Tramos: las reservas grandes, servidas por nosotros
// =========================================================================
//
// Un tramo son N trozos seguidos que forman UNA reserva.  Medido, el 90% de las
// grandes son de 64 KiB o menos y la cola llega a 16 MiB, asi que no compensa
// ni tratarlas como clases -- la
// fragmentacion se come la ganancia -- ni pedir cada una al sistema, que seria
// una llamada por reserva.
//
// POR QUE UN CERROJO AQUI Y NO EN EL RESTO.  Porque para ESTE consumidor no
// cuesta: en una compilacion entera hay 564 reservas grandes frente a 62
// millones de pequenas, o sea una de cada cien mil.  Una pila atomica exigiria
// resolver el problema del ABA -- varios hilos sacando a la vez -- y eso son
// mas instrucciones y mas formas de equivocarse.  El camino rapido de verdad,
// el de las pequenas, no toca esto ni de lejos.
//
// PERO ESE REPARTO NO VIAJA CON EL ASIGNADOR.  Un consumidor de bufers grandes
// desde muchos hilos cae de lleno aqui, y ahi el cerrojo deja de ser gratis.
// Por eso lo que se le exige a este cerrojo no es ser rapido, es no tener
// ACANTILADOS: nada de llamadas al sistema dentro de la seccion critica, y
// nada de girar de una forma que empeore sola al crecer el numero de nucleos.
// Medido con el banco de tramos: sacar la llamada al sistema vale 5,7x con 48
// hilos, y no le cuesta nada a quien no pasa por ahi.

/**
 * @brief El cerrojo de los tramos, con su linea de cache para el solo.
 *
 * POR QUE UNA LINEA ENTERA.  Sin esto queda pegado al final de `g_span_free`
 * -- comprobado en el objeto: el array termina en 0xC68 y el cerrojo empezaba
 * justo ahi --, con lo que las ultimas listas de libres viven en la MISMA
 * linea que la palabra que todos los que esperan estan mirando.  Cada
 * escritura en esas listas les invalida la copia y les obliga a volver a
 * pedirla, que es exactamente lo que este cerrojo intenta evitar.  Cuesta 60
 * bytes de `.bss` y su ausencia solo se paga en las clases de tramo que este
 * proyecto no usa: justo el tipo de mina que no puede llevar dentro algo
 * pensado para reusarse.
 */
struct alignas(64) SpanLockWord {
    std::atomic<uint32_t> v{0};
    char pad[64 - sizeof(std::atomic<uint32_t>)];
};
SpanLockWord g_span_lock;

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
 * @brief Marca en `ChunkHeader::extra` de un tramo que esta LIBRE.
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

/**
 * @brief Toma y suelta el cerrojo de tramos.  Se gira, no se duerme.
 *
 * POR QUE MIRAR ANTES DE INTENTAR.  Un giro sobre un intercambio a secas es una
 * lectura-modificacion-escritura por vuelta, y eso se lleva la linea en
 * EXCLUSIVA cada vez: los que esperan se la quitan unos a otros y, lo que es
 * peor, se la quitan al que la tiene cogida, que la necesita justamente para
 * soltarla.  Mirando primero con una lectura normal, los que esperan se quedan
 * con la linea COMPARTIDA y no molestan a nadie; solo intentan el cambio
 * cuando la ven libre.
 *
 * HONESTIDAD SOBRE ESTA FORMA: con las llamadas al sistema ya fuera de la
 * seccion critica, el banco de tramos NO distingue esto del giro anterior.
 * Esta asi porque sin contencion cuesta exactamente lo mismo -- un intercambio
 * -- y lo que evita es una degradacion que crece con el numero de nucleos de
 * quien lo ejecute, que es un dato que no tenemos.  Es criterio, no medida.
 */
struct SpanLock {
    SpanLock() noexcept {
        for (;;) {
            if (g_span_lock.v.exchange(1, std::memory_order_acquire) == 0)
                return;
            // Mirar sin tocar mientras siga cogido.
            while (g_span_lock.v.load(std::memory_order_relaxed) != 0)
                cpu_relax();
        }
    }
    ~SpanLock() { g_span_lock.v.store(0, std::memory_order_release); }
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
    return h->magic == kSpanMagic && h->extra == kSpanFree;
}

/**
 * @brief Un bit por trozo: "aqui EMPIEZA un tramo LIBRE".
 *
 * POR QUE EXISTE.  Para poder preguntar por el vecino de la derecha SIN LEER SU
 * MEMORIA.  Antes se usaba `g_chunk_next` como "hasta aqui se puede leer", y
 * eso es falso: ese contador sube al REPARTIR el trozo, o sea antes de
 * comprometerlo y antes de escribir su cabecera.  En esa ventana, un hilo que
 * soltaba un tramo miraba a su derecha, creia que habia vecino y leia memoria
 * sin comprometer.  Dos formas de acabar: un acceso invalido, o -- peor -- que
 * la basura pase por un tramo libre y se absorba.  TSan lo nombro: lectura en
 * `is_free_span` contra la escritura de `h->magic` en `alloc_span`.
 *
 * Con el mapa la pregunta cambia de "esta esa memoria repartida?" a "hay un
 * tramo libre que empieza justo ahi?", y eso es un dato que solo cambia con el
 * cerrojo cogido.
 *
 * Y trae una segunda propiedad de la que depende lo de abajo: **la cabecera de
 * un tramo EN USO no la lee nadie mas que su dueno.**  Quien fusiona solo mira
 * los marcados libres.  De ahi sale que se pueda comprometer memoria de un
 * tramo propio con el cerrojo ya soltado.
 *
 * SIN ATOMICOS a proposito: se pone y se quita en `list_insert`/`list_remove` y
 * se consulta en `right_neighbour`, y las tres cosas ocurren con el cerrojo
 * cogido.  Atomicos aqui serian pagar otra vez por una exclusion que ya existe.
 */
constexpr size_t kMaxChunksTotal = size_t(256) << 14; ///< 256 GiB / 64 KiB

/* ATOMIC ONLY WHERE IT HAS TO BE.  Under the policies that keep a lock, every
 * touch of this map already happens with that lock held, and making the words
 * atomic would be paying a second time for an exclusion that exists -- which is
 * what the note above said, and it was right for those.
 *
 * The lock-free policy is the exception, and not for the usual reason: there
 * the bit is not just a note about the map, it IS the claim.  Absorbing a
 * neighbour means clearing its bit, and whoever clears it owns the span; two
 * threads reaching for the same neighbour have to have exactly one winner.  A
 * read-modify-write on a plain word cannot promise that, and the failure would
 * be the worst kind -- the same memory handed to two callers. */
#if VESTA_ALLOC_SPAN_POLICY == VESTA_SPAN_LOCKFREE
using FreeMapWord = std::atomic<uint64_t>;
#else
using FreeMapWord = uint64_t;
#endif
FreeMapWord g_free_span_map[kMaxChunksTotal / 64]; ///< 512 KiB de `.bss`

// -------------------------------------------------------------------------
//  Span links, kept OUTSIDE the spans
// -------------------------------------------------------------------------
//
//  WHY THEY CANNOT LIVE INSIDE.  A free span used to carry its own `prev`/`next`
//  right behind its header, which works as long as one lock covers everything.
//  Without that lock it stops working, and not because of tearing -- because of
//  REPURPOSING: the moment a span is absorbed by its left neighbour, the bytes
//  that held its links become interior payload of the merged span, and the next
//  caller to receive it writes over them.  Another thread walking the list is
//  then reading whatever a user wrote.  No atomic fixes that; the memory is
//  simply not the list's any more.
//
//  With the link in a side array indexed by CHUNK NUMBER, the slot belongs to
//  the allocator for the life of the process.  Reading it is always safe, even
//  for a span that has since been absorbed, handed out and written over -- the
//  reader gets a stale index, notices, and moves on.  That is what makes the
//  atomic versions possible at all.
//
//  A 32-bit chunk index and not a pointer, on purpose: it halves the array and
//  leaves room for a tag in the same 64-bit word as the head, which is what
//  keeps the pop free of the ABA problem without a double-width compare.
//
//  16 MiB of `.bss`, and `.bss` is handed over a page at a time: a program that
//  only ever touches a gigabyte of spans pays for 16 thousand entries, not for
//  four million.

/* Everything here stores a chunk number PLUS ONE, so that zero means "none".
 * Chunk zero is a real chunk, so it cannot be the empty marker -- and the
 * marker has to be zero, because these live in `.bss` and the loader zeroes
 * them before a single instruction of the program runs.  Giving them an
 * initialiser instead would make the arrays non-trivially-constructible and
 * push them into DYNAMIC initialisation, which runs among the global
 * constructors, by which time the allocator is already serving.  That exact
 * mistake has already been made once in this file; see the guard next to
 * `ThreadCache`. */
std::atomic<uint32_t> g_span_next[kMaxChunksTotal];

/// Que numero de trozo de la region es @p addr.  Fuera de la region sale un
/// numero enorme, que quien lo use descarta por rango.
inline size_t chunk_index_of(uintptr_t addr) noexcept {
    return (addr - g_region_base.load(std::memory_order_relaxed)) / kChunkBytes;
}

/// Where chunk number @p i starts.  The inverse of @c chunk_index_of.
inline ChunkHeader *chunk_at(uint32_t i) noexcept {
    return reinterpret_cast<ChunkHeader *>(
        g_region_base.load(std::memory_order_relaxed) +
        uintptr_t(i) * kChunkBytes);
}

/**
 * @brief A stack of spans that costs no lock, one per chunk count.
 *
 * The head carries a TAG in its upper half, and that is what makes the pop
 * safe.  Without it the classic hole opens: a reader takes the head, is
 * descheduled, and by the time it returns the same span has been popped, used,
 * freed and pushed again -- the head looks untouched, the compare-exchange
 * succeeds, and the stack is left pointing at whatever came after the span the
 * FIRST time.  Bumping a counter on every push and pop means "the same head"
 * can no longer be confused with "the head has not moved", and the exchange
 * fails as it should.
 *
 * Thirty-two bits of tag: to fool it, four thousand million pushes would have
 * to land inside one reader's window.
 *
 * @par Threads
 * **Safe from any thread**, with no lock.  What it does NOT protect is the
 * span's contents: a span in here must not be absorbed by anybody, and that is
 * an invariant of the caller, not of this stack.
 */
struct SpanStack {
    /* No initialiser, on purpose: see the note above `g_span_next`.  Zero is
     * the empty stack, and the loader provides it. */
    std::atomic<uint64_t> head;

    /// Low half: chunk number plus one, zero meaning none.  High half: the tag.
    static uint64_t pack(uint32_t idx1, uint32_t tag) noexcept {
        return (uint64_t(tag) << 32) | idx1;
    }
    static uint32_t index1_of(uint64_t v) noexcept { return uint32_t(v); }
    static uint32_t tag_of(uint64_t v) noexcept { return uint32_t(v >> 32); }

    void push(ChunkHeader *h) noexcept {
        const uint32_t idx1 =
            uint32_t(chunk_index_of(reinterpret_cast<uintptr_t>(h))) + 1u;
        uint64_t old = head.load(std::memory_order_relaxed);
        for (;;) {
            g_span_next[idx1 - 1u].store(index1_of(old),
                                         std::memory_order_relaxed);
            const uint64_t want = pack(idx1, tag_of(old) + 1u);
            if (head.compare_exchange_weak(old, want, std::memory_order_release,
                                           std::memory_order_relaxed))
                return;
        }
    }

    ChunkHeader *pop() noexcept {
        uint64_t old = head.load(std::memory_order_acquire);
        for (;;) {
            const uint32_t idx1 = index1_of(old);
            if (idx1 == 0) return nullptr;
            /* Reading the link of a span somebody else may already have taken
             * is fine: the slot is ours for the life of the process, so the
             * worst case is a stale value -- and then the tag has moved and the
             * exchange below refuses it. */
            const uint32_t next =
                g_span_next[idx1 - 1u].load(std::memory_order_relaxed);
            const uint64_t want = pack(next, tag_of(old) + 1u);
            if (head.compare_exchange_weak(old, want, std::memory_order_acq_rel,
                                           std::memory_order_acquire))
                return chunk_at(idx1 - 1u);
        }
    }
};

static_assert(__is_trivially_constructible(SpanStack),
              "SpanStack must be constructible without running code: an array "
              "of these has to live in .bss, or it is initialised AFTER the "
              "allocator is already serving.  This has been broken once.");

#if VESTA_SPAN_HAS_PARKING
/**
 * @brief Spans waiting to be handed out again, reached without any lock.
 *
 * WHAT MAKES IT SAFE is what it does NOT do: a span in here is not marked free
 * and is in no free list, so no coalescer can find it, and nothing will absorb
 * it while somebody is walking the stack.  It is the same invariant the
 * per-thread span cache already relies on, moved up to something every thread
 * can reach.
 *
 * WHAT IT COSTS is that a span sitting here is not being coalesced, so two
 * adjacent free spans can stay apart.  That is fragmentation traded for the
 * lock, and it is bounded: past @c span_recycle_ceiling a freed span goes back to
 * the shared pool, where it gets merged as before.
 */
SpanStack g_span_recycle[kMaxSpanChunks + 1];

/// Bytes currently parked in @c g_span_recycle.
std::atomic<size_t> g_recycle_bytes;

/**
 * @brief The floor of the ceiling, and the share of the program it grows to.
 *
 * IT IS A CAP, NOT A RESERVATION, and that distinction is what makes a generous
 * one safe: only spans the program actually freed can be parked, so a program
 * that never frees a megabyte parks nothing however high this is set.  What it
 * bounds is the WORST case.
 *
 * WHY A SHARE AND NOT A NUMBER.  A fixed ceiling is the same mistake as the two
 * before it -- `kSpanCacheSlots = 2` justified with one consumer's histogram --
 * wearing different clothes: 64 MiB is generous for a compiler and sixteen
 * buffers for something working on images, and neither of them wrote it.
 * Measured on the span-heavy profile, that one constant was worth 2.5x:
 *
 *     ceiling    ns/op/core   over the locked policy
 *      64 MiB        1315.6                    1.7x
 *     256 MiB         625.0                    3.7x
 *    1024 MiB         528.8                    4.3x
 *
 * ...with committed memory flat and fragmentation unchanged.  So the ceiling
 * follows the program: a share of what it has taken from the region, which is
 * the cheapest honest measure of "how big is this program" -- one relaxed load
 * of a counter that already exists, and only on the parking path.
 */
#ifndef VESTA_ALLOC_SPAN_RECYCLE_MIN
#define VESTA_ALLOC_SPAN_RECYCLE_MIN (size_t(64) << 20)
#endif
#ifndef VESTA_ALLOC_SPAN_RECYCLE_SHIFT
#define VESTA_ALLOC_SPAN_RECYCLE_SHIFT 2 // a quarter of what has been taken
#endif
constexpr size_t kSpanRecycleMin = VESTA_ALLOC_SPAN_RECYCLE_MIN;

/// How much may be parked right now.  Grows with the program, never shrinks
/// below the floor.
inline size_t span_recycle_ceiling() noexcept {
    const size_t taken =
        g_chunk_next.load(std::memory_order_relaxed) * kChunkBytes;
    const size_t share = taken >> VESTA_ALLOC_SPAN_RECYCLE_SHIFT;
    return share > kSpanRecycleMin ? share : kSpanRecycleMin;
}

/// Takes a span of exactly @p chunks from the lock-free tier, or nullptr.
inline ChunkHeader *recycle_take(uint32_t chunks) noexcept {
    if (chunks > kMaxSpanChunks) return nullptr;
    ChunkHeader *h = g_span_recycle[chunks].pop();
    if (h != nullptr)
        g_recycle_bytes.fetch_sub(size_t(chunks) * kChunkBytes,
                                  std::memory_order_relaxed);
    return h;
}

/// Set while a deferred sweep is running, so only one thread does it.
std::atomic<uint32_t> g_sweeping;

/// Puts everything parked back into the pool and merges it.  Defined further
/// down, next to the pool; declared here because the deferred policy calls it.
size_t span_sweep() noexcept;

/// Parks a span in the lock-free tier.  False when the caller has to take the
/// slow path instead, where the span gets coalesced.
inline bool recycle_park(ChunkHeader *h) noexcept {
    const uint32_t chunks = h->cls;
    if (chunks > kMaxSpanChunks) return false;
    const size_t bytes = size_t(chunks) * kChunkBytes;
    const size_t ceiling = span_recycle_ceiling();
    size_t held = g_recycle_bytes.load(std::memory_order_relaxed);
    for (;;) {
        if (held + bytes > ceiling) {
#if VESTA_ALLOC_SPAN_POLICY == VESTA_SPAN_DEFERRED
            /* THE DIFFERENCE BETWEEN THE TWO POLICIES IS THIS BRANCH.  The
             * hybrid one gives up and lets this span go down the locked path,
             * so coalescing happens continuously, a span at a time, and the
             * lock is taken for each of them.  The deferred one instead pays
             * for it ALL AT ONCE, here, when the ceiling is hit: one sweep
             * merges everything and the parking is empty again.
             *
             * Same total work, spread differently: rare and lumpy instead of
             * constant and small.  Which one wins depends on whether the
             * program can afford the lump, and that is what the benchmark is
             * for -- guessing would be picking a shape and calling it a
             * result.  Only one thread sweeps; the rest carry on. */
            uint32_t idle = 0;
            if (g_sweeping.compare_exchange_strong(idle, 1u,
                                                   std::memory_order_acquire,
                                                   std::memory_order_relaxed)) {
                span_sweep();
                g_sweeping.store(0, std::memory_order_release);
            }
            held = g_recycle_bytes.load(std::memory_order_relaxed);
            if (held + bytes > ceiling) return false;
            continue;
#else
            return false;
#endif
        }
        if (g_recycle_bytes.compare_exchange_weak(held, held + bytes,
                                                  std::memory_order_relaxed,
                                                  std::memory_order_relaxed))
            break;
    }
    h->extra = 0; // not free-marked: nobody may absorb it while it waits here
    g_span_recycle[chunks].push(h);
    return true;
}
#endif

#if VESTA_ALLOC_SPAN_POLICY == VESTA_SPAN_LOCKFREE
inline void mark_free_span(const ChunkHeader *h) noexcept {
    const size_t i = chunk_index_of(reinterpret_cast<uintptr_t>(h));
    if (i < kMaxChunksTotal)
        g_free_span_map[i >> 6].fetch_or(uint64_t(1) << (i & 63),
                                         std::memory_order_release);
}
inline void clear_free_span(const ChunkHeader *h) noexcept {
    const size_t i = chunk_index_of(reinterpret_cast<uintptr_t>(h));
    if (i < kMaxChunksTotal)
        g_free_span_map[i >> 6].fetch_and(~(uint64_t(1) << (i & 63)),
                                          std::memory_order_acq_rel);
}
inline bool is_marked_free(uintptr_t addr) noexcept {
    const size_t i = chunk_index_of(addr);
    if (i >= kMaxChunksTotal) return false;
    return ((g_free_span_map[i >> 6].load(std::memory_order_acquire) >>
             (i & 63)) &
            1u) != 0;
}

/**
 * @brief Takes the span at @p addr for the caller, or says somebody else did.
 *
 * THE BIT IS THE CLAIM.  There is no separate state word and no lock: the same
 * bit that says "a free span starts here" is what is competed for, and clearing
 * it is what winning means.  Exactly one thread can see it go from set to
 * clear, so exactly one owns the span -- which is the whole safety argument of
 * the lock-free policy.
 *
 * Losing is NORMAL and costs nothing: it means a neighbour got there first, and
 * the caller simply does not coalesce this time.  That is the property that
 * makes all of this possible -- coalescing is allowed to be skipped.
 *
 * @return true if this thread now owns it.
 */
inline bool claim_free_span(uintptr_t addr) noexcept {
    const size_t i = chunk_index_of(addr);
    if (i >= kMaxChunksTotal) return false;
    const uint64_t bit = uint64_t(1) << (i & 63);
    const uint64_t was =
        g_free_span_map[i >> 6].fetch_and(~bit, std::memory_order_acq_rel);
    return (was & bit) != 0;
}

/**
 * @brief The shared pool itself, with no lock anywhere.
 *
 * One stack per chunk count, and a span in it is ALSO marked free -- unlike the
 * parking the other policies use, which hides its spans from coalescing on
 * purpose.  Here they stay visible, because here coalescing is allowed to reach
 * them: it claims the bit, and whoever loses the claim simply does not merge.
 *
 * WHAT A POP HAS TO DO, and why it is two steps.  Coming off the stack is not
 * enough to own a span: a coalescer may have absorbed it a moment ago, and its
 * entry is then a leftover pointing at memory that now belongs to the
 * neighbour.  So the popper claims it too, and a failed claim means exactly
 * that -- drop it and take the next.  Those leftovers are cleaned up by the
 * very pops that trip over them, and none of them holds any memory: the memory
 * went to whoever did the absorbing.
 */
SpanStack g_span_pool[kMaxSpanChunks + 1];

/**
 * @brief Puts @p h into the pool: marked free FIRST, then pushed.
 *
 * The order is not arbitrary.  Pushed first, a popper could take it in the
 * window before the mark, fail its claim, and drop a span that was nobody's
 * leftover -- losing it for good.  Marked first, the worst case is a neighbour
 * absorbing it before the push, and then the push leaves a leftover, which is
 * the case the pop already knows how to handle.
 */
inline void pool_push(ChunkHeader *h) noexcept {
    h->extra = kSpanFree;
    mark_free_span(h);
    g_span_pool[h->cls].push(h);
}

/// Takes a span of exactly @p k chunks out of the pool, or nullptr.
inline ChunkHeader *pool_pop_exact(uint32_t k) noexcept {
    for (;;) {
        ChunkHeader *h = g_span_pool[k].pop();
        if (h == nullptr) return nullptr;
        if (claim_free_span(reinterpret_cast<uintptr_t>(h))) {
            h->extra = 0;
            return h;
        }
        // A leftover: somebody absorbed it.  Drop it and take the next.
    }
}

/**
 * @brief A span of at least @p want chunks, splitting a bigger one if needed.
 *
 * Splitting needs nothing from anybody: the claim already made this span the
 * caller's alone, and the remainder has never been in a list, so putting it
 * back is a plain push.
 */
ChunkHeader *pool_take_lockfree(uint32_t want) noexcept {
    for (uint32_t k = want; k <= kMaxSpanChunks; ++k) {
        ChunkHeader *h = pool_pop_exact(k);
        if (h == nullptr) continue;
        if (k > want) {
            ChunkHeader *r = reinterpret_cast<ChunkHeader *>(
                reinterpret_cast<char *>(h) + size_t(want) * kChunkBytes);
            r->magic = kSpanMagic;
            r->cls = k - want;
            r->owner = h->owner;
            h->cls = want;
            pool_push(r);
        }
        return h;
    }
    return nullptr;
}

/**
 * @brief Returns @p h to the pool, absorbing free neighbours on the way.
 *
 * CLAIM BEFORE READING, and this is the ordering that matters most here.  The
 * obvious shape -- look at the neighbour's size, decide, then take it -- reads
 * a header that another thread may be absorbing at that instant, and once
 * absorbed those bytes are the neighbour's payload and hold whatever a caller
 * wrote.  So the bit is taken FIRST; only then is the header ours to read, and
 * if the size turns out not to fit, the span is simply handed back.
 *
 * @param h           the span, owned by the caller and in no list.
 * @param drop_addr   set to the pages to hand back, or left alone.
 * @param drop_bytes  how many; zero means nothing to give back.
 */
void pool_return_lockfree(ChunkHeader *h, uintptr_t *drop_addr,
                          size_t *drop_bytes) noexcept {
    h->extra = 0;
    for (;;) {
        const uintptr_t next =
            reinterpret_cast<uintptr_t>(h) + size_t(h->cls) * kChunkBytes;
        if (!is_marked_free(next)) break;
        if (!claim_free_span(next)) break; // somebody else got there first
        ChunkHeader *r = reinterpret_cast<ChunkHeader *>(next);
        if (size_t(h->cls) + r->cls > kMaxSpanChunks) {
            pool_push(r); // too big together: give it straight back
            break;
        }
        h->cls += r->cls;
        r->magic = 0; // it is interior memory now, not a header
    }

    if (h->cls > kMaxSpanChunks) {
        const size_t bytes = size_t(h->cls) * kChunkBytes;
        const size_t page = os_page_size();
        if (bytes > page) {
            *drop_addr = reinterpret_cast<uintptr_t>(h) + page;
            *drop_bytes = bytes - page;
        }
        h->magic = 0;
        g_spans_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    pool_push(h);
}
#else
inline void mark_free_span(const ChunkHeader *h) noexcept {
    const size_t i = chunk_index_of(reinterpret_cast<uintptr_t>(h));
    if (i < kMaxChunksTotal) g_free_span_map[i >> 6] |= uint64_t(1) << (i & 63);
}
inline void clear_free_span(const ChunkHeader *h) noexcept {
    const size_t i = chunk_index_of(reinterpret_cast<uintptr_t>(h));
    if (i < kMaxChunksTotal)
        g_free_span_map[i >> 6] &= ~(uint64_t(1) << (i & 63));
}
inline bool is_marked_free(uintptr_t addr) noexcept {
    const size_t i = chunk_index_of(addr);
    if (i >= kMaxChunksTotal) return false;
    return ((g_free_span_map[i >> 6] >> (i & 63)) & 1u) != 0;
}
#endif

void list_insert(ChunkHeader *h) noexcept {
    const uint32_t k = h->cls;
    SpanNode *n = node_of(h);
    n->prev = nullptr;
    n->next = g_span_free[k];
    if (n->next != nullptr) n->next->prev = n;
    g_span_free[k] = n;
    h->extra = kSpanFree;
    mark_free_span(h);
}

void list_remove(ChunkHeader *h) noexcept {
    SpanNode *n = node_of(h);
    if (n->prev != nullptr)
        n->prev->next = n->next;
    else
        g_span_free[h->cls] = n->next;
    if (n->next != nullptr) n->next->prev = n->prev;
    h->extra = 0;
    clear_free_span(h);
}

/// El vecino de la derecha si esta LIBRE, que es lo unico que se puede mirar
/// sin correr riesgos.  nullptr si no lo hay; ver `g_free_span_map`.
ChunkHeader *right_neighbour(ChunkHeader *h) noexcept {
    const uintptr_t next =
        reinterpret_cast<uintptr_t>(h) + size_t(h->cls) * kChunkBytes;
    if (!is_marked_free(next)) return nullptr;
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

/**
 * @brief Un trozo de region ya APARTADO al que todavia le falta la memoria.
 *
 * POR QUE NO SE COMPROMETE EN EL SITIO.  Comprometer paginas es una llamada al
 * sistema, y hacerla con el cerrojo cogido deja al resto de los hilos girando
 * durante toda ella -- que es de donde salia casi todo el coste de este camino
 * cuando se midio.  Se puede sacar fuera por dos razones, y las dos hacen
 * falta:
 *
 *  1. el rango ya es NUESTRO, porque lo aparto el compare-exchange sobre
 *     `g_chunk_next`, asi que nadie mas lo va a repartir;
 *  2. el tramo no esta en ninguna lista de libres, y con `g_free_span_map`
 *     quien fusiona solo mira los marcados libres: su cabecera no la lee nadie
 *     mas que su dueno.
 *
 * Sin la segunda no valdria, porque entonces otro hilo podria estar mirando la
 * cabecera mientras se le anaden trozos.
 */
struct PendingCommit {
    uintptr_t addr = 0; ///< desde donde hay que comprometer
    size_t bytes = 0;   ///< cuanto; cero si no hay nada pendiente
};

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
bool try_extend_span(ChunkHeader *h, uint32_t want,
                     PendingCommit *pend) noexcept {
    // 1. Tragarse vecinos libres mientras no baste.
#if VESTA_ALLOC_SPAN_POLICY == VESTA_SPAN_LOCKFREE
    /* THE SAME CLAIM AS EVERYWHERE ELSE, and it has to be: growing in place is
     * absorbing a neighbour, which is the one operation that reaches out.  Get
     * the bit first, read the header afterwards -- the other way round reads a
     * span that may already be somebody's payload.  Losing the claim just means
     * the buffer gets copied instead of grown, which is correct. */
    while (h->cls < want) {
        const uintptr_t next =
            reinterpret_cast<uintptr_t>(h) + size_t(h->cls) * kChunkBytes;
        if (!is_marked_free(next)) break;
        if (!claim_free_span(next)) break;
        ChunkHeader *r = reinterpret_cast<ChunkHeader *>(next);
        h->cls += r->cls;
        r->magic = 0;
    }
    if (h->cls >= want) {
        // Puede haber sobrado: lo que sobre vuelve al fondo.
        if (h->cls > want) {
            ChunkHeader *r = reinterpret_cast<ChunkHeader *>(
                reinterpret_cast<char *>(h) + size_t(want) * kChunkBytes);
            r->magic = kSpanMagic;
            r->cls = h->cls - want;
            r->owner = h->owner;
            h->cls = want;
            pool_push(r);
        }
        return true;
    }
#else
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
#endif

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
    /* La llamada al sistema NO se hace aqui: se deja apuntada y la hace el
     * llamante con el cerrojo ya soltado, que es tambien quien pone `h->cls`
     * cuando la memoria ya esta.  Ver `PendingCommit`. */
    pend->addr = addr + size_t(h->cls) * kChunkBytes;
    pend->bytes = size_t(want - h->cls) * kChunkBytes;
    return true;
}

/**
 * @brief Aparta @p count trozos del reparto.  0 si no caben.
 *
 * COMPARA-Y-CAMBIA Y NO SUMA-Y-DEVUELVE, y la diferencia no es de estilo.
 * Sumando primero y mirando despues, una peticion que NO cabe deja el cursor
 * adelantado igualmente -- y nadie lo devuelve --.  Con una sola peticion
 * absurda (un `new` de 32 TiB, que es lo que hace una prueba de `bad_alloc`) el
 * cursor se iba mas alla del final y **la region quedaba muerta para el resto
 * del proceso**: a partir de ahi ninguna reserva la conseguia.
 *
 * Y era MUDO, que es lo que lo hacia grave: como el asignador se caia a
 * `std::malloc` cuando no podia servir, el programa seguia funcionando, mas
 * lento y sin usar ya su propio asignador, sin que nada lo dijera.  Se destapo
 * al quitar ese respaldo.
 *
 * Asi el cursor solo avanza cuando lo apartado cabe de verdad.
 */
uintptr_t take_chunks(size_t count) noexcept {
    if (!ensure_region()) return 0;
    const uintptr_t base = g_region_base.load(std::memory_order_relaxed);
    const uintptr_t end = g_region_end.load(std::memory_order_relaxed);
    size_t idx = g_chunk_next.load(std::memory_order_relaxed);
    for (;;) {
        const uintptr_t addr = base + idx * kChunkBytes;
        /* El desbordamiento se mira aparte: con un `count` disparatado,
         * `addr + count * kChunkBytes` da la vuelta y la comparacion diria que
         * si cabe.  Es justo el caso que trae hasta aqui una peticion absurda. */
        if (count > (size_t(-1) / kChunkBytes)) return 0;
        const uintptr_t want = uintptr_t(count) * kChunkBytes;
        if (addr < base || want > end - addr) return 0; // no cabe
        if (g_chunk_next.compare_exchange_weak(idx, idx + count,
                                               std::memory_order_acq_rel,
                                               std::memory_order_relaxed))
            return addr;
        // Otro se adelanto: `idx` ya trae su valor y se vuelve a intentar.
    }
}

// -------------------------------------------------------------------------
//  Blocks the system serves on a reservation of their own
// -------------------------------------------------------------------------

/**
 * @brief One live block that the system served directly.
 *
 * `bytes` is what was ASKED OF THE SYSTEM, not what the caller wanted: that is
 * what has to be handed back -- on ELF releasing takes a length -- and it is
 * also the honest answer to @c host_usable_size, since the rounding up to a
 * page is memory the caller may use.
 */
struct DirectBlock {
    uintptr_t addr;
    size_t bytes;
};

/**
 * @brief The blocks the system is serving right now.
 *
 * DENSE, NOT HASHED.  `g_direct_n` says how many of the front slots are live
 * and removing one moves the last into the hole, so a lookup scans exactly
 * what is live -- 17 entries in a whole build, two cache lines -- and there
 * are no probe chains to keep intact.  A hash over `kDirectSlots` would touch
 * a similar number of lines on a miss and would need tombstones to stay
 * correct; this is both faster where it matters and simpler to be sure of.
 *
 * @par Threads
 * **Requires `g_direct_lock`.**  A lock and not atomics because every operation
 * that reaches this table already costs microseconds in the system call next
 * to it, so what it buys is not worth what lock-free removal from a table
 * would cost in care.
 */
struct alignas(64) DirectLockWord {
    std::atomic<uint32_t> v{0};
    char pad[64 - sizeof(std::atomic<uint32_t>)];
};
DirectLockWord g_direct_lock;
DirectBlock g_direct[kDirectSlots];
uint32_t g_direct_n = 0;

/// How many were served this way, and how many the table had no room for.
std::atomic<uint64_t> g_direct_allocs{0};
std::atomic<uint64_t> g_direct_refused{0};

struct DirectLock {
    DirectLock() noexcept {
        for (;;) {
            if (g_direct_lock.v.exchange(1, std::memory_order_acquire) == 0)
                return;
            while (g_direct_lock.v.load(std::memory_order_relaxed) != 0)
                cpu_relax();
        }
    }
    ~DirectLock() { g_direct_lock.v.store(0, std::memory_order_release); }
};

/// Notes a block the system served.  false when the table is full.
bool direct_register(void *p, size_t bytes) noexcept {
    DirectLock lk;
    if (g_direct_n == kDirectSlots) {
        g_direct_refused.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    g_direct[g_direct_n].addr = reinterpret_cast<uintptr_t>(p);
    g_direct[g_direct_n].bytes = bytes;
    ++g_direct_n;
    return true;
}

/**
 * @brief Takes the block that CONTAINS @p p out of the table.
 *
 * IT PLACES INTERIOR POINTERS, not just the base, and that is not generosity:
 * @c host_alloc_aligned_freeable hands the caller a pointer raised to its
 * alignment INSIDE the block, and promises the ordinary @c host_free will take
 * it.  In the region that works because masking finds the header from anywhere
 * in the chunk; here the same job is this comparison.  Without it that entry
 * would end in the panic for every size past the line -- and only on ELF,
 * where the system does not hand back 64 KiB-aligned addresses and the raising
 * therefore moves the pointer.
 *
 * @param base out: the address the system has to be given back, which is the
 *        block's, not @p p.
 * @return the block's size, or 0 when @p p is inside no block of ours.
 */
size_t direct_take(void *p, void **base) noexcept {
    const uintptr_t a = reinterpret_cast<uintptr_t>(p);
    DirectLock lk;
    for (uint32_t i = 0; i < g_direct_n; ++i) {
        if (a - g_direct[i].addr >= g_direct[i].bytes) continue;
        const size_t bytes = g_direct[i].bytes;
        *base = reinterpret_cast<void *>(g_direct[i].addr);
        // The last one fills the hole, so the front of the table stays dense.
        g_direct[i] = g_direct[--g_direct_n];
        return bytes;
    }
    return 0;
}

/**
 * @brief Serves a request too big for any span by asking the system.
 *
 * @param c the caller's cache, or nullptr; only used to count.
 * @param n the useful bytes wanted.
 * @return the block -- ALREADY ZERO, straight from the system -- or nullptr,
 *         which means "serve it out of the region as before".  Failing here is
 *         never an error: neither the system refusing nor the table being full
 *         costs anything but the older path.
 */
void *alloc_direct(ThreadCache *c, size_t n) noexcept {
    const size_t page = os_page_size();
    // An absurd request must not wrap the rounding up and come out small.
    if (n > size_t(-1) - page) return nullptr;
    const size_t bytes = (n + page - 1) & ~(page - 1);

    void *p = os_alloc(bytes, kOsReadWrite);
    if (p == nullptr) return nullptr;
    if (!direct_register(p, bytes)) {
        /* No room to note it down.  Giving it back and serving from the region
         * is slower but right; keeping a block that free could not recognise
         * would not be. */
        os_free(p, bytes);
        return nullptr;
    }
    g_direct_allocs.fetch_add(1, std::memory_order_relaxed);
    if (c != nullptr) {
        c->stats.large_allocs++;
        c->stats.bytes_reserved += bytes;
    }
    return p;
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

    /* TOO BIG FOR THE POOL: the system serves it on a reservation of its own.
     * Above this line the region can no longer recycle, so every allocation
     * would commit inside the big reservation and every free decommit -- which
     * is 9.4 + 55.2 us at 16 MiB against 0.7 + 0.7.  See `kMaxSpanBytes`.
     *
     * It is asked FIRST because everything below is bookkeeping for spans that
     * cannot serve this size anyway, and it comes back nullptr when the system
     * refuses or the table is full, which lands on exactly the path that ran
     * before this existed. */
    if (__builtin_expect(n > kMaxSpanBytes, 0)) {
        if (void *p = alloc_direct(c, n)) {
            // Straight from the system, so its pages are already zero.
            if (fresh != nullptr) *fresh = true;
            return p;
        }
    }

    const uint32_t chunks = chunks_for(n);
    const size_t bytes = size_t(chunks) * kChunkBytes;
    uintptr_t addr = 0;
    bool reused = false;

    /* Lo primero, lo que este hilo tenga guardado del mismo tamano: eso NO toca
     * el cerrojo ni las listas compartidas, que es de donde sale casi todo el
     * coste de este camino.  Ver `kSpanCacheSlots`. */
    if (c != nullptr && chunks <= kSpanCacheSlots &&
        c->span_cache[chunks - 1] != nullptr) {
        ChunkHeader *h = c->span_cache[chunks - 1];
        /* The next one of the same size, if the thread was holding more than
         * one.  The link is in the span's own node area; see `span_cache`. */
        c->span_cache[chunks - 1] =
            reinterpret_cast<ChunkHeader *>(node_of(h)->next);
        c->span_cache_bytes -= bytes;
        h->magic = kSpanMagic;
        h->cls = chunks;
        h->owner = c->id;
        h->extra = 0;
        c->stats.large_allocs++;
        // Reusado: su memoria ya estaba comprometida y NO viene a cero.
        return reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(h) +
                                        sizeof(ChunkHeader));
    }

    bool needs_commit = false;
#if VESTA_SPAN_HAS_PARKING
    /* The lock-free tier, before anything that synchronises.  An exact-size hit
     * here is the whole point of the non-locked policies: it is the case that
     * used to take the lock on every single large allocation. */
    if (ChunkHeader *r = recycle_take(chunks)) {
        r->magic = kSpanMagic;
        r->cls = chunks;
        r->owner = c != nullptr ? c->id : 0;
        r->extra = 0;
        if (c != nullptr) c->stats.large_allocs++;
        // Recycled: its memory is already committed and does NOT come zeroed.
        return reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(r) +
                                        sizeof(ChunkHeader));
    }
#endif
    if (chunks <= kMaxSpanChunks) {
        /* Del tamano exacto si lo hay, y si no de uno mayor PARTIENDOLO.  Sin
         * esto, un tramo grande libre no puede servir una peticion pequena y
         * hay que ir a por region nueva; ver la nota de `g_span_free`. */
#if VESTA_ALLOC_SPAN_POLICY == VESTA_SPAN_LOCKFREE
        ChunkHeader *h = pool_take_lockfree(chunks);
#else
        SpanLock lk;
        ChunkHeader *h = take_from_free_lists(chunks);
#endif
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
    h->extra = 0;
    if (c != nullptr) {
        c->stats.large_allocs++;
        // Solo se apunta lo que se pide POR PRIMERA VEZ; reusar no compromete
        // memoria nueva y contarlo otra vez inflaria el total.
        if (!reused) c->stats.bytes_reserved += bytes;
    }
    return reinterpret_cast<void *>(addr + sizeof(ChunkHeader));
}

/**
 * @brief Puts a span back into the shared pool, coalescing it.
 *
 * Split out because two callers need exactly this: an ordinary free, and
 * @c host_span_trim draining what the lock-free tier was holding.  What it
 * must NOT do is talk to the operating system: pages to hand back are reported
 * through @p drop_addr / @p drop_bytes and released by the caller once the lock
 * is gone.  Doing it here would stop every other thread for a whole system
 * call, which is where nearly all of this path's cost used to come from --
 * measured, 267 ms against 1,534 with 48 threads.
 *
 * @param h           the span, already out of every list and cache.
 * @param drop_addr   set to the pages to hand back, or left alone.
 * @param drop_bytes  how many; zero means nothing to give back.
 *
 * @par Threads
 * **Requires the span lock.**
 */
void pool_return_locked(ChunkHeader *h, uintptr_t *drop_addr,
                        size_t *drop_bytes) noexcept {
    h->extra = 0;
    coalesce_right(h);

    if (h->cls > kMaxSpanChunks) {
        /* Demasiado grande para guardarlo.  Sus paginas SI vuelven al
         * sistema -- quedarse con el rango y ademas con la memoria seria
         * regalar las dos cosas --, y se cuenta, para que no sea mudo si
         * deja de ser raro. */
        const size_t bytes = size_t(h->cls) * kChunkBytes;
        const size_t page = os_page_size();
        if (bytes > page) {
            *drop_addr = reinterpret_cast<uintptr_t>(h) + page;
            *drop_bytes = bytes - page;
        }
        h->magic = 0; // deja de ser una cabecera: ya no es de nadie
        g_spans_dropped.fetch_add(1, std::memory_order_relaxed);
    } else {
        list_insert(h);
    }
}

/**
 * @brief Puts everything parked back into the pool and merges it.
 *
 * TWO PHASES, AND THE ORDER IS THE WHOLE POINT.  Coalescing only ever looks
 * RIGHT, so returning spans one at a time merges nothing: the left one arrives
 * while its right neighbour is still parked -- invisible, because parked spans
 * are deliberately not marked free -- and by the time the right one arrives
 * there is nothing to its right.  Two adjacent spans would go back to the pool
 * as two, which is precisely what this exists to undo.  So: put them ALL back
 * first, then merge.
 *
 * @return Bytes moved back into the pool.
 */
size_t span_sweep() noexcept {
#if !VESTA_SPAN_HAS_PARKING
    /* Nothing is ever parked here: with the lock, every free already goes
     * straight to the pool; without it, reaching the pool costs nothing to
     * begin with, so there is no reason to keep spans anywhere else. */
    return 0;
#else
    size_t moved = 0;

    // Phase 1: everything out of the parking and into the pool, unmerged.
    for (uint32_t k = 1; k <= kMaxSpanChunks; ++k) {
        for (;;) {
            ChunkHeader *h = g_span_recycle[k].pop();
            if (h == nullptr) break;
            const size_t bytes = size_t(k) * kChunkBytes;
            g_recycle_bytes.fetch_sub(bytes, std::memory_order_relaxed);
            moved += bytes;
            SpanLock lk;
            h->extra = 0;
            list_insert(h);
        }
    }

    /* Phase 2: merge until nothing more merges.  Restarting after each merge is
     * blunt, and correct: the merged span changes size, so it moves to another
     * list and the walk it was on no longer means anything.  This is a
     * between-phases call, not a hot path -- see `host_span_trim`. */
    for (bool merged = true; merged;) {
        merged = false;
        for (uint32_t k = 1; k <= kMaxSpanChunks && !merged; ++k) {
            SpanLock lk;
            for (SpanNode *n = g_span_free[k]; n != nullptr;) {
                ChunkHeader *h = header_of(n);
                const uint32_t before = h->cls;
                coalesce_right(h);
                if (h->cls != before) {
                    /* It grew, so it is in the wrong list now: take it out and
                     * put it back where its new size belongs. */
                    const uint32_t grown = h->cls;
                    h->cls = before;
                    list_remove(h);
                    h->cls = grown;
                    list_insert(h);
                    merged = true;
                    break;
                }
                n = node_of(h)->next;
            }
        }
    }
    return moved;
#endif
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

    /* Si este hilo no tiene guardado uno de ese tamano, se lo queda: soltar y
     * volver a pedir el mismo tamano es el patron de un bufer que se recicla, y
     * asi no pasa por el cerrojo ni una sola vez.  Ver `kSpanCacheSlots`.
     *
     * Se guarda SIN marca de libre: no esta en ninguna lista, asi que nadie mas
     * lo puede absorber ni entregar, y por eso tampoco hace falta el cerrojo
     * para dejarlo aqui. */
    if (c != nullptr && h->cls <= kSpanCacheSlots) {
        const size_t bytes = size_t(h->cls) * kChunkBytes;
        /* THE BUDGET IS IN BYTES, not in slots, so the promise -- "this thread
         * will not sit on more than this much" -- means the same whatever sizes
         * the consumer asks for.  It also gets the trade right on its own: a
         * multi-megabyte span fills it alone and goes back to the shared pool,
         * where somebody else can use it, while small ones pile up cheaply. */
        if (c->span_cache_bytes + bytes <= kSpanCacheBytes) {
            h->extra = 0;
            node_of(h)->next =
                reinterpret_cast<SpanNode *>(c->span_cache[h->cls - 1]);
            c->span_cache[h->cls - 1] = h;
            c->span_cache_bytes += bytes;
            return;
        }
    }

    /* Lo que haya que devolverle al sistema se APUNTA aqui y se hace despues,
     * ya sin el cerrojo.  Un tramo al que se le quita la marca y que no entra
     * en ninguna lista no lo puede encontrar nadie -- ni por las listas ni por
     * `g_free_span_map` --, asi que soltar sus paginas no necesita exclusion.
     * Hacerlo dentro paraba a TODOS los demas durante una llamada al sistema
     * entera, y con tramos de decenas de MiB eso no es un detalle: medido, es
     * la diferencia entre 267 ms y 1.534 con 48 hilos. */
#if VESTA_SPAN_HAS_PARKING
    /* Park it where anybody can pick it up, without a lock.  It is not
     * coalesced while it waits, which is the trade; past the ceiling this
     * returns false and the span goes down the path below, where it is. */
    if (recycle_park(h)) return;
#endif

    uintptr_t drop_addr = 0;
    size_t drop_bytes = 0;
    {
#if VESTA_ALLOC_SPAN_POLICY != VESTA_SPAN_LOCKFREE
        SpanLock lk;
#endif
        /* Se absorbe a los vecinos libres de la derecha ANTES de entrar en
         * ninguna lista.  Asi los trozos vuelven a estar juntos y sirven para
         * lo que venga despues, en vez de quedarse atrapados en la lista de su
         * tamano exacto.
         *
         * UN TRAMO QUE SE GUARDA, SE GUARDA CON SU MEMORIA.  Aqui hubo dos
         * intentos de devolver paginas al soltarlo y los dos se midieron y se
         * retiraron: por tamano, convierte cada par reservar/soltar en tres
         * llamadas al sistema (1.438 ns por operacion frente a 9,6 de
         * `malloc`); por presupuesto, peor todavia y mas dificil de ver -- una
         * fase que suelta 512 tramos de 1 MiB deja sesenta y cuatro pinchados,
         * nadie vuelve a pedir ese tamano, y a partir de ahi CUALQUIER
         * liberacion ve el presupuesto lleno.
         *
         * Lo retenido esta acotado por el pico de tramos libres a la vez, que
         * es memoria que el programa ya llego a tener.  Devolverla, si hace
         * falta, es cosa de una llamada explicita entre fases. */
#if VESTA_ALLOC_SPAN_POLICY == VESTA_SPAN_LOCKFREE
        pool_return_lockfree(h, &drop_addr, &drop_bytes);
#else
        pool_return_locked(h, &drop_addr, &drop_bytes);
#endif
    }

    if (drop_bytes != 0)
        os_decommit(reinterpret_cast<void *>(drop_addr), drop_bytes);
}

/// Recoge de un golpe lo que otros hilos soltaron de esta clase.
void *take_remote(ThreadCache *c, uint32_t k) noexcept {
    void *head =
        g_remote[c->id].head[k].exchange(nullptr, std::memory_order_acq_rel);
    return head;
}

/**
 * @brief Parte un trozo GRANDE en bloques de la clase @p k.
 *
 * Identico a `grow` salvo en de donde sale la memoria y en el tamano del trozo,
 * y esa segunda diferencia es toda la ganancia: en 64 KiB una clase de 16 KiB
 * saca TRES bloques y deja el 25% muerto; en 1 MiB saca sesenta y tres y deja
 * el 1,56%.  El bloque no cruza ningun limite porque el trozo grande ES la
 * unidad de mascara: no hay nada dentro que cruzar.
 */
void *grow_big(ThreadCache *c, uint32_t k) noexcept {
    const size_t slot = kSizes[k];
    const size_t total = (kBigChunkBytes - sizeof(ChunkHeader)) / slot;

    /* Se sigue con el trozo que este hilo ya tenia abierto para esta clase, si
     * le queda algo.  Estrenar uno por cada recarga desperdiciaria justo lo que
     * el trozo grande viene a evitar. */
    ChunkHeader *h = c->big_run[k];
    if (h == nullptr || h->extra >= total) {
        const uintptr_t addr = take_big_chunks(1);
        if (addr == 0) return nullptr;
        /* Solo la cabecera de momento: lo demas se compromete segun se
         * entrega.  Ver mas abajo por que. */
        if (!os_commit(reinterpret_cast<void *>(addr), sizeof(ChunkHeader)))
            return nullptr;
        h = reinterpret_cast<ChunkHeader *>(addr);
        /* La MISMA marca que un trozo pequeno: lo que distingue a los dos no es
         * la cabecera sino de que region viene el puntero, y eso ya se sabe
         * antes de llegar a mirarla.  Con marcas distintas habria que comprobar
         * las dos en el camino de liberar, que es justo lo que se evita. */
        h->magic = kChunkMagic;
        h->cls = k;
        h->owner = c->id;
        h->extra = 0; // cuantos bloques van entregados de este trozo
        c->big_run[k] = h;
        c->stats.chunks++;
    }

    /* SE ENCADENA POR TANDAS, no el trozo entero.  Encadenar escribe un puntero
     * dentro de CADA bloque, asi que encadenar 1 MiB lo toca entero y lo
     * compromete entero: medido, una sola reserva de 16 KiB dejaba 759 KiB
     * residentes, doce veces lo que costaba antes.  Con una tanda del tamano de
     * un trozo pequeno se compromete lo mismo que antes y el desperdicio del
     * final se sigue repartiendo entre todo el trozo grande, que es para lo que
     * el trozo es grande. */
    const size_t per_batch = kChunkBytes / slot > 0 ? kChunkBytes / slot : 1;
    const size_t done = h->extra;
    size_t take = total - done;
    if (take > per_batch) take = per_batch;

    const uintptr_t base = reinterpret_cast<uintptr_t>(h) + sizeof(ChunkHeader);
    const uintptr_t first = base + done * slot;
    if (!os_commit(reinterpret_cast<void *>(first), take * slot))
        return nullptr;

    // Al reves, para que el primero que se entregue sea el de menor direccion.
    void *head = nullptr;
    for (size_t i = take; i-- > 0;) {
        void *b = reinterpret_cast<void *>(first + i * slot);
        *reinterpret_cast<void **>(b) = head;
        head = b;
    }
    h->extra = uint32_t(done + take);
    c->stats.bytes_reserved += take * slot;
    return head;
}

/// Pide un trozo nuevo y lo parte en bloques de la clase @p k.
void *grow(ThreadCache *c, uint32_t k) noexcept {
    /* Las clases grandes salen de su propia region.  Si esa no se pudo montar,
     * se sigue por el camino de siempre: se desperdicia, pero funciona. */
    if (kSizes[k] >= kBigClassMin) {
        void *p = grow_big(c, k);
        if (p != nullptr) return p;
    }
    /* POR `take_chunks` Y NO CON UN `fetch_add` AQUI.  Era la misma cuenta
     * escrita dos veces, y la copia de aqui tenia el mismo fallo que la otra:
     * sumaba primero y miraba despues, asi que cada intento fallido dejaba el
     * cursor un trozo mas adelante sin devolverlo.  Un hecho, un productor. */
    const uintptr_t addr = take_chunks(1);
    if (addr == 0) return nullptr; // region agotada
    if (!os_commit(reinterpret_cast<void *>(addr), kChunkBytes)) return nullptr;
    ChunkHeader *h = reinterpret_cast<ChunkHeader *>(addr);
    h->magic = kChunkMagic;
    h->cls = k;
    h->owner = c->id;
    h->extra = 0;

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
// veces.  El reparto no es estetico: ver la nota de `util/alloc/host_allocator.h`.

namespace detail {

/// Declarada aqui porque los caminos lentos de abajo la usan antes de que este
/// definida.  No devuelve: ver su definicion.
[[gnu::cold, noreturn]] void no_fallback(const char *why, size_t n) noexcept;

void record_size(ThreadCache *c, size_t n) noexcept {
    // Reparto de tamanos pedidos.  Sirve para UNA pregunta concreta: si lo que
    // se pide cae fuera de las clases, subir el tope da mas de lo que cuesta;
    // si no, no.  El reparto en si lo hace `util::bucket_of`, que es el mismo
    // que usan el apuntado por sitio y el exportador.
    c->stats.size_hist[util::bucket_of(n)]++;
}

ThreadCache *ensure_cache() noexcept {
    if (!allocator_active()) return nullptr;
    return cache();
}

/* Its helpers -- the free-id map and the exit callback -- live up in the
 * anonymous namespace next to `cache()`, which is the same thing this does for
 * the shared policy.  Only this one is exported, because it is the only one the
 * inline fast path in the header has to reach. */
ThreadCache *per_thread_cache_slow() noexcept {
    if (!g_per_thread_slot.ensure()) return nullptr;
    const uint32_t id = take_per_thread_id();
    if (__builtin_expect(id == kNoCacheId, 0)) return nullptr;
    ThreadCache *c = &g_caches[id];
    c->id = id;
    c->used = true;
    c->tag = 0; // the previous owner's purpose is not ours; see `cache()`
    /* Ask to be told BEFORE storing the value: it is `set` that arms the
     * notification, so the other way round this thread would die without
     * returning its id. */
    g_per_thread_slot.notify_on_exit(&on_per_thread_exit);
    g_per_thread_slot.set(c);
    return c;
}

/**
 * @brief Llena el lote caliente desde la lista de la clase @p k.
 *
 * Corre una vez cada @c kBatchSlots reservas de la misma clase, o al cambiar de
 * clase.  Aqui SI se paga la cadena de punteros -- se recorre la lista para
 * recoger hasta ocho bloques --, y ese es justo el trato: se paga una vez por
 * tirada en vez de en cada reserva.  Ver @c kBatchSlots.
 *
 * @param c El cache del hilo.
 * @param k La clase que se pide.
 * @return Un bloque, o nullptr si la lista tambien estaba vacia -- y entonces
 *         el que llama va a @c host_alloc_refill, que es quien pide mas.
 */
void *pop_block_refill(ThreadCache *c, uint32_t k) noexcept {
    /* LAS CLASES GRANDES NO USAN EL LOTE.  Ver `kBatchMaxClass`: ahi el lote no
     * rompe ninguna cadena y solo anade trabajo.  Se sirve de la lista tal cual,
     * que es lo que hacia esta funcion antes de que el lote existiera.
     *
     * Y por eso `batch_cls` nunca llega a valer una clase grande, que es lo que
     * permite que el camino rapido no tenga que preguntarlo. */
    if (k > kBatchMaxClass) {
        void *p = c->free_list[k];
        if (p == nullptr) return nullptr;
        c->free_list[k] = *reinterpret_cast<void **>(p);
        c->stats.by_tag[c->tag]++;
        return p;
    }

    /* De otra clase: lo que quede vuelve a SU lista antes de nada.  Si no, esos
     * bloques quedarian retenidos en el lote de una clase que ya nadie pide, y
     * un bloque retenido es un bloque perdido mientras el hilo viva. */
    if (c->batch_cls != k) {
        while (c->batch_n != 0) {
            void *q = c->batch[--c->batch_n];
            *reinterpret_cast<void **>(q) = c->free_list[c->batch_cls];
            c->free_list[c->batch_cls] = q;
        }
        c->batch_cls = k;
    }

    // Y a llenarlo, recorriendo la lista una vez.
    void *p = c->free_list[k];
    while (p != nullptr && c->batch_n < kBatchSlots) {
        void *next = *reinterpret_cast<void **>(p);
        c->batch[c->batch_n++] = p;
        p = next;
    }
    c->free_list[k] = p;

    if (c->batch_n == 0) return nullptr; // ni lote ni lista: hay que pedir mas
    void *r = c->batch[--c->batch_n];
    c->stats.by_tag[c->tag]++;
    return r;
}

void *host_alloc_refill(ThreadCache *c, uint32_t k, size_t n) noexcept {
    void *chain = take_remote(c, k);
    if (chain == nullptr) chain = grow(c, k);
    if (chain == nullptr) {
        /* NO HAY SITIO.  Antes se caia al sistema; eso es rendirse en silencio
         * y ya no se hace.  Pero tampoco se para el proceso: quedarse sin
         * memoria es una condicion que el lenguaje tiene CONTRATADA, y el
         * contrato lo cumple quien llamo -- `operator new` lanza `bad_alloc`,
         * su forma `nothrow` devuelve nulo y la capa en C devuelve NULL --.
         * Abortar aqui le quitaria al programa la posibilidad de manejarlo. */
        g_gave_up.fetch_add(1, std::memory_order_relaxed);
        // return std::malloc(n);
        return nullptr;
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
        // Sin sitio: nulo, y que el llamante cumpla el contrato.  Ver la nota
        // en `host_alloc_refill`.
        g_gave_up.fetch_add(1, std::memory_order_relaxed);
        // return std::malloc(n);
        return nullptr;
    }
    const uint32_t k = class_of(n);
    void *p = pop_block(c, k);
    if (p != nullptr) return p;
    return host_alloc_refill(c, k, n);
}

/**
 * @brief No hay respaldo: si el asignador no puede servir, se para el proceso.
 *
 * POR QUE UN PANICO Y NO `std::malloc`.  Porque caer al sistema no es una
 * decision, es una rendicion, y ademas es MUDA: el programa sigue, va mas
 * lento, y las cuentas del asignador dejan de cuadrar con la realidad sin que
 * nada lo diga.  Es exactamente el modo de fallo que este proyecto persigue en
 * todas partes -- un valor por defecto que "funciona" convierte un error en un
 * resultado equivocado --.
 *
 * Y ademas se descubrio midiendo: en el arranque, `operator new` entraba 16.421
 * veces y el asignador solo contaba 2.720, con un unico trozo pedido y todos
 * los contadores de rendicion a cero.  O sea que trece mil reservas salian por
 * una puerta que no dejaba rastro.  Con esto, esa puerta grita y dice cual es.
 *
 * QUEDARSE SIN MEMORIA ES OTRA COSA, y no pasa por aqui.  Ahi no se para el
 * proceso: se devuelve nulo y el contrato lo cumple quien llamo -- `operator
 * new` lanza `bad_alloc`, su forma `nothrow` devuelve nulo, y la capa en C
 * devuelve NULL --.  Abortar seria quitarle al programa la posibilidad de
 * manejar una condicion que el lenguaje tiene prevista.
 *
 * POR QUE LAS LINEAS DEL RESPALDO SIGUEN AHI, COMENTADAS.  Porque valen para
 * depurar: volver a ponerlas convierte un fallo duro en el respaldo silencioso
 * de antes, y eso es justo lo que hace falta para separar "el asignador no
 * puede" de "el que llama pide mal".  No son codigo muerto olvidado; estan a un
 * caracter de distancia a proposito.
 */
[[gnu::noinline, gnu::cold, noreturn]] void no_fallback(const char *why,
                                                        size_t n) noexcept {
    std::fprintf(stderr,
                 "[allocator] PANIC: cannot serve %llu bytes and there is NO "
                 "fallback: %s\n"
                 "            Falling back to the system allocator here would "
                 "be silent: the program would keep going, slower, and every "
                 "figure this allocator reports would stop matching reality.\n",
                 (unsigned long long)n, why);
    std::fflush(stderr);
    std::abort();
}

[[gnu::noinline, gnu::cold]] void no_foreign_free(void *p) noexcept {
    std::fprintf(stderr,
                 "[allocator] PANIC: free() of %p, which this allocator never "
                 "handed out.\n"
                 "            Nothing falls back to the system when "
                 "allocating, so there can be no system blocks to release: a "
                 "pointer arriving here means somebody allocated through a "
                 "door we are not watching.  Passing it to free() would work "
                 "and would hide exactly that.\n",
                 p);
    std::fflush(stderr);
    std::abort();
}

/* Cubre a la vez el alta del hilo, el asignador apagado, la reserva vacia, la
 * reserva grande y el desbordamiento de hilos.  Son casos distintos pero todos
 * raros, y juntarlos deja el camino rapido con una sola comparacion para los
 * cinco. */
void *host_alloc_slow(size_t n) noexcept {
    if (!allocator_active())
        // return std::malloc(n);
        no_fallback("the allocator is not active yet (this runs during static "
                    "initialisation, before it has decided)",
                    n);
    if (n == 0) n = 1;
    ThreadCache *c = cache();
    if (c != nullptr && g_measure) record_size(c, n);
    if (n > kMaxSmall) {
        /* Grande: la sirve un TRAMO de trozos de la region.  Ya no se le pide
         * al asignador del sistema: una llamada por reserva costaba ~1,13 us
         * por par, contra los ~4 ns que cuesta aqui. */
        void *p = alloc_span(c, n);
        if (p != nullptr) return p;
        /* La region no dio.  Antes se cedia al sistema; ahora nulo, y el
         * llamante cumple el contrato.  Ver la nota en `host_alloc_refill`. */
        g_gave_up.fetch_add(1, std::memory_order_relaxed);
        // return std::malloc(n);
        return nullptr;
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

void host_free_big(void *p) noexcept {
    /* La misma decision que el camino pequeno, con la otra mascara.  Esta
     * FUERA de linea a proposito: son el 0,9% de las liberaciones, y meterlo en
     * la cabecera engordaria el camino que si es caliente. */
    ChunkHeader *h = big_chunk_of(p);
    if (h->magic != kChunkMagic) {
        host_free_not_small(p, h);
        return;
    }
    ThreadCache *c = current_cache();
    if (have_cache(c) && h->owner == c->id) {
        push_block(c, p, h->cls);
        return;
    }
    host_free_remote(p, h);
}

void host_free_outside(void *p) noexcept {
    void *base = nullptr;
    const size_t bytes = direct_take(p, &base);
    /* Not in the table, so it is not one of ours by any door: the process
     * stops, exactly as it did before this path existed.  The lookup is what
     * tells the two apart, and it happens only here -- on the cold branch that
     * was already about to end the program. */
    if (bytes == 0) no_foreign_free(p);
    ThreadCache *c = cache();
    if (c != nullptr) c->stats.large_frees++;
    // The BLOCK's address, which is not @p p when the caller was handed an
    // aligned pointer inside it.
    os_free(base, bytes);
}

size_t direct_bytes(const void *p) noexcept {
    const uintptr_t a = reinterpret_cast<uintptr_t>(p);
    DirectLock lk;
    for (uint32_t i = 0; i < g_direct_n; ++i) {
        const uintptr_t off = a - g_direct[i].addr;
        if (off >= g_direct[i].bytes) continue;
        /* FROM @p p, not from the start of the block -- the same rule as the
         * span path, and for the same reason: with a pointer raised to an
         * alignment, answering with the whole size would promise bytes that are
         * behind it, and whoever believed it would write past the end. */
        return g_direct[i].bytes - size_t(off);
    }
    return 0;
}

void host_free_remote(void *p, ChunkHeader *h) noexcept {
    // De otro hilo: a su pila, sin bloquear a nadie.
    std::atomic<void *> &head = g_remote[h->owner].head[h->cls];
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

size_t host_region_reserved() noexcept {
    return g_region_reserved.load(std::memory_order_relaxed);
}

size_t host_span_trim() noexcept { return span_sweep(); }

uint64_t host_per_thread_exhausted() noexcept {
    return g_no_per_thread_id.load(std::memory_order_relaxed);
}

uint64_t host_direct_allocs() noexcept {
    return g_direct_allocs.load(std::memory_order_relaxed);
}

uint64_t host_direct_refused() noexcept {
    return g_direct_refused.load(std::memory_order_relaxed);
}

uint64_t host_new_calls() noexcept {
    uint64_t t = 0;
    for (uint32_t i = 0; i <= kMaxThreads; ++i)
        t += g_new_calls[i].n;
    return t;
}

HostAllocStats host_alloc_stats() {
    /* Con llaves: la estructura ya no lleva inicializadores de miembro -- ver la
     * nota en la cabecera --, asi que una copia en la pila hay que pedirla a
     * cero explicitamente.  Sin ellas se sumaria sobre basura. */
    HostAllocStats t{};
    /* The WHOLE table, both policies.  Stopping at `kMaxThreads` would leave
     * out every cache handed out by `PerThreadAllocator`, and the figures would
     * lie by omission -- which is worse than missing, because they would still
     * look right. */
    for (uint32_t i = 0; i < kTotalCaches; ++i) {
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
    /* Esta no se lleva por hilo -- quien la incrementa es justamente el que NO
     * consiguio hilo --, asi que sale del contador global. */
    t.no_owner_id = g_no_cache_id.load(std::memory_order_relaxed);
    return t;
}

bool host_alloc_active() {
    return allocator_active();
}

/* Un almacen propio se pide igual que el de un hilo -- mismo array, mismo
 * contador --, y por eso todo lo demas funciona sin ningun caso especial: su id
 * va en la cabecera de cada trozo, asi que las liberaciones desde otro hilo y
 * las estadisticas lo tratan como a cualquier otro. */
SingleOwnerAllocator::SingleOwnerAllocator() noexcept
    : cache_(nullptr), base_() {
    if (!allocator_active()) return;
    const uint32_t id = take_cache_id();
    if (id == kNoCacheId) return; // sin almacen: se ira por el general
    ThreadCache *c = &g_caches[id];
    c->id = id;
    c->used = true;
    c->tag = 0; // la etiqueta del dueno anterior no es la nuestra
    /* Y la linea de partida, por el mismo motivo que la etiqueta: lo que el
     * dueno anterior contara no es nuestro.  Se RESTA en `stats` en vez de
     * borrarse aqui, porque las cifras globales se forman sumando estos mismos
     * contadores.  Ver `base_`. */
    base_ = c->stats;
    cache_ = c;
}

SingleOwnerAllocator::~SingleOwnerAllocator() noexcept {
    if (cache_ != nullptr) give_cache_id(cache_->id);
}

HostAllocStats SingleOwnerAllocator::stats() const noexcept {
    /* Sin almacen propio no hay nada suyo que contar: todo a cero, que es la
     * respuesta correcta, no una inventada. */
    if (cache_ == nullptr) return HostAllocStats{};
    HostAllocStats s = cache_->stats;
    /* Menos lo que ya habia cuando este almacen cogio el cache.  Ver `base_`:
     * los identificadores se reciclan, asi que lo de aqui puede no ser todo
     * nuestro.  Los campos son los MISMOS que suma `host_alloc_stats`; si
     * aparece uno nuevo hay que restarlo tambien, o este almacen empezara
     * contando lo del anterior en ese campo y solo en ese. */
    for (uint32_t g = 0; g < AllocTag::kSlots; ++g)
        s.by_tag[g] -= base_.by_tag[g];
    s.small_frees -= base_.small_frees;
    s.remote_frees -= base_.remote_frees;
    s.large_allocs -= base_.large_allocs;
    s.large_frees -= base_.large_frees;
    s.chunks -= base_.chunks;
    s.bytes_reserved -= base_.bytes_reserved;
    for (uint32_t b = 0; b < kSizeBuckets; ++b)
        s.size_hist[b] -= base_.size_hist[b];
    // El total no se lleva en el camino caliente: ES la suma del reparto.
    for (uint32_t g = 0; g < AllocTag::kSlots; ++g)
        s.small_allocs += s.by_tag[g];
    return s;
}

size_t host_usable_size(const void *p) noexcept {
    if (p == nullptr) return 0;
    if (!in_region(p)) {
        // De la region grande: misma cuenta, otra mascara.
        if (in_big_region(p)) {
            const ChunkHeader *bh = big_chunk_of(const_cast<void *>(p));
            return bh->magic == kChunkMagic ? kSizes[bh->cls] : 0;
        }
        /* Fuera de las dos: puede ser uno de los que sirve el sistema en
         * reserva propia, y esos si tienen tamano que dar.  Cero cuando no lo
         * es, que es lo mismo que se contestaba antes. */
        return detail::direct_bytes(p);
    }
    const ChunkHeader *h = chunk_of(const_cast<void *>(p));
    if (h->magic == kChunkMagic) return kSizes[h->cls];
    if (h->magic == kSpanMagic)
        /* DESDE @p p, no desde el principio del tramo.  Casi siempre son lo
         * mismo -- un tramo se entrega en `trozo + 16` --, pero no cuando el
         * puntero viene de `host_alloc_aligned_freeable`, que lo sube hasta la
         * alineacion dentro del primer trozo.  Restando la cabecera fija se
         * diria que hay `alineacion - 16` bytes de mas de los que hay, y quien
         * se lo creyera escribiria fuera.  El caso normal da lo mismo que
         * antes: la resta vale 16. */
        return size_t(h->cls) * kChunkBytes -
               (reinterpret_cast<uintptr_t>(p) - reinterpret_cast<uintptr_t>(h));
    return 0; // marca desconocida: no inventamos un tamano
}

void *host_realloc(void *p, size_t n) noexcept {
    if (p == nullptr) return host_alloc(n);
    if (n == 0) {
        host_free(p);
        return nullptr;
    }
    if (!in_region(p)) {
        if (in_big_region(p)) {
            /* De la region grande.  No se estira en su sitio -- son bloques de
             * clase, no tramos --, asi que se copia y se suelta, que es lo que
             * hace el camino general un poco mas abajo. */
            const size_t old_big = host_usable_size(p);
            if (old_big >= n) return p;
            void *q = host_alloc(n);
            if (q == nullptr) return nullptr;
            vesta_memcpy(q, p, old_big < n ? old_big : n);
            host_free(p);
            return q;
        }
        /* Servido por el sistema en reserva propia.  No se estira en su sitio:
         * crecer una reserva propia es pedir el rango de al lado, y eso el
         * sistema no lo promete.  Se copia y se suelta, como la region grande.
         */
        if (const size_t old_direct = detail::direct_bytes(p)) {
            if (old_direct >= n) return p;
            void *q = host_alloc(n);
            if (q == nullptr) return nullptr;
            vesta_memcpy(q, p, old_direct);
            host_free(p);
            return q;
        }
        /* No es nuestro.  Antes se le pasaba a `std::realloc`; ahora no, por lo
         * mismo que en `no_foreign_free`: si nadie cae al sistema al reservar,
         * un bloque ajeno aqui no es un caso a cubrir, es la prueba de que
         * alguien reservo por otra puerta. */
        // return std::realloc(p, n);
        detail::no_foreign_free(p);
    }

    const size_t old = host_usable_size(p);
    if (old >= n) return p; // ya cabe; el redondeo a clase juega a favor

    /* Estirar en su sitio si es un tramo y esta al final de lo repartido.  Es
     * lo que evita la copia en un bufer que crece; ver la cabecera. */
    ChunkHeader *h = chunk_of(p);
    if (h->magic == kSpanMagic) {
        /* Cuantos trozos hacen falta CONTANDO desde el principio del tramo.
         * `chunks_for` cuenta desde la cabecera, y eso solo coincide cuando el
         * puntero esta pegado a ella; con uno subido por alineacion se pediria
         * de menos y el estiron dejaria el bloque corto.  Ver
         * `host_alloc_aligned_freeable`. */
        const size_t off =
            reinterpret_cast<uintptr_t>(p) - reinterpret_cast<uintptr_t>(h);
        const uint32_t want = uint32_t((off + n + kChunkBytes - 1) / kChunkBytes);
        if (want > h->cls) {
            // CON EL CERROJO: estirar toca las listas de libres al absorber al
            // vecino, y sin el dos hilos creciendo a la vez las corromperian.
            // Lo que NO va dentro es comprometer las paginas; ver
            // `PendingCommit`.
            PendingCommit pend;
            bool ok;
            {
#if VESTA_ALLOC_SPAN_POLICY != VESTA_SPAN_LOCKFREE
                SpanLock lk;
#endif
                ok = try_extend_span(h, want, &pend);
            }
            if (ok && pend.bytes != 0) {
                // Ya sin cerrojo: aqui es donde se llama al sistema.
                if (os_commit(reinterpret_cast<void *>(pend.addr), pend.bytes))
                    h->cls = want;
                else
                    ok = false; // se queda el rango, pero no la memoria
            }
            if (ok) return p;
        }
    }

    void *fresh = host_alloc(n);
    if (fresh == nullptr) return nullptr; // `p` sigue valido, como manda
    vesta_memcpy(fresh, p, old < n ? old : n);
    host_free(p);
    return fresh;
}

/**
 * @brief Lets the SYSTEM zero @p n bytes at @p p, instead of writing them.
 *
 * Hands the pages back and takes them again: what comes back is zeroed, and the
 * cost follows what the caller touches rather than how big the block is.  Only
 * from @c kLazyZeroMin upwards, which is measured and per system -- see there
 * for the numbers and for why the threshold is not lower.
 *
 * WHAT IT CANNOT TOUCH.  The span's header lives just before @p p and shares
 * its first page, so that page is written by hand and only whole pages after it
 * are handed back.  Giving away the header would lose the span.
 *
 * THREE ANSWERS AND NOT TWO, and the third is the one that matters.  If the
 * pages go and do not come back, "it did not do it" would be a lie with teeth:
 * the caller would reach for `memset` and fault on memory that is no longer
 * there.  That case has to say so, so the allocation can fail the way running
 * out of memory fails -- which the language has contracted for -- instead of
 * killing the program somewhere else.
 *
 * @code
 *   switch (lazy_zero(p, n)) {
 *   case LazyZero::kDone:   break;                      // the system did it
 *   case LazyZero::kSkip:   zero_aligned(p, n); break;
 *   case LazyZero::kBroken: return nullptr;             // out of memory
 *   }
 * @endcode
 */
/**
 * @brief Zeroes @p n bytes at @p p, SAYING that they are aligned.
 *
 * `vesta_memset` takes a `void *` and a byte count and nothing else, so it has
 * to assume the worst and open with a prologue that walks the pointer up to
 * alignment.  That prologue is not free: it is what stops the fill being
 * unrolled -- a 256-byte one went from straight line to 97 instructions with
 * five branches because of it.
 *
 * Everything this allocator hands out is aligned to @c kAlign, so the prologue
 * is dead work every single time.  Saying so is the whole difference, and it is
 * what the typed C++ entry point (`vesta_memfill`) does for a type; here there
 * is no type, only a byte count, so the alignment goes in by hand.
 */
[[gnu::always_inline]] inline void zero_aligned(void *p, size_t n) noexcept {
    vesta_mem_fill_dispatch(p, 0, n, 1, kAlign);
}

enum class LazyZero { kDone, kSkip, kBroken };

LazyZero lazy_zero(void *p, size_t n) noexcept {
    if (n < kLazyZeroMin) return LazyZero::kSkip;
    const size_t page = os_page_size();
    const uintptr_t start = reinterpret_cast<uintptr_t>(p);
    const uintptr_t end = start + n;
    /* The first whole page of ours, and the last: what is outside them is a
     * scrap that shares a page with somebody else -- the header before, the
     * rest of the span after -- and is written by hand. */
    const uintptr_t first = (start + page - 1) & ~uintptr_t(page - 1);
    const uintptr_t last = end & ~uintptr_t(page - 1);
    if (last <= first) return LazyZero::kSkip; // not even one whole page

    if (!os_decommit(reinterpret_cast<void *>(first), size_t(last - first)))
        return LazyZero::kSkip; // nothing was given away: zero it as always
    if (!os_commit(reinterpret_cast<void *>(first), size_t(last - first)))
        return LazyZero::kBroken; // they went and did not come back

    /* The scraps at both ends, which the system did not zero.  The head starts
     * at `p`, which IS aligned; the tail starts on a page boundary, which is
     * more aligned still. */
    zero_aligned(p, size_t(first - start));
    zero_aligned(reinterpret_cast<void *>(last), size_t(end - last));
    return LazyZero::kDone;
}

void *host_alloc_zeroed(size_t n) noexcept {
    /* Solo los TRAMOS pueden venir limpios del sistema, y solo si son nuevos.
     * Todo lo demas sale de una lista de libres con lo que dejara el inquilino
     * anterior, asi que hay que limpiarlo.  El motivo de que esto no sea
     * `host_alloc` + `memset`, con la medida, esta en la cabecera. */
    if (n > kMaxSmall && allocator_active()) {
        ThreadCache *c = detail::current_cache();
        if (c == nullptr) c = detail::ensure_cache();
        /* `have_cache` y no un nulo: un hilo pasado su aviso de fin lleva la
         * marca, y `ensure_cache` no se la quita -- ni debe. */
        if (detail::have_cache(c)) {
            bool already_zero = false;
            void *p = alloc_span(c, n, &already_zero);
            if (p != nullptr) {
                if (already_zero) return p; // recien del sistema: ya viene a cero
                switch (lazy_zero(p, n)) {
                case LazyZero::kDone:
                    return p;
                case LazyZero::kSkip:
                    zero_aligned(p, n);
                    return p;
                case LazyZero::kBroken:
                    /* Las paginas se fueron y no volvieron.  Sin memoria es una
                     * condicion CONTRATADA -- `operator new` lanza, su forma
                     * `nothrow` devuelve nulo, la capa en C devuelve NULL --,
                     * asi que se dice y lo cumple quien llamo.  Devolver el
                     * bloque como si nada seria entregar memoria que no esta. */
                    return nullptr;
                }
            }
        }
    }
    void *p = host_alloc(n);
    if (p != nullptr) zero_aligned(p, n);
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
                     "  | COULD NOT SERVE: %llu  <- the region ran short and "
                     "these were REFUSED (operator new threw, or the nothrow "
                     "form returned null).  Nothing went to the system",
                     (unsigned long long)gave_up);
    std::fprintf(stderr, "\n");
    /* Lo que sirvio el sistema por su cuenta.  Se enseña porque es una via
     * DISTINTA -- esos bloques no salen de la region ni aparecen en lo
     * comprometido de arriba --, y con ella la vez que la tabla se quedo corta,
     * que es la unica forma de que este camino se apague sin decirlo. */
    const uint64_t direct = g_direct_allocs.load(std::memory_order_relaxed);
    if (direct != 0) {
        std::fprintf(stderr,
                     "[allocator] served by the SYSTEM directly: %llu  <- over "
                     "%zu MiB, where the region can no longer recycle",
                     (unsigned long long)direct,
                     (size_t)(kMaxSpanBytes / (1024 * 1024)));
        const uint64_t refused =
            g_direct_refused.load(std::memory_order_relaxed);
        if (refused != 0)
            std::fprintf(stderr,
                         "  | %llu WENT BACK to the region: no room left in "
                         "the table of %u",
                         (unsigned long long)refused, kDirectSlots);
        std::fprintf(stderr, "\n");
    }
    /* Y la otra cota que degradaba callando: quedarse sin identificador manda
     * a ese hilo a las listas compartidas, detras del unico cerrojo. */
    const uint64_t no_id = g_no_cache_id.load(std::memory_order_relaxed);
    if (no_id != 0)
        std::fprintf(stderr,
                     "[allocator] OUT OF OWNER IDS %llu times  <- more than "
                     "%u live owners; those threads were served from the "
                     "SHARED lists, behind the lock\n",
                     (unsigned long long)no_id, kMaxThreads - 1);
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
        /* Y DE DONDE sale lo que no lo dijo.  El porcentaje de arriba dice
         * cuanto falta; esta lista dice por donde empezar, que es otra
         * pregunta.  Ver `util/report/alloc_sites.h`. */
        dump_alloc_sites(30);
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

/**
 * @brief Apunta de donde vino esta reserva Y CON QUE PROPOSITO.
 *
 * AQUI Y NO DENTRO DE `host_alloc`: desde alli el llamante es SIEMPRE
 * `operator new` y el dato no vale para nada.  Tiene que ser en la frontera.
 *
 * SE APUNTA TODO, CON SU ETIQUETA, y esto es lo que hace que el informe sirva
 * para lo que este asignador existe.  Antes se filtraba a lo que llegara SIN
 * declarar -- para que el coste menguara segun se fuera migrando --, y el
 * efecto era que la columna de proposito solo podia decir "no se": por
 * construccion no habia nada mas que ensenar.  Guardando el par
 * (sitio, proposito) se ve que declara cada funcion, se puede CONTRASTAR con la
 * forma y la vida medidas de ese mismo sitio, y lo que no declara nada sigue
 * saliendo como "no se", que es la lista de lo que falta.
 *
 * Y solo cuando se ha pedido medir.  La CAPTURA de la direccion es gratis --
 * medido: 1,529 ns sin ella y 1,544 con ella, que es ruido --, pero apuntarla
 * en una tabla es una medida, y una medida que nadie pidio no se paga.  La
 * bandera es la misma que ya mira el camino rapido para el reparto de tamanos,
 * asi que la comparacion no es nueva.
 */
[[gnu::noinline, gnu::cold]] void *new_measured(size_t n,
                                                const void *ret) noexcept {
    void *p = util::host_alloc(n);
    const util::detail::ThreadCache *c = util::detail::current_cache();
    util::detail::note_new_call(c);
    util::record_alloc_site(ret, n,
                            util::detail::have_cache(c) ? c->tag : 0);

    return p;
}

/**
 * @brief Reserva, y si se ha pedido medir apunta ademas de donde vino.
 *
 * TODO LO MEDIDO VIVE EN LA RAMA FRIA, y esto no es estilo: la primera version
 * llamaba a `host_alloc` y apuntaba DESPUES, y eso obliga a que el tamano y la
 * direccion de retorno sobrevivan a la llamada.  En el desensamblado se veia:
 * `operator new` pasaba de 100 a 119 instrucciones y ganaba DOS `push`/`pop` de
 * mas en el prologo -- que se pagan siempre, tambien con la medida apagada --.
 * Medido, +3,3% en el camino de `operator new`.
 *
 * Sacando la rama entera fuera, el camino de siempre queda como estaba: nada
 * tiene que sobrevivir a nada porque despues de reservar ya no se usa el
 * tamano.  Lo unico que queda es la comparacion, que es una lectura de un byte
 * que ya esta en cache y una rama que nunca se toma.
 */
[[gnu::always_inline]] inline void *new_or_measure(size_t n,
                                                   const void *ret) noexcept {
    if (__builtin_expect(util::detail::g_measure, 0))
        return new_measured(n, ret);
    return util::host_alloc(n);
}

/* THE FOUR `operator new`, AND WHY THEY SAY NOTHING ABOUT MEASURING.
 *
 * What is here is the usual path and nothing else: allocate and, if there is
 * none, fail.  Not one extra instruction -- not even the one that used to read
 * the return address.
 *
 * When measuring is requested, `call_site.cpp` PATCHES the entry of these four
 * with a jump to an assembly thunk that takes the return address out of `[rsp]`
 * and carries on into `vesta_alloc_new_from`.  Which is why the measurement is
 * nowhere to be seen here: it costs nothing when off because it literally IS
 * NOT THERE.
 *
 * Why patch instead of asking here: asking costs a load and a branch on every
 * allocation, and this is the path the whole file exists to look after -- a
 * difference of that order already cost a measured 3.3%.
 *
 * Why a thunk and not `__builtin_return_address(0)`, which is what this used to
 * be: it is reliable for zero, but it is a value the compiler hands you, and
 * for N > 0 GCC itself says it "may have unpredictable effects, including
 * crashing the calling program".  With a prologue of our own, the calling
 * convention is enough.
 *
 * The OVER-ALIGNED variants further down are patched too. */
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

/* And the other side of the patch: the same, but recording where it came from.
 *
 * `new` and `new[]` share a body -- they do the same thing -- so the four
 * patched symbols come in through two functions. */
extern "C" void *vesta_alloc_new_from(size_t n, const void *ret) {
    void *p = new_or_measure(n, ret);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}

extern "C" void *vesta_alloc_new_nothrow_from(size_t n, const std::nothrow_t &,
                                              const void *ret) noexcept {
    return new_or_measure(n, ret);
}

/* AND THE OVER-ALIGNED ONES, which until now recorded NOTHING.
 *
 * They did not show up in the report as "undeclared": they did not show up at
 * all.  A missing site leaves no gap behind, so everything aligned to a cache
 * line -- which in this project is not a little -- was invisible.  A list of
 * who allocates that is missing allocations is not an incomplete list, it is a
 * wrong one.
 *
 * The SIZE histogram did see them, because `host_alloc_aligned` ends up in
 * `host_alloc`; what was missing was the site. */
[[gnu::noinline, gnu::cold]] static void *
new_aligned_measured(size_t n, size_t a, const void *ret) noexcept {
    void *p = util::host_alloc_aligned(n, a);
    const util::detail::ThreadCache *c = util::detail::current_cache();
    util::detail::note_new_call(c);
    util::record_alloc_site(ret, n,
                            util::detail::have_cache(c) ? c->tag : 0);
    return p;
}

extern "C" void *vesta_alloc_new_aligned_from(size_t n, size_t a,
                                              const void *ret) {
    void *p = new_aligned_measured(n, a, ret);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}

extern "C" void *
vesta_alloc_new_aligned_nothrow_from(size_t n, size_t a, const std::nothrow_t &,
                                     const void *ret) noexcept {
    return new_aligned_measured(n, a, ret);
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

// =========================================================================
//  operator new / delete SOBRE-ALINEADOS (C++17)
// =========================================================================
//
// Un tipo con `alignas` mayor que la alineacion natural de `operator new` no
// usa los operadores de arriba: el compilador emite las sobrecargas con
// `std::align_val_t`.  Si no se sustituyen, TODO tipo sobre-alineado se escapa
// del asignador -- que es justo lo que este fichero existe para impedir --.
//
// Y no es solo que se escape.  Reemplazar unos si y otros no deja al proceso
// con DOS asignadores a la vez, y basta con que un objeto se reserve por un
// camino y se suelte por el otro para corromper el monton.  El sintoma no es
// un error: es una violacion de segmento en otro sitio y mucho despues.
//
// Se sirven desde el MISMO asignador -- @c util::host_alloc_aligned --, no
// delegando en el del sistema, que es lo que hace que la promesa de este
// fichero siga siendo cierta para todo tipo.

void *operator new(size_t n, std::align_val_t a) {
    void *p = util::host_alloc_aligned(n, (size_t)a);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void *operator new[](size_t n, std::align_val_t a) {
    void *p = util::host_alloc_aligned(n, (size_t)a);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void *operator new(size_t n, std::align_val_t a,
                   const std::nothrow_t &) noexcept {
    return util::host_alloc_aligned(n, (size_t)a);
}
void *operator new[](size_t n, std::align_val_t a,
                     const std::nothrow_t &) noexcept {
    return util::host_alloc_aligned(n, (size_t)a);
}

void operator delete(void *p, std::align_val_t) noexcept {
    util::host_free_aligned(p);
}
void operator delete[](void *p, std::align_val_t) noexcept {
    util::host_free_aligned(p);
}
void operator delete(void *p, size_t, std::align_val_t) noexcept {
    util::host_free_aligned(p);
}
void operator delete[](void *p, size_t, std::align_val_t) noexcept {
    util::host_free_aligned(p);
}
void operator delete(void *p, std::align_val_t,
                     const std::nothrow_t &) noexcept {
    util::host_free_aligned(p);
}
void operator delete[](void *p, std::align_val_t,
                       const std::nothrow_t &) noexcept {
    util::host_free_aligned(p);
}
