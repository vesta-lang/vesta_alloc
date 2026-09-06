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
 * COMO.  En Windows x64 las ranuras 0..63 que reparte `TlsAlloc` viven en el
 * TEB, en `TEB+0x1480`, y al TEB se llega por el segmento `gs`.  Leer una es
 * una sola instruccion.  Para el resto de casos -- ranura >= 64, otra
 * arquitectura -- se cae a la API del sistema, que sigue siendo cinco veces
 * mejor que la TLS emulada.  Fuera de Windows se usa `__thread`, que ahi si es
 * TLS de verdad y no tiene este problema.
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

namespace detail {

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
        if (s != kNoThreadSlot) thread_slot_set_api(s, v);
    }

  private:
    /// La reserva de verdad, una vez en la vida del objeto.  Fuera de linea
    /// para que el camino normal de @c ensure sea una carga y una rama.
    bool reserve_slot() noexcept;

    std::atomic<uint32_t> slot_{kNoThreadSlot};
};

} // namespace util

#endif // VESTA_UTIL_THREAD_SLOT_H
