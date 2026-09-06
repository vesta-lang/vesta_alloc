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
#include "util/thread_slot.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace util {

namespace detail {

/// La bandera vive aqui; el porque de que se DECLARE en la cabecera esta alli.
std::atomic<int> g_direct_state{0};

#if defined(VESTA_THREAD_SLOT_FS_FAST)

std::atomic<uintptr_t> g_tp_key[kTpSlots];
void *g_tp_value[64][kTpSlots];

/**
 * @brief Da de alta un hilo en la tabla directa.
 *
 * Sondeo lineal desde su casilla natural.  Con 256 casillas y los pocos hilos
 * que tiene un proceso, la primera suele estar libre; el bucle esta por los
 * casos raros, no por el normal.
 *
 * El `compare_exchange` es lo que hace que dos hilos que lleguen a la vez no se
 * pisen: el que pierde sigue buscando.  Y si su clave YA esta puesta, es que
 * otro le gano la carrera reclamando la misma casilla para el mismo hilo, cosa
 * imposible -- un puntero de hilo solo lo tiene un hilo --, asi que basta con
 * quedarse con ella.
 *
 * Las casillas NO se liberan al morir un hilo: no hay aviso de fin de hilo sin
 * volver a depender de la biblioteca C, que es justo lo que se esta quitando.
 * Es el mismo limite que ya tienen los identificadores del asignador, y se
 * comporta igual -- al pasarse, se sirve por el camino general.
 */
uint32_t register_thread_pointer(uintptr_t tp) noexcept {
    const uint32_t start = tp_index(tp); // el MISMO reparto que el camino rapido
    for (uint32_t n = 0; n < kTpSlots; ++n) {
        const uint32_t i = (start + n) & (kTpSlots - 1);
        uintptr_t expected = 0;
        if (g_tp_key[i].compare_exchange_strong(expected, tp,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire))
            return i;
        if (expected == tp) return i; // ya era nuestra
    }
    return kTpSlots; // sin sitio: el que llama se va por el camino general
}

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

} // namespace util
