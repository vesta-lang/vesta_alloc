/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/host_allocator.h
 * @brief Asignador propio del proceso anfitrion, detras de `operator new`.
 *
 * POR QUE EXISTE.  Al compilar, `malloc`+`free` del sistema son el 18,5% del
 * tiempo (medido con VTune sobre 24k lineas: 3,108 s de 16,795 s) y estan
 * REPARTIDOS -- el mayor sitio suelto es el 8,7% de esa cifra --, asi que
 * ningun arreglo puntual los mueve.  Lo que los mueve es cambiar el asignador,
 * porque afecta a todos los sitios a la vez.
 *
 * Y compensa: medido cara a cara con el `malloc` de msvcrt en el patron del
 * compilador (rafagas de reservas pequenas que se sueltan juntas), el nuestro
 * va a 13,5 ns por operacion frente a 44,3 -- **3,3x**.
 *
 * SIN CERROJOS, POR DISENO.  Poner un mutex para hacerlo hilo-seguro se comeria
 * justo la ventaja que se busca.  En su lugar cada hilo tiene sus propias
 * listas libres y el camino rapido no sincroniza NADA: sacar un bloque son dos
 * lecturas y una escritura sobre memoria del propio hilo.
 *
 * LIBERAR ENTRE HILOS.  Es el caso que hunde a los asignadores por hilo
 * ingenuos: lo que reserva un hilo lo suelta otro.  Aqui no se pierde ni se
 * corrompe.  Todos los trozos salen de UNA region reservada de antemano y
 * alineada, asi que:
 *
 *   - saber si un puntero es nuestro son DOS comparaciones (esta o no en la
 *     region), sin buscar en ninguna tabla ni tomar ningun cerrojo;
 *   - de un puntero se saca su trozo con una mascara, y del trozo su cabecera,
 *     que dice el tamano y QUIEN lo posee.
 *
 * Si el que libera no es el dueno, el bloque va a una pila atomica del dueno
 * (un `compare_exchange`, sin bloquear a nadie) y el dueno la recoge entera de
 * un golpe cuando se queda sin bloques.  Es el modelo de tcmalloc / mimalloc.
 *
 * QUE NO HACE.  Lo grande (por encima de @c kMaxSmall) va al asignador del
 * sistema tal cual: ahi el reparto en clases no aporta y el sistema ya sabe
 * hacerlo.  Tampoco devuelve memoria al sistema; la reusa.
 *
 * Se puede desactivar con `VESTA_NO_HOST_SLAB=1`, que hace que todo vaya al
 * sistema.  No es un escape: sin poder apagarlo no hay con que comparar, que
 * es la unica forma de saber si un asignador mejora algo.
 *
 * ------------------------------------------------------------------------
 * POR QUE EL CAMINO RAPIDO ESTA EN LINEA Y AQUI
 * ------------------------------------------------------------------------
 *
 * Porque no lo estaba, y se notaba.  `operator new` es REEMPLAZABLE, asi que
 * nunca se puede meter dentro de quien reserva; lo que si se puede es que ella
 * no llame a su vez a nadie.  No era el caso: el cuerpo entero vivia fuera y
 * era demasiado grande para que el compilador lo metiera dentro.  Desensamblado
 * del objeto de Release, antes de este cambio:
 *
 *     operator new(unsigned long long):
 *         push   %r12
 *         sub    $0x20,%rsp          <- espacio de sombra de Win64
 *         mov    ...,%eax            <- esto SI se inlinaba
 *         test   %eax,%eax
 *         ...
 *         call   host_alloc          <- y aqui se iba fuera
 *
 * Un marco de llamada completo en cada una de los 62 millones de reservas de
 * una compilacion.  Medido con un banco que imita las dos formas (`-O3`, listas
 * acertando siempre):
 *
 *     llamando fuera (como antes)     2,323 ns
 *     camino rapido dentro            1,529 ns      -34%
 *
 * De ahi el reparto: **en linea solo lo que ocurre casi siempre** -- mirar la
 * clase y sacar el primer bloque de la lista --, y fuera de linea todo lo
 * demas: dar de alta el hilo, recoger lo que soltaron otros, pedir un trozo
 * nuevo y la caida al sistema.  Meter mas dentro no gana nada y engorda cada
 * sitio que reserva.
 *
 * Lo que eso obliga: `ThreadCache` y la tabla de clases tienen que ser
 * VISIBLES.  Estan en @c detail y no son API -- nadie fuera de este fichero y
 * de su `.cpp` debe tocarlas --, y la geometria comun (region, trozos, clases)
 * vive aparte en `util/host_allocator_layout.h` porque tambien la usan la arena
 * y los metadatos de diagnostico.
 *
 * INVARIANTE del que depende el camino rapido: **si un hilo tiene cache, el
 * asignador esta activo y la tabla de clases esta construida.**  Se cumple
 * porque el alta solo ocurre dentro del camino lento, y ese comprueba lo uno y
 * construye lo otro antes.  Gracias a eso el camino rapido no mira ninguna
 * bandera de activacion: le basta con que el cache no sea nulo.
 */
#ifndef VESTA_UTIL_HOST_ALLOCATOR_H
#define VESTA_UTIL_HOST_ALLOCATOR_H

#include "util/alloc_tag.h"
#include "util/host_allocator_layout.h"
#include "util/thread_slot.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace util {

/**
 * @brief Contadores de uso del asignador, para diagnostico.
 *
 * Se llevan POR HILO y se suman al pedirlos, para que contarlos no obligue a
 * sincronizar en el camino rapido.
 */
struct HostAllocStats {
    /**
     * @brief Reservas pequenas, repartidas POR ETIQUETA.
     *
     * Indexado directamente por @c AllocTag::raw, sin descomponer nada: contar
     * por proposito es entonces el MISMO incremento indexado que contaba el
     * total, no uno mas.  Por eso no hay un contador de totales aparte -- seria
     * la suma de esto -- y por eso llevar el reparto no cuesta ni una
     * instruccion extra en el camino caliente.
     */
    uint64_t by_tag[AllocTag::kSlots] = {};
    uint64_t small_allocs = 0;   ///< suma de @c by_tag; la rellena el que pide
    uint64_t small_frees = 0;    ///< liberaciones del propio hilo
    uint64_t remote_frees = 0;   ///< liberaciones hechas por OTRO hilo
    uint64_t large_allocs = 0;   ///< reservas grandes, servidas por tramos
    uint64_t large_frees = 0;    ///< tramos devueltos
    uint64_t chunks = 0;         ///< trozos pedidos a la region
    uint64_t bytes_reserved = 0; ///< bytes comprometidos de la region
    /// Reparto de los tamanos PEDIDOS.  Los tramos exactos estan en
    /// `kBucketLimit`, en el `.cpp`; llegan hasta arriba porque hay que poder
    /// ver la COLA para decidir como servir las reservas grandes.  Solo se
    /// llena con VESTA_HOST_ALLOC_STATS=1.
    uint64_t size_hist[12] = {};
};

namespace detail {

/**
 * @brief Listas libres de un hilo.  Todo POD: se inicializa a cero sin codigo.
 *
 * No es API.  Esta en la cabecera unicamente porque el camino rapido la toca y
 * tiene que poder estar en linea; ver la nota del principio del fichero.
 */
struct ThreadCache {
    void *free_list[kClasses];
    uint32_t id;
    bool used;
    /**
     * @brief La etiqueta que corre AHORA en este hilo, empaquetada.
     *
     * Puesta junto a @c id y @c used a proposito: el camino rapido ya carga esa
     * linea de cache para llegar a las listas, asi que leerla no cuesta ningun
     * acceso nuevo.  Cero es "no se", que es lo que da un POD sin inicializar.
     * La mueve @c AllocScope, nunca se toca a mano.
     */
    uint8_t tag;
    HostAllocStats stats;
};

/// La ranura por hilo donde vive el puntero al cache.  Ver `util/thread_slot.h`.
extern ThreadSlot g_cache_slot;

/// Si se lleva el reparto de tamanos.  Se mira una vez; contar no puede salir
/// gratis, pero NO contar si.
extern bool g_measure;

/**
 * @brief El cache de ESTE hilo, o nullptr si todavia no tiene.
 *
 * Nulo significa tres cosas a la vez -- sin dar de alta, asignador apagado, o
 * mas hilos que ranuras -- y las tres se resuelven igual: por el camino lento.
 * Que una sola comparacion cubra los tres casos es lo que deja el camino rapido
 * sin ninguna bandera que mirar.
 *
 * @par Hilos
 * Segura.  Lee una ranura por hilo, asi que cada hilo ve la suya y no hay nada
 * que compartir.
 */
[[gnu::always_inline]] inline ThreadCache *current_cache() noexcept {
    if (!g_cache_slot.ensure()) return nullptr;
    return static_cast<ThreadCache *>(g_cache_slot.get());
}

/**
 * @brief Saca un bloque de la lista de la clase @p k.  nullptr si esta vacia.
 *
 * Es el NUCLEO, y esta suelto a proposito.  Entre la version de hoy -- un cache
 * por hilo, con liberaciones que pueden venir de otro -- y una version de un
 * solo dueno, sin nada que sincronizar, lo que cambia es COMO se llega al cache
 * y que hace falta al liberar; esto es identico en las dos.  Sacarlo aparte
 * ahora no cuesta ni una instruccion y evita duplicarlo despues.
 *
 * El primer campo de un bloque libre ES el enlace al siguiente: la lista no
 * gasta memoria aparte.
 *
 * @par Hilos
 * **NO es segura, y eso es EL DISEÑO.**  Aqui no hay ni un atomico ni una
 * barrera: sacar un bloque son dos lecturas y una escritura sobre memoria del
 * propio hilo.  Poner sincronizacion se comeria justo la ventaja que se busca
 * -- son 3 ns por operacion --, asi que en vez de proteger la estructura se
 * garantiza que nadie mas la toca.
 *
 * Quien llame tiene que cumplir UNA de estas dos:
 *   - @p c es el cache de ESTE hilo (@c current_cache), o
 *   - @p c es el cache compartido y se tiene su cerrojo (ver `alloc_shared` en
 *     el `.cpp`).
 *
 * Lo que llega de otros hilos NO entra por aqui: va a una pila atomica aparte
 * y su dueno la recoge entera cuando se queda sin bloques.
 */
[[gnu::always_inline]] inline void *pop_block(ThreadCache *c,
                                              uint32_t k) noexcept {
    void *p = c->free_list[k];
    if (p == nullptr) return nullptr;
    c->free_list[k] = *reinterpret_cast<void **>(p);
    // Contar por etiqueta ES contar: el total sale de sumar esta tabla, asi
    // que saber el proposito no anade ni una instruccion.
    c->stats.by_tag[c->tag]++;
    return p;
}

/**
 * @brief Devuelve un bloque a la lista de su clase.  La otra mitad del nucleo.
 *
 * @par Hilos
 * **NO es segura, a proposito.**  Mismas condiciones que @c pop_block.
 */
[[gnu::always_inline]] inline void push_block(ThreadCache *c, void *p,
                                              uint32_t k) noexcept {
    *reinterpret_cast<void **>(p) = c->free_list[k];
    c->free_list[k] = p;
    c->stats.small_frees++;
}

/// Anota el tamano pedido en el reparto.  Fuera de linea: solo corre midiendo.
/// @par Hilos
/// **NO segura**, mismas condiciones que @c pop_block: escribe en @p c.
void record_size(ThreadCache *c, size_t n) noexcept;

/**
 * @brief Da de alta el cache de este hilo si aun no lo tiene.
 *
 * Lo necesita @c AllocScope: un ambito que se abre antes de la primera reserva
 * del hilo no tendria donde dejar la etiqueta, y esa fase entera contaria como
 * "no se" -- que es peor que no medir, porque parece un dato.
 *
 * Fuera de linea porque ocurre una vez por hilo.  Devuelve nullptr si el
 * asignador esta apagado o si ya no quedan ranuras de hilo.
 *
 * @par Hilos
 * Segura.  El identificador se reparte con un `fetch_add`.
 */
ThreadCache *ensure_cache() noexcept;

/// Todo lo que no es sacar un bloque de una lista que ya lo tenia: alta del
/// hilo, asignador apagado, reserva grande, y el cache compartido.
/// @par Hilos
/// **Segura desde cualquier hilo**; resuelve por su cuenta que cache usar.
void *host_alloc_slow(size_t n) noexcept;

/// La lista de esa clase se quedo vacia: recoger lo remoto o pedir un trozo.
/// @par Hilos
/// **NO segura** respecto a @p c: mismas condiciones que @c pop_block.  Lo que
/// SI es seguro es de donde saca los bloques (pila atomica y region).
void *host_alloc_refill(ThreadCache *c, uint32_t k, size_t n) noexcept;

/// Liberar un bloque que no es de este hilo: a la pila atomica de su dueno.
/// @par Hilos
/// **Segura desde cualquier hilo**, con un `compare_exchange` y sin bloquear.
void host_free_remote(void *p, ChunkHeader *h) noexcept;

/// Liberar algo que NO es un bloque de una clase: un tramo grande, o basura.
/// @par Hilos
/// **Segura desde cualquier hilo**; toma el cerrojo de tramos si hace falta.
void host_free_not_small(void *p, ChunkHeader *h) noexcept;

} // namespace detail

/**
 * @brief Sirve @p n bytes con la garantia de alineacion de `operator new`.
 * @return nullptr solo si tampoco pudo el asignador del sistema.
 *
 * @par Hilos
 * **Segura desde cualquier hilo**, y sin sincronizar nada en el camino normal.
 * No es una contradiccion: la seguridad sale de PARTIR el estado -- cada hilo
 * tiene sus listas --, no de proteger uno compartido.  Lo poco que de verdad se
 * comparte esta fuera del camino rapido: la pila de liberaciones ajenas (con un
 * `compare_exchange`), las listas de tramos y el cache de desbordamiento (con
 * cerrojo, y son una de cada cien mil operaciones).
 */
[[gnu::always_inline]] inline void *host_alloc(size_t n) noexcept {
    /* Un solo salto cubre los dos casos raros: con `n == 0` la resta da el
     * mayor sin signo, que tambien cae fuera.  Asi el tamano se valida sin
     * gastar una segunda comparacion en algo que no pasa casi nunca. */
    if (n - 1 >= kMaxSmall) return detail::host_alloc_slow(n);
    detail::ThreadCache *c = detail::current_cache();
    if (c == nullptr) return detail::host_alloc_slow(n);
    if (detail::g_measure) detail::record_size(c, n);
    const uint32_t k = class_of(n);
    void *p = detail::pop_block(c, k);
    if (p == nullptr) return detail::host_alloc_refill(c, k, n);
    return p;
}

/**
 * @brief Devuelve un bloque de @c host_alloc.
 *
 * Vale aunque lo reservara OTRO hilo: es el caso que hunde a los asignadores
 * por hilo ingenuos, y aqui esta resuelto sin cerrojos ni tablas.
 *
 * @par Hilos
 * **Segura desde cualquier hilo.**  Si el bloque es de este hilo, va a su lista
 * sin sincronizar nada.  Si es de otro, va a la pila atomica de su dueno con un
 * `compare_exchange`, sin bloquear a nadie.  Liberar dos veces el mismo bloque
 * NO es seguro -- como en cualquier asignador --, pero desde 2026-09 al menos
 * se DETECTA y se avisa en vez de corromper en silencio.
 */
[[gnu::always_inline]] inline void host_free(void *p) noexcept {
    if (p == nullptr) return;
    if (!in_region(p)) {
        std::free(p);
        return;
    }
    ChunkHeader *h = chunk_of(p);
    if (h->magic != kChunkMagic) {
        // Un tramo (reserva grande), o algo que no deberia estar aqui.  Las dos
        // cosas fuera de linea: ninguna es el caso normal.
        detail::host_free_not_small(p, h);
        return;
    }
    detail::ThreadCache *c = detail::current_cache();
    if (c != nullptr && h->owner == c->id) {
        detail::push_block(c, p, h->cls);
        return;
    }
    detail::host_free_remote(p, h);
}

/**
 * @brief Declara para que es lo que se reserve en este hilo mientras viva.
 *
 * Es el canal AMBIENTAL de la etiqueta, y existe porque hay reservas que no
 * pueden declarar nada por si mismas: un `std::string` no tiene donde decir su
 * proposito, y el codigo de terceros menos aun.  Envolviendo una fase, todo lo
 * que reserve dentro queda etiquetado sin tocar un solo sitio de llamada.
 *
 * Anida: al salir se restaura la etiqueta que hubiera antes.
 *
 * SOLO VALE PARA ESTE HILO.  Si la fase reparte trabajo por el pool, los
 * trabajadores no heredan nada por su cuenta -- lo suyo apareceria como "no
 * se", que es un dato falso, no un dato que falta.  La etiqueta viaja con la
 * tarea; ver `ir/parallel_for.h`.
 *
 * @par Hilos
 * Cada objeto vale para SU hilo y solo para el.  Varios hilos pueden tener el
 * suyo a la vez sin estorbarse -- cada uno escribe en su propio cache --, pero
 * un mismo objeto NO se puede compartir: por eso no es copiable.
 *
 * @code
 *   util::AllocScope fase{{util::AllocUse::Medium, util::AllocShape::Growing}};
 *   // ...todo lo que reserve aqui dentro queda contado como tal
 * @endcode
 */
class AllocScope {
  public:
    explicit AllocScope(AllocTag t) noexcept
        : c_(detail::ensure_cache()), prev_(c_ != nullptr ? c_->tag : 0) {
        if (c_ != nullptr) c_->tag = t.raw();
    }
    ~AllocScope() noexcept {
        if (c_ != nullptr) c_->tag = prev_;
    }

    AllocScope(const AllocScope &) = delete;
    AllocScope &operator=(const AllocScope &) = delete;

    /// La etiqueta que corre en este hilo; "no se" si no hay cache todavia.
    static AllocTag current() noexcept {
        const detail::ThreadCache *c = detail::current_cache();
        return c != nullptr ? AllocTag::from_raw(c->tag) : AllocTag{};
    }

  private:
    detail::ThreadCache *c_;
    uint8_t prev_;
};

/**
 * @brief Suma los contadores de todos los hilos.
 *
 * @par Hilos
 * Se puede llamar desde cualquier hilo, pero **la foto no es coherente**: se
 * recorren los caches sin pararlos, asi que un contador puede cambiar mientras
 * se suma.  Es a proposito -- sincronizar aqui costaria en el camino rapido, y
 * lo que se quiere de estas cifras son ordenes de magnitud --.  Para un numero
 * exacto, mirarlas cuando ya no queden hilos trabajando.
 *
 * @code
 *   // Cuantas reservas quedaron vivas, y cuanto se comprometio por ellas.
 *   const util::HostAllocStats s = util::host_alloc_stats();
 *   const long long vivas = (long long)(s.small_allocs + s.large_allocs) -
 *                           (long long)(s.small_frees + s.remote_frees +
 *                                       s.large_frees);
 *   std::printf("%lld vivas, %.1f MiB comprometidos\n", vivas,
 *               s.bytes_reserved / (1024.0 * 1024.0));
 *
 *   // Y cuanto de eso sigue sin declarar su proposito, que es lo que queda
 *   // por migrar (ver `doc/PLAN_RESERVAS.md`).
 *   const uint64_t sin_declarar = s.by_tag[util::AllocTag{}.raw()];
 * @endcode
 */
HostAllocStats host_alloc_stats();

/// true si el asignador esta sirviendo (false con `VESTA_NO_HOST_SLAB=1`).
/// @par Hilos
/// Segura.  La primera llamada consulta el entorno; las demas leen un atomico.
bool host_alloc_active();

} // namespace util

#endif // VESTA_UTIL_HOST_ALLOCATOR_H
