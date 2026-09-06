/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/thread_slot.h
 * @brief Un puntero por hilo que NO pasa por la TLS emulada de MinGW.
 *
 * POR QUE NO `thread_local`.  En MinGW cada acceso a una variable de hilo es
 * una LLAMADA a `__emutls_get_address`.  Medido en este mismo toolchain:
 *
 *     thread_local (emutls)      10,83 ns por acceso
 *     TlsGetValue                 2,14 ns
 *     lectura directa del TEB      0,85 ns     <- lo que hace esto
 *
 * Doce veces.  Eso descarta `thread_local` en cualquier camino caliente: el
 * asignador que hay encima de esto sirve un bloque en ~13 ns, asi que mirar su
 * estado por la via emulada costaria casi tanto como el trabajo.
 *
 * Y no es solo velocidad.  La TLS emulada ya nos costo un CUELGUE: un
 * `thread_local` con inicializador dinamico genera una variable de guarda que
 * en MinGW se bloquea cuando hay hilos que nacen y mueren -- el proceso se
 * quedaba esperandola para siempre.  Un mecanismo propio quita de en medio esa
 * clase entera de fallo.
 *
 * COMO.  Un array de ranuras por hilo al que se llega con desplazamiento
 * constante desde el registro de segmento del hilo, que es UNA instruccion.  La
 * forma es la misma en los dos sistemas y solo cambia de quien es el array:
 *
 *     Windows x64   el del TEB, que es donde `TlsAlloc` reparte las ranuras
 *                   0..63:  `mov %gs:0x1480(,%rax,8)`
 *     ELF           uno NUESTRO, en TLS de verdad y con modelo `initial-exec`
 *                   para que el desplazamiento lo fije el enlazador:
 *                   `mov %fs:OFFSET(,%rax,8)`
 *
 * Para el resto de casos -- ranura por encima de las directas, otra
 * arquitectura -- se cae a la API del sistema, que sigue siendo cinco veces
 * mejor que la TLS emulada.
 *
 * Ahi fuera `__thread` SI es TLS de verdad, asi que usarlo por dentro no
 * contradice nada de lo de arriba: lo que no se puede usar es la version de
 * MinGW.  Hubo una epoca en que la rama de ELF no lo hacia asi -- una tabla de
 * 256 casillas indexada por un hash del puntero de hilo -- y salia dieciocho
 * veces mas cara y con un modo de fallo propio; esta contado en `g_tls_slots`.
 *
 * SE COMPRUEBA SOLO.  `0x1480` es una interioridad del sistema, no un contrato.
 * Al reservar la ranura se escribe un valor por la API y se lee por la via
 * directa: si no coinciden, la via directa se apaga y todo pasa por la API.
 * Preferimos perder ocho nanosegundos a leer memoria que no nos toca.
 *
 * NO INCLUYE `windows.h`.  Esa cabecera define `VOID` como macro y rompe
 * cualquier `enum class` que use ese nombre -- ya obligo a aislar
 * `ThreadPool.h` en su propio `.cpp`.  Aqui el camino rapido es ensamblador en
 * linea, que no necesita nada, y lo que si necesita la API vive en el `.cpp`.
 *
 * CON QUE COMPARARSE.  El asignador de la biblioteca C de Linux resuelve el
 * mismo problema y sirve de vara de medir: su camino rapido llega a su estado
 * por hilo con `mov <offset>(%rip),%rdx` y `mov %fs:(%rdx),%rcx`, y el resto de
 * la reserva son otras dieciocho instrucciones sin un solo atomico.  Cualquier
 * cosa que se haga aqui tiene que quedar en ese orden de magnitud; si no, el
 * mecanismo propio deja de estar justificado.
 */
#ifndef VESTA_UTIL_THREAD_SLOT_H
#define VESTA_UTIL_THREAD_SLOT_H

#include <atomic>
#include <cstdint>

namespace util {

/// Valor de ranura que significa "todavia no reservada".
constexpr uint32_t kNoThreadSlot = 0xFFFFFFFFu;

/// Lee una ranura por la API del sistema.  Vive en el `.cpp` para no arrastrar
/// `windows.h` (ni `pthread.h`) hasta aqui.
void *thread_slot_get_api(uint32_t slot) noexcept;
/// Escribe una ranura por la API del sistema.
void thread_slot_set_api(uint32_t slot, void *value) noexcept;
/// Reserva una ranura nueva; @c kNoThreadSlot si el sistema no da mas.
uint32_t thread_slot_alloc_api() noexcept;

/// Lo que se avisa cuando un hilo termina, con el valor que tenia en la ranura.
using ThreadExitFn = void (*)(void *value);

/**
 * @brief Reserva un canal por el que el sistema avise de fin de hilo.
 *
 * POR QUE HACE FALTA UN SEGUNDO MECANISMO teniendo el nuestro.  Porque el
 * nuestro no puede darlo: la ranura rapida es una casilla del TEB que se lee
 * con una instruccion, y nadie nos llama cuando un hilo se va.  Solo el sistema
 * sabe eso.
 *
 * Asi que se pide APARTE y SOLO para el aviso: en Windows con @c FlsAlloc, que
 * es @c TlsAlloc con devolucion de llamada.  Y no puede sustituir a la ranura
 * rapida aunque lo parezca -- los valores de FLS NO viven en `gs:0x1480`, sino
 * en otra estructura --, asi que cambiarla por FLS costaria el camino rapido
 * entero.  Fuera de Windows es una clave de @c pthread con destructor, que es
 * exactamente lo mismo, y ahi ni siquiera es una dependencia nueva: la ranura
 * de respaldo ya usa esas claves.
 *
 * @return @c kNoThreadSlot si el sistema no da mas.  Quien lo pida tiene que
 *         seguir funcionando sin aviso: peor, no roto.
 */
uint32_t thread_exit_alloc_api(ThreadExitFn fn) noexcept;

/// Deja en el canal el valor con el que llegara el aviso de ESTE hilo.  Nulo lo
/// cancela: el sistema solo avisa de los valores que no lo son.
void thread_exit_arm_api(uint32_t channel, void *value) noexcept;

namespace detail {

#if !defined(_WIN32) && defined(__ELF__) &&                                    \
    (defined(__GNUC__) || defined(__clang__))
#define VESTA_THREAD_SLOT_TLS_FAST 1

/**
 * @brief Las ranuras de este hilo, en TLS de verdad.
 *
 * ES EL ESPEJO EXACTO DE LA RAMA DE WINDOWS.  Alli las ranuras viven en el
 * array del TEB y se leen con `mov %gs:0x1480(,%rax,8)`; aqui viven en un array
 * NUESTRO cuyo desplazamiento respecto al puntero de hilo lo fija el enlazador,
 * y se leen con `mov %fs:OFFSET(,%rax,8)`.  Una instruccion en los dos sitios,
 * y en ninguno se toca la biblioteca C.
 *
 * POR QUE NO LO ERA ANTES, que es lo que hay que no repetir.  Habia aqui una
 * tabla de 256 casillas indexada por un hash del puntero de hilo, con sondeo
 * lineal.  Costaba dieciocho instrucciones -- dos cargas atomicas de la ranura,
 * una multiplicacion, dos tablas, una comparacion --, y tenia un modo de fallo
 * peor que su coste: un hilo cuya casilla natural ocupara otro NO acertaba
 * nunca en la primera comprobacion, asi que en CADA reserva y CADA liberacion
 * llamaba a la funcion de alta, que antes de encontrar la suya intentaba un
 * `compare_exchange` sobre la casilla ajena -- robandole la linea de cache al
 * hilo que la ocupaba, indefinidamente.  Con 256 casillas eso empieza a pasar
 * hacia los 20 hilos, y medido con 24: el hilo mas lento tardaba 3,2 veces lo
 * que el mas rapido y el coste por operacion subia de 16 a 23 ns, mientras que
 * el asignador del sistema, con los mismos hilos y los mismos nucleos, no se
 * movia.  Con esto, ninguna de las dos cosas existe.
 *
 * MODELO `initial-exec`, que es lo que lo convierte en una instruccion: el
 * desplazamiento se resuelve al enlazar y deja de haber llamada a
 * `__tls_get_addr`.  Es lo mismo que hace la propia glibc para su cache de
 * `malloc`.  Tiene un limite conocido y aceptado: una biblioteca DINAMICA que
 * se cargue con `dlopen` necesita que quede reserva estatica de TLS, y si no
 * queda, la carga FALLA -- ruidosamente, que es como tiene que fallar --.  Son
 * 512 bytes por hilo, y ademas la version dinamica ya tiene sus propios avisos
 * en el README.
 *
 * Sin inicializador: un array de punteros arranca a cero por construccion, asi
 * que no hay variable de guarda ni registro de destruccion -- que es de donde
 * venian los cuelgues de la TLS emulada de MinGW.
 */
constexpr uint32_t kDirectSlots = 64;
extern __thread void *g_tls_slots[kDirectSlots]
    __attribute__((tls_model("initial-exec")));

#endif

/**
 * @brief Si la lectura directa del TEB quedo validada.
 *
 * 0 = sin comprobar todavia, 1 = coincide con la API, 2 = NO coincide.
 *
 * Es una sola bandera para todas las ranuras a proposito: el desplazamiento del
 * TEB o es el que creemos o no lo es; no depende de la ranura.
 *
 * ESTA AQUI Y NO EN EL `.cpp` porque @c ThreadSlot::get la consulta en CADA
 * acceso, y con la comprobacion fuera de linea el camino rapido no era rapido:
 * se ahorraba la llamada a la API del sistema para pagar otra llamada.  No es
 * API publica; se lee por @c thread_slot_direct_ok.
 */
extern std::atomic<int> g_direct_state;

} // namespace detail

/// true si la lectura directa del TEB quedo validada al reservar.  Siempre
/// false fuera de Windows x64.
[[gnu::always_inline]] inline bool thread_slot_direct_ok() noexcept {
    return detail::g_direct_state.load(std::memory_order_acquire) == 1;
}

/**
 * @brief Un puntero por hilo.
 *
 * Se declara como variable global normal (no de hilo): lo que cambia por hilo
 * es el CONTENIDO de la ranura, no el objeto.  Asi no hay ni inicializador
 * dinamico ni variable de guarda, que es de donde venian los cuelgues.
 */
class ThreadSlot {
  public:
    /**
     * @brief Reserva la ranura la primera vez.  Idempotente y entre hilos.
     * @return false si el sistema no pudo dar una ranura.
     *
     * La comprobacion va EN LINEA y solo la reserva de verdad vive en el
     * `.cpp`.  No es un adorno: el cache por hilo del asignador llama aqui en
     * CADA reserva, y con la funcion entera fuera eso era una llamada por
     * `malloc`.  Medido con VTune sobre 144k lineas, `ensure` retiraba 1.466
     * millones de instrucciones -- el cuarto puesto de todo el compilador --
     * para no hacer nada mas que mirar un entero.
     */
    [[gnu::always_inline]] bool ensure() noexcept {
        // Ya reservada, que es el caso de siempre menos la primera vez.
        if (slot_.load(std::memory_order_acquire) != kNoThreadSlot) return true;
        return reserve_slot();
    }

    /// El valor de ESTE hilo, o nullptr si nunca se puso.
    [[gnu::always_inline]] void *get() const noexcept {
        const uint32_t s = slot_.load(std::memory_order_acquire);
        if (s == kNoThreadSlot) return nullptr;
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
        // Camino rapido: la ranura vive en el TEB y se lee con una
        // instruccion.  Solo si quedo validada al reservar (ver la cabecera
        // del fichero) y si cabe en las 64 ranuras directas.
        //
        // El `_WIN32` de la condicion NO sobra: `gs:0x1480` es el TEB de
        // Windows.  En Linux x86-64 `gs` apunta a otra cosa y esto leeria
        // memoria que no es nuestra.
        if (s < 64 && thread_slot_direct_ok()) {
            /* El `volatile` NO sobra, y se comprobo: quitarlo no consigue que
             * `-O2` meta esta lectura dentro del asignador (sigue fuera), y a
             * cambio deja que el compilador la trate como funcion PURA de la
             * ranura.  Siendolo, podria reusar el resultado de un `get` a
             * traves de un `set` -- y el alta del cache por hilo hace
             * exactamente eso: leer, ver nulo, escribir, volver a leer.  En
             * `-O3`, que es lo que usan Release y Profile, se mete dentro con
             * `volatile` puesto. */
            void *v;
            asm volatile("movq %%gs:0x1480(,%1,8), %0"
                         : "=r"(v)
                         : "r"(static_cast<uint64_t>(s)));
            return v;
        }
#elif defined(VESTA_THREAD_SLOT_TLS_FAST)
        /* Camino rapido de Linux, y es la MISMA forma que el de arriba: un
         * acceso indexado a un array por hilo con desplazamiento constante.
         * `mov %fs:OFFSET(,%rax,8)`.  Ver la nota de `g_tls_slots`. */
        if (s < detail::kDirectSlots) return detail::g_tls_slots[s];
#endif
        return thread_slot_get_api(s);
    }

    /**
     * @brief El indice de ranura, para quien tenga que leerla POR SU CUENTA.
     *
     * Lo necesita el codigo generado: un thunk del JIT no puede llamar a
     * `get()`, asi que emite el la lectura del TEB y para eso necesita el
     * indice.  Devuelve @c kNoThreadSlot si aun no se ha reservado.
     */
    uint32_t slot_index() const noexcept {
        return slot_.load(std::memory_order_acquire);
    }

    /// Fija el valor de ESTE hilo.  No hace falta que sea rapido: se llama una
    /// vez por hilo, no en el camino caliente.
    void set(void *v) noexcept {
        const uint32_t s = slot_.load(std::memory_order_acquire);
        if (s == kNoThreadSlot) return;
        bool stored = false;
#if defined(VESTA_THREAD_SLOT_TLS_FAST)
        if (s < detail::kDirectSlots) {
            detail::g_tls_slots[s] = v;
            stored = true;
        }
#endif
        if (!stored) thread_slot_set_api(s, v);

        /* Y el aviso de fin de hilo, si alguien lo pidio.  Una ranura que no lo
         * pidio no paga mas que esta comparacion, y esto corre una vez por
         * hilo, no en el camino caliente. */
        const uint32_t ch = exit_.load(std::memory_order_acquire);
        if (ch != kNoThreadSlot) thread_exit_arm_api(ch, v);
    }

    /**
     * @brief Pide que se avise cuando termine un hilo con valor en esta ranura.
     *
     * Idempotente y entre hilos: el primero que llega reserva el canal y los
     * demas se quedan con el suyo.  A partir de ahi, cada @c set arma el aviso.
     * Hay que pedirlo ANTES del primer @c set del hilo, o ese hilo no avisara.
     *
     * @return false si el sistema no da canal.  Entonces no se avisa de nada,
     *         que es como estaba antes de existir esto -- no es un fallo.
     */
    [[gnu::always_inline]] bool notify_on_exit(ThreadExitFn fn) noexcept {
        if (exit_.load(std::memory_order_acquire) != kNoThreadSlot) return true;
        return reserve_exit(fn);
    }

  private:
    /// La reserva de verdad, una vez en la vida del objeto.  Fuera de linea
    /// para que el camino normal de @c ensure sea una carga y una rama.
    bool reserve_slot() noexcept;
    /// Igual que @c reserve_slot, y por el mismo motivo: pasa una vez.
    [[gnu::cold]] bool reserve_exit(ThreadExitFn fn) noexcept;

    std::atomic<uint32_t> slot_{kNoThreadSlot};
    /// El canal de aviso, o @c kNoThreadSlot si nadie lo pidio.
    std::atomic<uint32_t> exit_{kNoThreadSlot};
};

} // namespace util

#endif // VESTA_UTIL_THREAD_SLOT_H
