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
/*
 * SIN INICIALIZADORES DE MIEMBRO AQUI, Y ESTO NO ES UN DESCUIDO.
 *
 * Poner `= 0` o `= {}` en los campos hace que la estructura deje de tener
 * construccion TRIVIAL, y eso se propaga: `ThreadCache` la contiene, asi que
 * `g_caches` deja de ser un array que el cargador pone a cero y pasa a tener un
 * INICIALIZADOR DINAMICO que corre entre los constructores globales.
 *
 * Y ahi esta la trampa, porque el asignador se usa ANTES.  El estandar no
 * ordena los constructores de unidades distintas: cualquier global de otro
 * fichero que reserve arranca el asignador, que se monta y empieza a servir.
 * Cuando por fin le toca el turno a ESTE fichero, su inicializador hace un
 * `rep stos` sobre `g_caches` y **borra lo que el asignador ya tenia**: las
 * listas libres con sus bloques, el identificador, la marca de usado, los
 * tramos guardados y las cuentas.
 *
 * Medido en el compilador: 13.702 reservas atendidas y contadas, y de golpe
 * todo a cero.  Los bloques que colgaban de esas listas se pierden -- nadie los
 * vuelve a ver -- y el hilo sigue apuntando a un cache recien borrado.  No
 * fallaba nada: el programa seguia, reservando de nuevo lo perdido, y las
 * cifras que publicaba el asignador contaban desde ese punto como si el
 * arranque no hubiera existido.
 *
 * Con la estructura trivial, `g_caches` vive en `.bss`, lo pone a cero el
 * cargador antes de que corra una sola instruccion, y no hay ningun momento en
 * el que alguien pueda borrarlo.  Quien necesite una copia a cero en la pila
 * que la pida: `HostAllocStats t{};`.
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
    uint64_t by_tag[AllocTag::kSlots];
    uint64_t small_allocs;       ///< suma de @c by_tag; la rellena el que pide
    uint64_t small_frees;        ///< liberaciones del propio hilo
    uint64_t remote_frees;       ///< liberaciones hechas por OTRO hilo
    uint64_t large_allocs;       ///< reservas grandes, servidas por tramos
    uint64_t large_frees;        ///< tramos devueltos
    uint64_t chunks;             ///< trozos pedidos a la region
    uint64_t bytes_reserved;     ///< bytes comprometidos de la region
    /**
     * @brief Veces que se pidio identificador de dueno y no quedaba.
     *
     * Distinto de cero significa que hubo mas de @c kMaxThreads-1 duenos VIVOS
     * a la vez, y que esos hilos se sirvieron de las listas COMPARTIDAS, detras
     * del unico cerrojo del asignador.  No es un fallo -- funciona igual --,
     * pero es la unica forma de enterarse: desde fuera solo se nota que todo va
     * mas lento sin razon visible.
     *
     * No se lleva por hilo: quien la incrementa es precisamente el que no
     * consiguio uno.
     */
    uint64_t no_owner_id;
    /// Reparto de los tamanos PEDIDOS.  Los tramos exactos estan en
    /// `kBucketLimit`, en el `.cpp`; llegan hasta arriba porque hay que poder
    /// ver la COLA para decidir como servir las reservas grandes.  Solo se
    /// llena con VESTA_HOST_ALLOC_STATS=1.
    uint64_t size_hist[12];
};

namespace detail {

/**
 * @brief Listas libres de un hilo.  Todo POD: se inicializa a cero sin codigo.
 *
 * No es API.  Esta en la cabecera unicamente porque el camino rapido la toca y
 * tiene que poder estar en linea; ver la nota del principio del fichero.
 */
struct alignas(64) ThreadCache {
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
    /**
     * @brief El trozo GRANDE que este hilo esta gastando, por clase.
     *
     * Un trozo grande es de 1 MiB, y encadenar su lista libre escribe un
     * puntero en CADA bloque -- o sea que lo toca entero y lo compromete
     * entero --.  Medido: una sola reserva de 16 KiB dejaba 759 KiB residentes,
     * doce veces lo de antes.  Por eso se encadena por TANDAS: se compromete y
     * se encadena lo que se va a entregar, y aqui se recuerda de que trozo
     * seguir.  El desperdicio se sigue amortizando sobre el 1 MiB entero, que
     * es la razon de que el trozo sea grande.
     *
     * Nulo mientras esa clase no se haya tocado, que es lo normal: casi ninguna
     * las usa todas.
     */
    ChunkHeader *big_run[kClasses];
    /**
     * @brief Un tramo guardado por este hilo, por numero de trozos.
     *
     * El indice es `trozos - 1`.  Nulo si no hay ninguno guardado, que es lo
     * normal en un hilo que no pida reservas grandes.  Ver `kSpanCacheSlots`:
     * existe porque el cerrojo de tramos era el 76% de lo que costaba una
     * reserva grande, y era el unico sitio donde el camino rapido sincronizaba.
     */
    ChunkHeader *span_cache[kSpanCacheSlots];
    HostAllocStats stats;
};

/**
 * EL GUARDIAN DE LO ANTERIOR, y no es una formalidad: esto ya se rompio.
 *
 * Mientras estas dos sean de construccion TRIVIAL, el array de caches vive en
 * `.bss` y lo pone a cero el cargador, antes de que corra una sola instruccion
 * del programa.  En cuanto alguien le pone un `= 0` a un campo, dejan de serlo
 * y el array pasa a tener un inicializador DINAMICO que corre entre los
 * constructores globales -- y el asignador ya esta sirviendo para entonces,
 * porque los globales de otras unidades reservan y el estandar no ordena entre
 * unidades --.  Resultado medido: 13.702 reservas atendidas y luego un
 * `rep stos` que se lleva las listas libres, los identificadores y las cuentas.
 *
 * No fallaba nada.  Se perdian los bloques que colgaban de esas listas y las
 * cifras contaban desde ahi como si el arranque no hubiera existido.  Por eso
 * la comprobacion es de COMPILACION: un test tendria que depender del orden de
 * inicializacion entre unidades, que es justo lo que no esta definido.
 */
static_assert(__is_trivially_constructible(HostAllocStats),
              "HostAllocStats must be constructible without running code: "
              "otherwise the cache array is initialised AFTER the allocator is "
              "already in use, and wipes what it had");
static_assert(__is_trivially_constructible(ThreadCache),
              "ThreadCache must be constructible without running code: see the "
              "note above, this has been broken once already");

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
    /* SIN `ensure` aqui, y no es un olvido.  Reservar la ranura es cosa del
     * camino LENTO, que es quien la usa: @c cache lo hace antes de dar de alta.
     * Aqui `ensure` no cambiaba ninguna respuesta -- con la ranura sin reservar,
     * @c get devuelve nulo igual, y con ella reservada pero vacia tambien --,
     * solo anadia una carga atomica mas de la MISMA variable en el camino mas
     * caliente que tiene el compilador.  Y son dos cargas y no una porque son
     * atomicas con orden de adquisicion: el compilador no puede fusionarlas. */
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

/**
 * @brief Suelta un bloque de la region de las clases GRANDES.
 *
 * Fuera de linea porque son el 0,9% de las liberaciones: meterlo en la cabecera
 * engordaria el camino caliente para servir al caso raro.
 */
void host_free_big(void *p) noexcept;

/// Thread slot of the lock-free per-thread policy.  Separate from
/// @c g_cache_slot because a thread may use both allocators at once.
extern ThreadSlot g_per_thread_slot;

/// Registers this thread with the lock-free policy and hands it its own cache.
/// Cold: runs ONCE per thread.  Returns nullptr only when that policy's id pool
/// is exhausted -- there is no shared fallback here, by design.
ThreadCache *per_thread_cache_slow() noexcept;

/// This thread's cache under the lock-free policy, or nullptr the first time.
/// @par Threads
/// Safe: a thread slot is private to the thread that reads it.
[[gnu::always_inline]] inline ThreadCache *per_thread_cache() noexcept {
    return static_cast<ThreadCache *>(g_per_thread_slot.get());
}

/**
 * @brief Para el proceso: ha llegado a soltarse un bloque que no es nuestro.
 *
 * NO DEVUELVE.  Es la otra mitad de no tener respaldo al reservar: si nadie se
 * cae al sistema pidiendo, no puede haber bloques del sistema que soltar, y uno
 * que aparezca es la prueba de que alguien reservo por una puerta que no
 * miramos.  Pasarselo a `free` funcionaria -- y esa es justamente la trampa:
 * el programa seguiria, mas lento, y las cifras del asignador dejarian de
 * describir lo que pasa sin que nada fallara.
 *
 * Fuera de linea y fria: el camino caliente de soltar no la ve.
 */
[[noreturn]] void no_foreign_free(void *p) noexcept;

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
 * @brief Sirve @p n bytes alineados a @p align, con ESTE asignador.
 *
 * @c host_alloc solo garantiza la alineacion natural de `operator new` -- 16
 * bytes --, asi que un tipo con `alignas(32)` o mas no lo puede usar.  Sin esto
 * la unica salida era el asignador del SISTEMA, y entonces el proceso acaba con
 * dos asignadores a la vez: basta con que un objeto se reserve por un camino y
 * se suelte por el otro para corromper el monton, y el sintoma no es un error
 * sino una violacion de segmento en otro sitio y mucho despues.
 *
 * COMO: se pide de mas, se sube el puntero hasta la alineacion pedida y el
 * original se guarda en el hueco de justo antes, que es lo unico que hace falta
 * para poder soltarlo.
 *
 * LO QUE SE PIDE DE MAS SON @p align BYTES, no @p align - 1 + 8, y la
 * diferencia no es cosmetica: los tamanos se redondean a una CLASE, asi que
 * siete bytes de mas pueden costar una clase entera.  Un `alignas(64)` de 64
 * bytes pedia 135 y caia en la clase de 160; pidiendo 128 cae en la de 128.
 *
 * Se puede pedir menos porque @c host_alloc ya entrega 16 alineados en TODOS
 * sus caminos -- los bloques de un trozo salen a `trozo + 16` con paso multiplo
 * de 16, un tramo sale tambien a `trozo + 16`, y el respaldo del sistema
 * garantiza `max_align_t` --.  Con el original ya 16 alineado, la distancia
 * hasta el siguiente multiplo de @p align ESTRICTAMENTE mayor que el es como
 * mucho @p align, y como poco 16: lo primero acota lo que hay que pedir, y lo
 * segundo garantiza que los 8 bytes de la cabecera caben siempre.
 *
 * @param n     Bytes utiles.
 * @param align Alineacion pedida.  Potencia de dos.
 * @return El bloque alineado, o nullptr si no hay memoria.
 *
 * @par Hilos
 * **Segura desde cualquier hilo**, la misma que @c host_alloc: no anade estado.
 */
[[gnu::always_inline]] inline void *host_alloc_aligned(size_t n,
                                                       size_t align) noexcept {
    /* El paso no baja de la alineacion natural del asignador.  Por debajo de
     * 16, la distancia hasta el destino podria quedarse en menos de los 8 bytes
     * que necesita la cabecera y se escribiria ANTES del bloque.  Antes esto no
     * hacia falta y habia una nota diciendo que no se ponia "por si acaso"; con
     * el relleno justo si hace falta, y ademas esta funcion es publica: dar por
     * hecho que solo la llama `operator new` es dar por hecho algo que el
     * compilador no puede comprobar.  Es un `cmov`, y en un camino que solo
     * pisan los tipos sobre-alineados. */
    const size_t step = align < kAlign ? kAlign : align;
    if (__builtin_expect(n > (size_t)-1 - step, 0)) return nullptr;
    void *raw = host_alloc(n + step);
    if (__builtin_expect(raw == nullptr, 0)) return nullptr;
    /* `+ step` antes de truncar es lo que fuerza a subir SIEMPRE al menos una
     * posicion: si `raw` ya estuviera alineado, quedarse donde esta no dejaria
     * sitio para la cabecera. */
    const uintptr_t aligned =
        ((uintptr_t)raw + step) & ~(uintptr_t)(align - 1u);
    ((void **)aligned)[-1] = raw; // el original, para poder soltarlo
    return (void *)aligned;
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
        /* Aqui no llega NUNCA un bloque pequeno, y por eso la region de las
         * clases grandes se pregunta justo aqui y no antes: el camino que si es
         * caliente sigue siendo el mismo que antes de que existiera, sin una
         * comparacion de mas.  Comprobado desensamblando. */
        if (in_big_region(p)) {
            detail::host_free_big(p);
            return;
        }
        /* NO ES NUESTRO, y eso no puede pasar: si nadie cae al sistema al
         * reservar, no hay bloques del sistema que soltar.  Que llegue uno
         * significa que alguien reservo por otra puerta, y devolverselo a
         * `free` lo taparia -- el programa seguiria y las cuentas dejarian de
         * cuadrar sin que nada lo dijera.  Ver `no_fallback`. */
        // std::free(p);
        detail::no_foreign_free(p);
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
 * @brief Devuelve un bloque de @c host_alloc_aligned.
 *
 * NO vale para bloques de @c host_alloc, ni al reves: el puntero que se
 * devolvio no es el que se reservo, y el original vive en el hueco de justo
 * antes.
 *
 * @par Hilos
 * **Segura desde cualquier hilo**, la misma que @c host_free.
 */
[[gnu::always_inline]] inline void host_free_aligned(void *p) noexcept {
    if (p == nullptr) return;
    host_free(((void **)p)[-1]);
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
    /**
     * @brief Pone la etiqueta y guarda la que habia.
     *
     * EL CASO DE SIEMPRE VA EN LINEA.  Un hilo tiene cache desde su primera
     * reserva, asi que lo normal es una lectura de la ranura y dos escrituras,
     * sin ninguna llamada.  `ensure_cache` esta fuera de linea y solo se paga
     * la primera vez de cada hilo.
     *
     * Importa mas de lo que parece porque esto ya no se abre solo una vez por
     * fase: el pool de hilos construye uno por TAREA para que la etiqueta viaje
     * con el trabajo repartido (D11 del plan de reservas).
     */
    explicit AllocScope(AllocTag t) noexcept
        : c_(detail::current_cache()), prev_(0) {
        if (__builtin_expect(c_ == nullptr, 0)) c_ = detail::ensure_cache();
        if (__builtin_expect(c_ != nullptr, 1)) {
            prev_ = c_->tag;
            c_->tag = t.raw();
        }
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

/**
 * @brief Cuanto ESPACIO DE DIRECCIONES tiene reservado la region, en bytes.
 *
 * No es memoria: reservar solo aparta direcciones y no cuesta nada hasta que se
 * COMPROMETE.  Lo comprometido es otra cifra y ya estaba a la vista
 * (@c HostAllocStats::bytes_reserved); esta faltaba, y sin las dos no se puede
 * responder a "cuanto ocupa esto", que es la pregunta que se hace todo el que
 * se lleva la libreria.
 *
 * Cero si la region aun no se ha montado -- se monta en la primera reserva
 * grande --, lo que tambien es una respuesta.
 *
 * @par Hilos
 * Segura desde cualquier hilo.  Es una lectura atomica relajada.
 *
 * @code
 *   std::printf("reservado %.1f MiB, comprometido %.1f MiB\n",
 *               util::host_region_reserved() / (1024.0 * 1024.0),
 *               util::host_alloc_stats().bytes_reserved / (1024.0 * 1024.0));
 * @endcode
 */
size_t host_region_reserved() noexcept;

/**
 * @brief Cuantas veces ha entrado `operator new`.  Cero si no se esta midiendo.
 *
 * POR QUE NO BASTA CON `HostAllocStats`.  Esa cuenta RESERVAS servidas; esta
 * cuenta ENTRADAS por la puerta del lenguaje.  Deberian coincidir, y cuando no
 * coinciden es cuando hace falta saberlo: con una sola de las dos, un descuadre
 * entre la tabla de sitios y el reparto por proposito no se puede atribuir a
 * ninguna de las dos partes, y se acaba deduciendo por eliminacion.
 *
 * Se lleva por hilo y sin atomicos -- una linea de cache por dueno --, asi que
 * no introduce contencion donde no la habia.  La unica cifra aproximada es la
 * de los hilos que se quedaron sin cache propio, que pueden pisarse entre
 * ellos; normalmente es cero.
 *
 * @par Hilos
 * Segura.  Suma lecturas simples; puede ver una cuenta a medio actualizar de un
 * hilo que este reservando en ese instante.
 */
uint64_t host_new_calls() noexcept;

/**
 * @brief Un asignador de UN SOLO DUENO, sin nada que sincronizar.
 *
 * CONVIVE con el general, no lo sustituye, y por eso NO es una opcion de
 * compilacion: eso obligaria a elegir una de las dos para todo el programa, y
 * lo que hace falta es poder mezclarlas.  Los bloques son intercambiables: uno
 * que salga de aqui se puede soltar con @c host_free -- o llegar a
 * `operator delete`, que es lo que pasara si lo suelta un `std::vector` -- y
 * acabara donde debe.
 *
 * QUE SE AHORRA.  En el camino rapido del general no hay ni un atomico: las
 * listas ya son por hilo.  Lo que cuesta no es sincronizar, es LLEGAR al cache,
 * leyendo la ranura del hilo en cada reserva.  Este tiene el puntero, asi que
 * se salta esa lectura y su comprobacion; y al soltar se ahorra ademas comparar
 * el dueno.
 *
 * PARA QUE SIRVE.  Para lo que tiene dueno declarado y no se comparte: una fase
 * de compilacion, un hilo trabajador con su propio almacen, una libreria de
 * terceros que sabemos que se usa desde un hilo.  **Nunca** detras de
 * `operator new`, que sirve a todo el proceso.
 *
 * @par Hilos
 * **NO es segura, y ESE es el punto.**  Un objeto de estos lo usa UN hilo.  Lo
 * que si es seguro es que OTRO hilo suelte un bloque suyo: eso va por la pila
 * atomica de siempre, igual que entre hilos normales.
 *
 * Las reservas GRANDES siguen pasando por el camino comun, que si toma un
 * cerrojo: son una de cada cien mil operaciones y no compensa duplicar la
 * maquinaria de tramos.  Lo que se ahorra aqui es el camino caliente.
 *
 * @code
 *   util::SingleOwnerAllocator local;              // suyo, de este hilo
 *   void *p = local.alloc(64);
 *   local.free(p);
 *   util::host_free(local.alloc(64));       // y esto tambien vale
 * @endcode
 */
class SingleOwnerAllocator {
  public:
    /**
     * @brief Da de alta un almacen propio.
     *
     * Si no quedan, @c valid() sale false y todo se sirve por el camino
     * general -- que funciona igual, solo que pasando por la ranura del hilo.
     * Degradar asi es a proposito: quedarse sin almacen no puede convertirse en
     * un fallo del programa que lo usa.
     */
    SingleOwnerAllocator() noexcept;

    /**
     * @brief Devuelve el identificador al reparto.
     *
     * Lo que quedara dentro NO se libera: son bloques validos de trozos que
     * siguen siendo nuestros, y quien tome el identificador despues los hereda.
     * Sin esto, cada almacen propio que se creara y se destruyera se llevaba un
     * identificador para siempre -- el mismo defecto que tenian los hilos --, y
     * al agotarse los 63 todo el proceso pasa a servirse por las listas
     * compartidas, detras del unico cerrojo.
     */
    ~SingleOwnerAllocator() noexcept;

    SingleOwnerAllocator(const SingleOwnerAllocator &) = delete;
    SingleOwnerAllocator &operator=(const SingleOwnerAllocator &) = delete;

    /// true si consiguio almacen propio.
    bool valid() const noexcept { return cache_ != nullptr; }

    /// Sirve @p n bytes.  Sin leer ninguna ranura de hilo.
    [[gnu::always_inline]] void *alloc(size_t n) noexcept {
        if (cache_ == nullptr || n - 1 >= kMaxSmall) return host_alloc(n);
        if (detail::g_measure) detail::record_size(cache_, n);
        const uint32_t k = class_of(n);
        void *p = detail::pop_block(cache_, k);
        if (p == nullptr) return detail::host_alloc_refill(cache_, k, n);
        return p;
    }

    /**
     * @brief Returns a block.  Works even if somebody else allocated it.
     *
     * IT LOOKS AT BOTH REGIONS, and that is not a detail.  Size classes from
     * `kBigClassMin` upwards live in the BIG region, so `in_region` says no and
     * this used to fall through to the general path -- which re-derives the
     * owner from the THREAD SLOT, not from this allocator.  Because a
     * single-owner id is decoupled from any thread, the two never matched:
     * every 4 KiB free ended up on the remote stack, paying a
     * `compare_exchange`.
     *
     * Measured before touching anything, 20,000 blocks per size: at 64 and
     * 1024 bytes all 20,000 frees were local; at 4096 and 8192, all 20,000
     * were REMOTE.  That is what made the single-owner column 2-3x worse than
     * the shared one from exactly that size upwards.
     *
     * The small path pays nothing: the second question is only asked once the
     * first has already said no, which is precisely where this used to give up.
     *
     * @code
     *   util::SingleOwnerAllocator a;
     *   void *small = a.alloc(64);    // small region
     *   void *big   = a.alloc(4096);  // big region
     *   a.free(big);                  // local push, no atomic
     *   a.free(small);
     * @endcode
     */
    [[gnu::always_inline]] void free(void *p) noexcept {
        if (p == nullptr) return;
        if (cache_ != nullptr) {
            ChunkHeader *h = nullptr;
            if (in_region(p))
                h = chunk_of(p);
            else if (in_big_region(p))
                h = big_chunk_of(p);
            if (h != nullptr && h->magic == kChunkMagic &&
                h->owner == cache_->id) {
                detail::push_block(cache_, p, h->cls);
                return;
            }
        }
        host_free(p); // de otro dueno, o un tramo: por el camino de siempre
    }

    /**
     * @brief Lo que lleva contado ESTE almacen, sin sumar el de nadie mas.
     *
     * Por VALOR y no por referencia a proposito: `small_allocs` no se
     * incrementa en el camino caliente -- el total es la suma de `by_tag`, que
     * es lo que hace que contar por proposito salga gratis --, asi que hay que
     * rellenarlo al pedirlo.  Devolviendo una referencia, ese campo saldria
     * SIEMPRE a cero y nadie se enteraria: un dato que miente en silencio es
     * peor que uno que falta.
     */
    HostAllocStats stats() const noexcept;

  private:
    detail::ThreadCache *cache_;
};

/**
 * @brief Many threads, and NOT ONE LOCK on the small path.
 *
 * WHAT IT IS FOR.  The process-wide allocator bounds MEMORY: it can name at
 * most `kMaxThreads` owners, and a thread that does not get one is served from
 * the shared lists behind a spin lock.  That bound is the right default for
 * something that serves the whole process, but it has a price, and the price
 * was measured: past the point where the threads on the shared path outnumber
 * the cores, the lock stops being a wait and becomes a convoy.  With 20,000
 * allocations per thread on a 24-core machine, CPU time per operation went
 * 46.5 -> 167.4 -> 537.1 ns at 21, 25 and 65 threads on the shared path, doing
 * exactly the same work.
 *
 * This type bounds LATENCY instead.  Every thread gets a cache of its own, so
 * allocating and freeing take no lock ever, and there is no shared fallback to
 * fall into.  What it costs is memory: one cache per live thread, out of a pool
 * of `kPerThreadCaches`.
 *
 * NEITHER IS BETTER.  They bound different things, and that is exactly why both
 * exist rather than one replacing the other.  Pick this one when the thread
 * count is known and latency matters; keep the shared one when threads are
 * unbounded and memory must not grow with them.
 *
 * WHAT IS STILL SHARED.  Only allocations up to `kMaxSmall` are lock-free.
 * Anything larger is a span, and spans go through the general path, which does
 * take the span lock -- rarely, since spans are the uncommon case.
 *
 * @par Threads
 * **Safe from any thread.**  Every thread that calls this gets its own cache on
 * its first allocation and gives it back when it dies.  A block may be freed
 * from a different thread than the one that allocated it: that goes down the
 * general path, exactly like everywhere else in this allocator.
 *
 * @code
 *   // One instance per call site, or none at all: it holds no state.
 *   util::PerThreadAllocator a;
 *   void *p = a.alloc(256);
 *   a.free(p);
 *
 *   // Equivalent, and this is the form that shows it is a choice of TYPE:
 *   void *q = util::PerThreadAllocator::alloc(256);
 *   util::PerThreadAllocator::free(q);
 * @endcode
 */
class PerThreadAllocator {
  public:
    /**
     * @brief Serves @p n bytes without taking any lock.
     *
     * Falls back to the general path in two cases, and both are the uncommon
     * one: a span (larger than `kMaxSmall`), and a pool with no id left.
     */
    [[gnu::always_inline]] static void *alloc(size_t n) noexcept {
        detail::ThreadCache *c = detail::per_thread_cache();
        if (__builtin_expect(c == nullptr, 0)) {
            c = detail::per_thread_cache_slow();
            if (c == nullptr) return host_alloc(n); // pool exhausted
        }
        if (n - 1 >= kMaxSmall) return host_alloc(n); // spans: general path
        if (detail::g_measure) detail::record_size(c, n);
        const uint32_t k = class_of(n);
        void *p = detail::pop_block(c, k);
        if (p == nullptr) return detail::host_alloc_refill(c, k, n);
        return p;
    }

    /**
     * @brief Returns a block.  Works even if another thread allocated it.
     *
     * Looks at BOTH regions before giving up, for the same reason
     * @c SingleOwnerAllocator::free does: size classes from `kBigClassMin`
     * upwards live in the big region, so asking only `in_region` would send
     * every one of those frees down the general path -- and there the owner is
     * re-derived from the OTHER thread slot, which never matches, turning each
     * one into an atomic push.
     */
    [[gnu::always_inline]] static void free(void *p) noexcept {
        if (p == nullptr) return;
        detail::ThreadCache *c = detail::per_thread_cache();
        if (c != nullptr) {
            ChunkHeader *h = nullptr;
            if (in_region(p))
                h = chunk_of(p);
            else if (in_big_region(p))
                h = big_chunk_of(p);
            if (h != nullptr && h->magic == kChunkMagic &&
                h->owner == c->id) {
                detail::push_block(c, p, h->cls);
                return;
            }
        }
        host_free(p); // another owner, or a span: the usual path
    }
};

/**
 * @brief How many times @c PerThreadAllocator ran out of caches.
 *
 * IT HAS TO BE ASKABLE.  This policy has no shared fallback on purpose, so
 * exhausting the pool means threads silently going down the general path --
 * that is, back behind the lock this type exists to avoid.  Non-zero here means
 * `kPerThreadCaches` is too small for the program, and the only symptom
 * otherwise would be "it got slower for no visible reason".
 *
 * @return Times a thread asked for a per-thread cache and none was left.
 *
 * @code
 *   if (util::host_per_thread_exhausted() != 0)
 *       report("more live threads than per-thread caches");
 * @endcode
 */
uint64_t host_per_thread_exhausted() noexcept;

/**
 * @brief Sirve @p n bytes PUESTOS A CERO.
 * @return nullptr solo si tampoco pudo el asignador del sistema.
 *
 * NO es `host_alloc` mas un `memset`, y ahi esta toda la gracia: cuando la
 * memoria acaba de venir del sistema operativo **ya viene a cero** -- lo
 * garantizan tanto Windows como POSIX, porque entregar paginas de otro proceso
 * sin limpiarlas seria una fuga de datos --, asi que volver a ponerla a cero es
 * escribir de balde.
 *
 * Cuanto de balde, medido en Linux contra `calloc` con bloques de 1 MiB:
 *
 *     con memset siempre     176.126 ns
 *     calloc de glibc          7.578 ns
 *
 * Veintitres veces, y no por ser mas lento haciendo lo mismo: por hacer un
 * trabajo que no hacia falta.  Los bloques pequenos SI se limpian, porque salen
 * de una lista de libres y llevan lo que dejara el inquilino anterior.
 *
 * @par Hilos
 * Segura desde cualquier hilo, igual que @c host_alloc.
 *
 * @code
 *   int *v = static_cast<int *>(util::host_alloc_zeroed(n * sizeof(int)));
 * @endcode
 */
void *host_alloc_zeroed(size_t n) noexcept;

/**
 * @brief Bytes UTILIZABLES de @p p, que pueden ser mas de los que se pidieron.
 * @return 0 si @p p no salio de aqui (vino del sistema, o es nulo).
 *
 * El redondeo a clase no es desperdicio si se aprovecha: pedir 40 bytes da 48.
 *
 * @par Hilos
 * Segura desde cualquier hilo.  Solo lee la cabecera del trozo.
 */
size_t host_usable_size(const void *p) noexcept;

/**
 * @brief Cambia el tamano de @p p a @p n bytes.
 * @return nullptr si no se pudo; en ese caso @p p SIGUE SIENDO VALIDO, igual
 *         que manda `realloc`.
 *
 * Evita copiar en dos casos, y el segundo es el que importa:
 *
 *  1. Si ya cabe en lo que tiene, devuelve el mismo puntero.
 *  2. Si es una reserva grande y su tramo es **lo ultimo que se tomo de la
 *     region**, lo ESTIRA en su sitio.  Ese es el patron de un bufer que crece
 *     -- reservar, duplicar, duplicar --, y sin esto cada duplicacion copiaba
 *     el contenido entero: crecer hasta 1 MiB costaba 1.450 ns por paso frente
 *     a los 49 de `realloc`, que usa `mremap` y no copia.
 *
 * `mremap` aqui no vale, y conviene saber por que: moveria el bloque FUERA de
 * nuestra region y `in_region` dejaria de reconocerlo.
 *
 * @par Hilos
 * Segura desde cualquier hilo.  Vale aunque @p p lo reservara otro.
 */
void *host_realloc(void *p, size_t n) noexcept;

/// true si el asignador esta sirviendo (false con `VESTA_NO_HOST_SLAB=1`).
/// @par Hilos
/// Segura.  La primera llamada consulta el entorno; las demas leen un atomico.
bool host_alloc_active();

} // namespace util

#endif // VESTA_UTIL_HOST_ALLOCATOR_H
