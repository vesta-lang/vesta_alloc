/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/os/thread_slot.h
 * @brief
 * \~english One pointer per thread that does NOT go through MinGW's emulated
 *          TLS.
 * \~spanish Un puntero por hilo que NO pasa por la TLS emulada de MinGW.
 * \~
 *
 * \~english
 * WHY NOT `thread_local`.  On MinGW every access to a thread variable is a CALL
 * to `__emutls_get_address`.  Measured on this very toolchain:
 *
 *     thread_local (emutls)      10.83 ns per access
 *     TlsGetValue                 2.14 ns
 *     direct TEB read             0.85 ns     <- what this does
 *
 * Twelve times.  That rules `thread_local` out of any hot path: the allocator
 * on top of this serves a block in ~13 ns, so looking up its state the emulated
 * way would cost almost as much as the work.
 *
 * And it is not only speed.  Emulated TLS has already cost us a HANG: a
 * `thread_local` with a dynamic initialiser generates a guard variable that on
 * MinGW deadlocks when threads are born and die -- the process sat waiting for
 * it forever.  A mechanism of our own removes that whole class of failure.
 *
 * HOW.  An array of per-thread slots reached at a constant offset from the
 * thread's segment register, which is ONE instruction.  The shape is the same
 * on both systems and only the owner of the array changes:
 *
 *     Windows x64   the TEB's, which is where `TlsAlloc` hands out slots
 *                   0..63:  `mov %gs:0x1480(,%rax,8)`
 *     ELF           one of OURS, in real TLS and with the `initial-exec` model
 *                   so that the linker fixes the offset:
 *                   `mov %fs:OFFSET(,%rax,8)`
 *
 * For every other case -- a slot above the direct ones, another architecture --
 * it falls back to the system API, which is still five times better than
 * emulated TLS.
 *
 * Out there `__thread` IS real TLS, so using it inside contradicts none of the
 * above: what cannot be used is MinGW's version.  There was a time when the ELF
 * branch did not do it this way -- a table of 256 cells indexed by a hash of
 * the thread pointer -- and it came out eighteen times dearer and with a
 * failure mode of its own; the story is in `g_tls_slots`.
 *
 * IT CHECKS ITSELF.  `0x1480` is an internal of the system, not a contract.
 * When the slot is reserved a value is written through the API and read back
 * the direct way: if they do not match, the direct way is switched off and
 * everything goes through the API.  We would rather lose eight nanoseconds than
 * read memory that is not ours.
 *
 * IT DOES NOT INCLUDE `windows.h`.  That header defines `VOID` as a macro and
 * breaks any `enum class` using that name -- it already forced `ThreadPool.h`
 * into a `.cpp` of its own.  Here the fast path is inline assembly, which needs
 * nothing, and what the API does need lives in the `.cpp`.
 *
 * WHAT TO MEASURE AGAINST.  Linux's C library allocator solves the same problem
 * and serves as a yardstick: its fast path reaches its per-thread state with
 * `mov <offset>(%rip),%rdx` and `mov %fs:(%rdx),%rcx`, and the rest of the
 * allocation is another eighteen instructions without a single atomic.
 * Whatever is done here has to stay in that order of magnitude; otherwise the
 * mechanism of our own stops being justified.
 *
 * \~spanish
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
 *
 * \~
 */
#ifndef VESTA_UTIL_THREAD_SLOT_H
#define VESTA_UTIL_THREAD_SLOT_H

#include <atomic>
#include <cstdint>

namespace util {

/// \~english The slot value meaning "not reserved yet".
/// \~spanish Valor de ranura que significa "todavia no reservada".
/// \~
constexpr uint32_t kNoThreadSlot = 0xFFFFFFFFu;

/**
 * @brief
 * \~english Reads a slot through the system API.
 * \~spanish Lee una ranura por la API del sistema.
 * \~
 *
 * \~english
 * It lives in the `.cpp` so as not to drag `windows.h` (or `pthread.h`) in
 * here.
 *
 * \~spanish
 * Vive en el `.cpp` para no arrastrar `windows.h` (ni `pthread.h`) hasta aqui.
 *
 * \~
 * @param slot
 * \~english the slot index handed out by @c thread_slot_alloc_api.
 * \~spanish el indice de ranura que reparte @c thread_slot_alloc_api.
 * \~
 * @return
 * \~english this thread's value, or nullptr if it was never set.
 * \~spanish el valor de este hilo, o nullptr si nunca se puso.
 * \~
 */
void *thread_slot_get_api(uint32_t slot) noexcept;

/**
 * @brief
 * \~english Writes a slot through the system API.
 * \~spanish Escribe una ranura por la API del sistema.
 * \~
 * @param slot
 * \~english the slot index.
 * \~spanish el indice de ranura.
 * \~
 * @param value
 * \~english what this thread will read back.
 * \~spanish lo que este hilo leera despues.
 * \~
 */
void thread_slot_set_api(uint32_t slot, void *value) noexcept;

/**
 * @brief
 * \~english Reserves a new slot.
 * \~spanish Reserva una ranura nueva.
 * \~
 * @return
 * \~english the index, or @c kNoThreadSlot if the system gives no more.
 * \~spanish el indice, o @c kNoThreadSlot si el sistema no da mas.
 * \~
 */
uint32_t thread_slot_alloc_api() noexcept;

/// \~english What gets called when a thread ends, with the value it had in the
///           slot.
/// \~spanish Lo que se avisa cuando un hilo termina, con el valor que tenia en
///           la ranura.
/// \~
using ThreadExitFn = void (*)(void *value);

/**
 * @brief
 * \~english Reserves a channel through which the system reports a thread
 *          ending.
 * \~spanish Reserva un canal por el que el sistema avise de fin de hilo.
 * \~
 *
 * \~english
 * WHY A SECOND MECHANISM IS NEEDED given that we have our own.  Because ours
 * cannot provide it: the fast slot is a cell of the TEB read with one
 * instruction, and nobody calls us when a thread goes away.  Only the system
 * knows that.
 *
 * So it is asked for SEPARATELY and ONLY for the notification: on Windows with
 * @c FlsAlloc, which is @c TlsAlloc with a callback.  And it cannot replace the
 * fast slot however much it looks like it -- FLS values do NOT live at
 * `gs:0x1480` but in another structure -- so swapping it for FLS would cost the
 * whole fast path.  Off Windows it is a @c pthread key with a destructor, which
 * is exactly the same thing, and there it is not even a new dependency: the
 * fallback slot already uses those keys.
 *
 * \~spanish
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
 * \~
 * @param fn
 * \~english what to call, with the value the thread had.
 * \~spanish a que llamar, con el valor que tuviera el hilo.
 * \~
 * @return
 * \~english the channel, or @c kNoThreadSlot if the system gives no more.
 *           Whoever asks has to keep working without the notification: worse,
 *           not broken.
 * \~spanish el canal, o @c kNoThreadSlot si el sistema no da mas.  Quien lo
 *           pida tiene que seguir funcionando sin aviso: peor, no roto.
 * \~
 */
uint32_t thread_exit_alloc_api(ThreadExitFn fn) noexcept;

/**
 * @brief
 * \~english Leaves in the channel the value THIS thread's notification will
 *          carry.
 * \~spanish Deja en el canal el valor con el que llegara el aviso de ESTE hilo.
 * \~
 * @param channel
 * \~english the channel from @c thread_exit_alloc_api.
 * \~spanish el canal que dio @c thread_exit_alloc_api.
 * \~
 * @param value
 * \~english what the notification carries; null cancels it, since the system
 *           only reports values that are not null.
 * \~spanish lo que lleva el aviso; nulo lo cancela, porque el sistema solo
 *           avisa de los valores que no lo son.
 * \~
 */
void thread_exit_arm_api(uint32_t channel, void *value) noexcept;

namespace detail {

#if !defined(_WIN32) && defined(__ELF__) &&                                    \
    (defined(__GNUC__) || defined(__clang__))
#define VESTA_THREAD_SLOT_TLS_FAST 1

/**
 * @brief
 * \~english This thread's slots, in real TLS.
 * \~spanish Las ranuras de este hilo, en TLS de verdad.
 * \~
 *
 * \~english
 * IT IS THE EXACT MIRROR OF THE WINDOWS BRANCH.  There the slots live in the
 * TEB's array and are read with `mov %gs:0x1480(,%rax,8)`; here they live in an
 * array of OURS whose offset from the thread pointer the linker fixes, and they
 * are read with `mov %fs:OFFSET(,%rax,8)`.  One instruction in both places, and
 * in neither is the C library touched.
 *
 * WHY IT WAS NOT SO BEFORE, which is what must not be repeated.  There was a
 * table of 256 cells here indexed by a hash of the thread pointer, with linear
 * probing.  It cost eighteen instructions -- two atomic loads of the slot, a
 * multiplication, two tables, a compare -- and it had a failure mode worse than
 * its cost: a thread whose natural cell was taken by another NEVER hit on the
 * first check, so on EVERY allocation and EVERY free it called the registration
 * function, which before finding its own tried a `compare_exchange` on somebody
 * else's cell -- stealing the cache line from the thread that owned it,
 * indefinitely.  With 256 cells that starts happening around 20 threads, and
 * measured with 24: the slowest thread took 3.2 times what the fastest did and
 * the cost per operation went from 16 to 23 ns, while the system allocator,
 * with the same threads and the same cores, did not move.  With this, neither
 * of those two things exists.
 *
 * The `initial-exec` MODEL, which is what turns it into one instruction: the
 * offset is resolved at link time and there stops being a call to
 * `__tls_get_addr`.  It is the same thing glibc itself does for its `malloc`
 * cache.  It has a known and accepted limit: a DYNAMIC library loaded with
 * `dlopen` needs static TLS reserve to be left, and if there is none, the load
 * FAILS -- loudly, which is how it has to fail.  It is 512 bytes per thread,
 * and besides the dynamic build already carries its own warnings in the README.
 *
 * No initialiser: an array of pointers starts at zero by construction, so there
 * is no guard variable and no destruction registration -- which is where
 * MinGW's emulated-TLS hangs came from.
 *
 * \~spanish
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
 *
 * \~
 */
constexpr uint32_t kDirectSlots = 64;
extern __thread void *g_tls_slots[kDirectSlots]
    __attribute__((tls_model("initial-exec")));

#endif

/**
 * @brief
 * \~english Whether the direct TEB read came out validated.
 * \~spanish Si la lectura directa del TEB quedo validada.
 * \~
 *
 * \~english
 * 0 = not checked yet, 1 = matches the API, 2 = does NOT match.
 *
 * It is one single flag for every slot on purpose: the TEB offset either is the
 * one we believe or it is not; it does not depend on the slot.
 *
 * IT IS HERE AND NOT IN THE `.cpp` because @c ThreadSlot::get consults it on
 * EVERY access, and with the check out of line the fast path was not fast: it
 * saved the call to the system API in order to pay another call.  It is not
 * public API; it is read through @c thread_slot_direct_ok.
 *
 * \~spanish
 * 0 = sin comprobar todavia, 1 = coincide con la API, 2 = NO coincide.
 *
 * Es una sola bandera para todas las ranuras a proposito: el desplazamiento del
 * TEB o es el que creemos o no lo es; no depende de la ranura.
 *
 * ESTA AQUI Y NO EN EL `.cpp` porque @c ThreadSlot::get la consulta en CADA
 * acceso, y con la comprobacion fuera de linea el camino rapido no era rapido:
 * se ahorraba la llamada a la API del sistema para pagar otra llamada.  No es
 * API publica; se lee por @c thread_slot_direct_ok.
 *
 * \~
 */
extern std::atomic<int> g_direct_state;

} // namespace detail

/**
 * @brief
 * \~english Whether the direct TEB read came out validated when the slot was
 *          reserved.
 * \~spanish Si la lectura directa del TEB quedo validada al reservar.
 * \~
 * @return
 * \~english true when it did; always false off Windows x64.
 * \~spanish true si quedo validada; siempre false fuera de Windows x64.
 * \~
 */
[[gnu::always_inline]] inline bool thread_slot_direct_ok() noexcept {
    return detail::g_direct_state.load(std::memory_order_acquire) == 1;
}

/**
 * @brief
 * \~english One pointer per thread.
 * \~spanish Un puntero por hilo.
 * \~
 *
 * \~english
 * It is declared as an ordinary global variable (not a thread one): what
 * changes per thread is the slot's CONTENT, not the object.  That way there is
 * neither a dynamic initialiser nor a guard variable, which is where the hangs
 * came from.
 *
 * \~spanish
 * Se declara como variable global normal (no de hilo): lo que cambia por hilo
 * es el CONTENIDO de la ranura, no el objeto.  Asi no hay ni inicializador
 * dinamico ni variable de guarda, que es de donde venian los cuelgues.
 *
 * \~
 *
 * \~english
 * @code
 *   static util::ThreadSlot g_slot;      // one global, no constructor
 *   if (g_slot.ensure()) {               // once per process
 *       g_slot.set(my_state);            // once per thread
 *       auto *s = static_cast<State *>(g_slot.get());   // one instruction
 *   }
 * @endcode
 *
 * \~spanish
 * @code
 *   static util::ThreadSlot g_slot;      // un global, sin constructor
 *   if (g_slot.ensure()) {               // una vez por proceso
 *       g_slot.set(mi_estado);           // una vez por hilo
 *       auto *s = static_cast<Estado *>(g_slot.get());  // una instruccion
 *   }
 * @endcode
 *
 * \~
 */
class ThreadSlot {
  public:
    /**
     * @brief
     * \~english Reserves the slot the first time.  Idempotent and thread safe.
     * \~spanish Reserva la ranura la primera vez.  Idempotente y entre hilos.
     * \~
     *
     * \~english
     * The check is INLINE and only the actual reservation lives in the `.cpp`.
     * It is not decoration: the allocator's per-thread cache calls here on
     * EVERY allocation, and with the whole function out of line that was one
     * call per `malloc`.  Measured with VTune over 144k lines, `ensure` retired
     * 1,466 million instructions -- fourth place in the whole compiler -- to do
     * nothing but look at an integer.
     *
     * \~spanish
     * La comprobacion va EN LINEA y solo la reserva de verdad vive en el
     * `.cpp`.  No es un adorno: el cache por hilo del asignador llama aqui en
     * CADA reserva, y con la funcion entera fuera eso era una llamada por
     * `malloc`.  Medido con VTune sobre 144k lineas, `ensure` retiraba 1.466
     * millones de instrucciones -- el cuarto puesto de todo el compilador --
     * para no hacer nada mas que mirar un entero.
     *
     * \~
     * @return
     * \~english false if the system could not give a slot.
     * \~spanish false si el sistema no pudo dar una ranura.
     * \~
     */
    [[gnu::always_inline]] bool ensure() noexcept {
        // Ya reservada, que es el caso de siempre menos la primera vez.
        if (slot_.load(std::memory_order_acquire) != kNoThreadSlot) return true;
        return reserve_slot();
    }

    /**
     * @brief
     * \~english THIS thread's value.
     * \~spanish El valor de ESTE hilo.
     * \~
     *
     * \~english
     * ONE COMPARE FOR BOTH RARE CASES.  There used to be two: first "is there a
     * slot?" (`s == kNoThreadSlot`) and then "does it fit in the direct ones?"
     * (`s < kDirectSlots`).  The second already rules the first out, because
     * `kNoThreadSlot` is all ones and is not less than anything -- so asking
     * both was asking the same thing twice.
     *
     * Leaving only the unsigned compare, the usual path is one branch and one
     * read, and the two rare cases fall together at the end.  It is the same
     * trick `host_alloc` already uses with the size (`n - 1 >= kMaxSmall`
     * covers zero and the overflow at once), applied where it was missing.
     *
     * \~spanish
     * UNA COMPARACION PARA LOS DOS CASOS RAROS.  Antes habia dos: primero "hay
     * ranura?" (`s == kNoThreadSlot`) y luego "cabe en las directas?"
     * (`s < kDirectSlots`).  La segunda ya descarta la primera, porque
     * `kNoThreadSlot` es todo unos y no es menor que nada -- asi que preguntar
     * las dos era preguntar dos veces lo mismo.
     *
     * Dejando solo la comparacion sin signo, el camino de siempre son un salto
     * y una lectura, y los dos casos raros caen juntos al final.  Es el mismo
     * truco que ya usa `host_alloc` con el tamano (`n - 1 >= kMaxSmall` cubre
     * el cero y el desbordamiento a la vez), aplicado donde faltaba.
     *
     * \~
     * @return
     * \~english what @c set left, or nullptr if it was never called in this
     *           thread.
     * \~spanish lo que dejo @c set, o nullptr si nunca se llamo en este hilo.
     * \~
     */
    [[gnu::always_inline]] void *get() const noexcept {
        const uint32_t s = slot_.load(std::memory_order_acquire);
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
        /* La pista NO sobra: sin ella el compilador deja el caso raro como
         * caida natural y manda el de siempre detras de un salto TOMADO, que
         * es la disposicion al reves de la que interesa. */
        if (__builtin_expect(s < detail::kDirectSlots, 1))
            return detail::g_tls_slots[s];
#endif
        // Los dos casos raros, juntos y fuera del camino de siempre.
        if (s == kNoThreadSlot) return nullptr;
        return thread_slot_get_api(s);
    }

    /**
     * @brief
     * \~english The slot index, for whoever has to read it ON THEIR OWN.
     * \~spanish El indice de ranura, para quien tenga que leerla POR SU CUENTA.
     * \~
     *
     * \~english
     * Generated code needs it: a JIT thunk cannot call `get()`, so it emits the
     * TEB read itself and for that it needs the index.
     *
     * \~spanish
     * Lo necesita el codigo generado: un thunk del JIT no puede llamar a
     * `get()`, asi que emite el la lectura del TEB y para eso necesita el
     * indice.
     *
     * \~
     * @return
     * \~english the index, or @c kNoThreadSlot if it has not been reserved yet.
     * \~spanish el indice, o @c kNoThreadSlot si aun no se ha reservado.
     * \~
     */
    uint32_t slot_index() const noexcept {
        return slot_.load(std::memory_order_acquire);
    }

    /**
     * @brief
     * \~english Sets THIS thread's value.
     * \~spanish Fija el valor de ESTE hilo.
     * \~
     *
     * \~english
     * It does not need to be fast: it is called once per thread, not on the hot
     * path.
     *
     * \~spanish
     * No hace falta que sea rapido: se llama una vez por hilo, no en el camino
     * caliente.
     *
     * \~
     * @param v
     * \~english what @c get will return from now on in this thread.
     * \~spanish lo que devolvera @c get a partir de ahora en este hilo.
     * \~
     */
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
     * @brief
     * \~english Sets the value WITHOUT re-arming the thread-exit notification.
     * \~spanish Fija el valor SIN rearmar el aviso de fin de hilo.
     * \~
     *
     * \~english
     * WHY IT EXISTS, because the difference from @c set is not one of style.
     * The notification is armed by storing the value in the system's channel,
     * so leaving something with @c set FROM INSIDE the notification itself
     * leaves the channel occupied again -- and the system, which is walking
     * precisely those channels in order to notify, finds ours full once more.
     * On Windows that hangs the thread's teardown, checked: five runs out of
     * five.
     *
     * So whatever is left after the notification is left through here.  The
     * value is seen the same with @c get -- it is the same slot -- and the only
     * thing that does not happen is the re-arming.
     *
     * \~spanish
     * PARA QUE EXISTE, porque la diferencia con @c set no es de estilo.  El
     * aviso se arma guardando el valor en el canal del sistema, asi que dejar
     * algo con @c set DESDE DENTRO del propio aviso vuelve a dejar el canal
     * ocupado -- y el sistema, que esta recorriendo justo esos canales para
     * avisar, se encuentra el nuestro lleno otra vez.  En Windows eso cuelga el
     * desmontaje del hilo, comprobado: cinco de cinco corridas.
     *
     * Asi que lo que se deja despues del aviso se deja por aqui.  El valor se
     * ve igual con @c get -- es la misma ranura --; lo unico que no pasa es el
     * rearme.
     *
     * \~
     * @param v
     * \~english what this slot keeps for THIS thread.
     * \~spanish lo que guarda esta ranura para ESTE hilo.
     * \~
     *
     * \~english
     * @code
     * // Inside the thread-exit notification, leaving a mark for whatever this
     * // thread still allocates while it is being torn down.
     * slot.set_quiet(reinterpret_cast<void *>(1));
     * @endcode
     *
     * \~spanish
     * @code
     * // Dentro del aviso de fin de hilo, dejando una marca para lo que
     * // todavia reserve este hilo mientras se desmonta.
     * slot.set_quiet(reinterpret_cast<void *>(1));
     * @endcode
     *
     * \~
     */
    void set_quiet(void *v) noexcept {
        const uint32_t s = slot_.load(std::memory_order_acquire);
        if (s == kNoThreadSlot) return;
#if defined(VESTA_THREAD_SLOT_TLS_FAST)
        if (s < detail::kDirectSlots) {
            detail::g_tls_slots[s] = v;
            return;
        }
#endif
        thread_slot_set_api(s, v);
    }

    /**
     * @brief
     * \~english Asks to be told when a thread with a value in this slot ends.
     * \~spanish Pide que se avise cuando termine un hilo con valor en esta
     *          ranura.
     * \~
     *
     * \~english
     * Idempotent and thread safe: whoever arrives first reserves the channel
     * and everybody else keeps theirs.  From then on, every @c set arms the
     * notification.  It has to be asked for BEFORE the thread's first @c set,
     * or that thread will not report.
     *
     * \~spanish
     * Idempotente y entre hilos: el primero que llega reserva el canal y los
     * demas se quedan con el suyo.  A partir de ahi, cada @c set arma el aviso.
     * Hay que pedirlo ANTES del primer @c set del hilo, o ese hilo no avisara.
     *
     * \~
     * @param fn
     * \~english what to call when a thread ends.
     * \~spanish a que llamar cuando termine un hilo.
     * \~
     * @return
     * \~english false if the system gives no channel.  Then nothing is
     *           reported, which is how it was before this existed -- not a
     *           failure.
     * \~spanish false si el sistema no da canal.  Entonces no se avisa de nada,
     *           que es como estaba antes de existir esto -- no es un fallo.
     * \~
     */
    [[gnu::always_inline]] bool notify_on_exit(ThreadExitFn fn) noexcept {
        if (exit_.load(std::memory_order_acquire) != kNoThreadSlot) return true;
        return reserve_exit(fn);
    }

  private:
    /// \~english The real reservation, once in the object's life.  Out of line
    ///           so that @c ensure's normal path is one load and one branch.
    /// \~spanish La reserva de verdad, una vez en la vida del objeto.  Fuera de
    ///           linea para que el camino normal de @c ensure sea una carga y
    ///           una rama.
    /// \~
    bool reserve_slot() noexcept;
    /// \~english The same as @c reserve_slot, and for the same reason: it
    ///           happens once.
    /// \~spanish Igual que @c reserve_slot, y por el mismo motivo: pasa una
    ///           vez.
    /// \~
    [[gnu::cold]] bool reserve_exit(ThreadExitFn fn) noexcept;

    /// \~english The slot index, or @c kNoThreadSlot while nobody has asked for
    ///           it.
    /// \~spanish El indice de ranura, o @c kNoThreadSlot mientras nadie la haya
    ///           pedido.
    /// \~
    std::atomic<uint32_t> slot_{kNoThreadSlot};
    /// \~english The notification channel, or @c kNoThreadSlot when nobody
    ///           asked for it.
    /// \~spanish El canal de aviso, o @c kNoThreadSlot si nadie lo pidio.
    /// \~
    std::atomic<uint32_t> exit_{kNoThreadSlot};
};

} // namespace util

#endif // VESTA_UTIL_THREAD_SLOT_H
