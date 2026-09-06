/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/thread_slot.cpp
 * @brief Implementacion de ThreadSlot.  El contrato y los motivos, en la
 *        cabecera; aqui solo lo que necesita las cabeceras del sistema.
 */
/* ANTES DE NADA, y por eso esta por encima incluso de nuestra propia cabecera:
 * `FlsAlloc` -- el aviso de fin de hilo -- es de Vista, y las cabeceras de
 * MinGW la esconden si nadie pide esa version.  Pedirla despues del primer
 * include no vale: cualquier cabecera estandar arrastra `_mingw.h`, que ya deja
 * `_WIN32_WINNT` puesto en su valor por defecto, y entonces esto no haria nada
 * -- que es justo lo que pasaba.
 *
 * Se pide SOLO si quien nos incluye no ha pedido otra version: subirle el
 * minimo a su espalda seria peor que quedarse sin reciclar identificadores.  Y
 * si acaba sin declararse, `thread_exit_alloc_api` devuelve "no hay canal" y
 * quien lo pidio se entera por el valor de retorno. */
#if defined(_WIN32) && !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0600
#endif

#include "util/thread_slot.h"

#if defined(_WIN32)
#include <windows.h>
#include <fibersapi.h>
#else
#include <pthread.h>
#endif

namespace util {

namespace detail {

/// La bandera vive aqui; el porque de que se DECLARE en la cabecera esta alli.
std::atomic<int> g_direct_state{0};

#if defined(VESTA_THREAD_SLOT_TLS_FAST)

/* La definicion.  El modelo se repite aqui porque tiene que coincidir con el de
 * la declaracion: si difieren, el compilador genera accesos de otro modelo y se
 * pierde justamente lo que se venia a ganar. */
__thread void *g_tls_slots[kDirectSlots]
    __attribute__((tls_model("initial-exec")));

#endif

} // namespace detail

using detail::g_direct_state;

namespace {

#if !defined(_WIN32)
/// Claves de pthread, que es lo que hace de ranura fuera de Windows.
constexpr uint32_t kMaxKeys = 64;
pthread_key_t g_keys[kMaxKeys];
std::atomic<uint32_t> g_key_count{0};
#endif

/// Devuelve una ranura que ya no se va a usar (se perdio una carrera).
void release_slot(uint32_t slot) noexcept {
#if defined(_WIN32)
    TlsFree(static_cast<DWORD>(slot));
#else
    // Las claves de pthread se reparten de un array fijo y no se devuelven:
    // esto solo pasa al perder una carrera de inicializacion, que ocurre como
    // mucho una vez por ranura en toda la vida del proceso.
    (void)slot;
#endif
}

/**
 * @brief Comprueba que leer el TEB a mano da lo MISMO que la API.
 *
 * `0x1480` es una interioridad del sistema operativo, no un contrato publico.
 * En vez de confiar, se escribe un valor reconocible por la API y se lee por la
 * via directa.  Si no coincide -- otra version de Windows, otra disposicion --
 * la via directa se apaga y todo pasa por la API: se pierden ocho nanosegundos
 * por acceso y no se lee ni un byte que no sea nuestro.
 */
void validate_direct_read(uint32_t slot) noexcept {
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    // Por encima de 64 la ranura ya no vive en el TEB, asi que esta no puede
    // decir nada: se deja el estado como estaba para que lo valide otra.
    if (slot >= 64) return;
    void *const probe =
        reinterpret_cast<void *>(static_cast<uintptr_t>(0xA110CA7EULL));
    void *const saved = TlsGetValue(static_cast<DWORD>(slot));
    TlsSetValue(static_cast<DWORD>(slot), probe);
    void *seen;
    asm volatile("movq %%gs:0x1480(,%1,8), %0"
                 : "=r"(seen)
                 : "r"(static_cast<uint64_t>(slot)));
    TlsSetValue(static_cast<DWORD>(slot), saved);
    g_direct_state.store(seen == probe ? 1 : 2, std::memory_order_release);
#else
    (void)slot;
    g_direct_state.store(2, std::memory_order_release);
#endif
}

} // namespace

#if defined(_WIN32)

void *thread_slot_get_api(uint32_t slot) noexcept {
    return TlsGetValue(static_cast<DWORD>(slot));
}

void thread_slot_set_api(uint32_t slot, void *value) noexcept {
    TlsSetValue(static_cast<DWORD>(slot), value);
}

uint32_t thread_slot_alloc_api() noexcept {
    const DWORD s = TlsAlloc();
    return (s == TLS_OUT_OF_INDEXES) ? kNoThreadSlot : static_cast<uint32_t>(s);
}

#if defined(FLS_OUT_OF_INDEXES)

uint32_t thread_exit_alloc_api(ThreadExitFn fn) noexcept {
    /* `PFLS_CALLBACK_FUNCTION` es `void __stdcall (PVOID)`.  En x64 solo hay
     * una convencion de llamada, asi que la conversion no puede mentir; en
     * x86-32 si son distintas, y por eso `ThreadExitFn` se declara SIN
     * convencion explicita -- ahi el compilador rechaza la conversion en vez de
     * aceptarla y dejar la pila descuadrada al volver del aviso. */
    const DWORD s = FlsAlloc(reinterpret_cast<PFLS_CALLBACK_FUNCTION>(fn));
    return (s == FLS_OUT_OF_INDEXES) ? kNoThreadSlot : static_cast<uint32_t>(s);
}

void thread_exit_arm_api(uint32_t channel, void *value) noexcept {
    FlsSetValue(static_cast<DWORD>(channel), value);
}

#else // sin FLS: no hay aviso, y se DICE por el valor de retorno

uint32_t thread_exit_alloc_api(ThreadExitFn) noexcept { return kNoThreadSlot; }
void thread_exit_arm_api(uint32_t, void *) noexcept {}

#endif

#else // !_WIN32

void *thread_slot_get_api(uint32_t slot) noexcept {
    return (slot < kMaxKeys) ? pthread_getspecific(g_keys[slot]) : nullptr;
}

void thread_slot_set_api(uint32_t slot, void *value) noexcept {
    if (slot < kMaxKeys) pthread_setspecific(g_keys[slot], value);
}

uint32_t thread_slot_alloc_api() noexcept {
    const uint32_t i = g_key_count.fetch_add(1, std::memory_order_acq_rel);
    if (i >= kMaxKeys) return kNoThreadSlot;
    if (pthread_key_create(&g_keys[i], nullptr) != 0) return kNoThreadSlot;
    return i;
}

/* El aviso sale del MISMO array de claves: aqui una clave con destructor es
 * exactamente el mecanismo que hace falta, asi que no hay nada especifico que
 * anadir -- solo pasar el destructor en vez de nulo. */
uint32_t thread_exit_alloc_api(ThreadExitFn fn) noexcept {
    const uint32_t i = g_key_count.fetch_add(1, std::memory_order_acq_rel);
    if (i >= kMaxKeys) return kNoThreadSlot;
    if (pthread_key_create(&g_keys[i], fn) != 0) return kNoThreadSlot;
    return i;
}

void thread_exit_arm_api(uint32_t channel, void *value) noexcept {
    if (channel < kMaxKeys) pthread_setspecific(g_keys[channel], value);
}

#endif // _WIN32

bool ThreadSlot::reserve_slot() noexcept {
    /* La comprobacion de "ya esta" la hace @c ensure en linea; aqui se vuelve a
     * mirar porque dos hilos pueden haber pasado esa comprobacion a la vez. */
    if (slot_.load(std::memory_order_acquire) != kNoThreadSlot) return true;

    /* Dos hilos pueden llegar aqui a la vez.  Cada uno pide la suya y solo una
     * se queda: quien pierde devuelve la que pidio.  Reservar de mas una vez es
     * barato; usar dos ranuras distintas para el mismo dato no lo seria. */
    const uint32_t mine = thread_slot_alloc_api();
    if (mine == kNoThreadSlot) return false;

    uint32_t expected = kNoThreadSlot;
    if (!slot_.compare_exchange_strong(expected, mine,
                                       std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
        release_slot(mine);
        return true; // gano otro; la ranura buena ya esta puesta
    }
    if (g_direct_state.load(std::memory_order_acquire) == 0)
        validate_direct_read(mine);
    return true;
}

bool ThreadSlot::reserve_exit(ThreadExitFn fn) noexcept {
    // Se vuelve a mirar: dos hilos pueden haber pasado la comprobacion en linea.
    if (exit_.load(std::memory_order_acquire) != kNoThreadSlot) return true;

    const uint32_t mine = thread_exit_alloc_api(fn);
    if (mine == kNoThreadSlot) return false;

    /* Misma carrera y mismo desenlace que en @c reserve_slot: quien pierde se
     * queda con un canal de mas sin usar.  Devolverlo costaria mas de lo que
     * ahorra -- pasa como mucho una vez en la vida del proceso -- y en POSIX ni
     * siquiera se puede: las claves salen de un array que no se recicla. */
    uint32_t expected = kNoThreadSlot;
    if (!exit_.compare_exchange_strong(expected, mine,
                                       std::memory_order_acq_rel,
                                       std::memory_order_acquire))
        return true; // gano otro; el canal bueno ya esta puesto
    return true;
}

} // namespace util
