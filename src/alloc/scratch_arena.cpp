/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/scratch_arena.cpp
 * @brief Implementacion de la arena de fase.  Los motivos, en la cabecera.
 *
 * La memoria de los bloques se pide con `util::os_reserve` + `os_commit`, que
 * es la unica capa del proyecto que habla de memoria con el sistema por debajo
 * de las reservas.  No se duplica aqui.
 *
 * Se pedia por `vm::allocate_memory`, y se cambio por dos razones: aquella
 * arrastra `windows.h` -- que define `VOID` como macro y rompe los `enum class`
 * que usen ese nombre -- y los flujos de C++, y ataba la arena al arbol del
 * compilador entero.  Sin esa atadura, esto se puede sacar y reusar tal cual.
 */
#include "util/alloc/scratch_arena.h"

/* `host_alloc_pages`: con ancla, los bloques se le piden al asignador, que es
 * quien puede servir DENTRO de la reserva donde vive el dato. */
#include "util/alloc/host_allocator.h"

#include "util/os/os_memory.h" // apalabrar y entregar, sin arrastrar nada
#include "util/os/thread_slot.h"

#include <atomic>

namespace util {

namespace {

/// Primer bloque.  Pequeno a proposito: la mayoria de fases necesitan poco y un
/// hilo que apenas trabaja no deberia pagar megabytes.
constexpr size_t kFirstBlock = 64 * 1024;
/// Los bloques van doblando hasta aqui, para que una fase grande no acabe
/// encadenando cientos de bloques pequenos.
constexpr size_t kMaxBlock = 4 * 1024 * 1024;

/// Arenas preparadas de antemano, una por hilo.  Es memoria estatica: no hay
/// inicializador dinamico que pueda colgarse (ver `thread_slot.h`).
///
/// El nombre lleva `Arena` a proposito: el asignador tiene su propio
/// `kMaxThreads` -- cuantos duenos puede nombrar -- y son dos cifras distintas
/// que no tienen por que coincidir.  Llamarlas igual las hacia ambiguas al
/// incluirlo, y peor: invitaba a creer que una gobierna a la otra.
constexpr uint32_t kMaxArenaThreads = 64;
ScratchArena g_arenas[kMaxArenaThreads];
std::atomic<uint32_t> g_next_arena{0};
ThreadSlot g_arena_slot;

/**
 * @brief Pide memoria del sistema por la capa propia.
 *
 * Antes se pedia por `vm::allocate_memory`, y eso ataba la arena al arbol del
 * compilador entero: esa cabecera arrastra `windows.h`, los flujos de C++ y el
 * gestor de arenas de la maquina virtual.  `util/os/os_memory.h` hace lo mismo sin
 * arrastrar nada -- ni siquiera las cabeceras del sistema --, que es lo que
 * permite que esto se pueda sacar y reusar tal cual.
 *
 * @par Hilos
 * Segura.  Cada llamada devuelve un bloque distinto.
 */
void *system_block(size_t bytes, OsProt prot) noexcept {
    void *p = os_reserve(bytes);
    if (p == nullptr) return nullptr;
    if (!os_commit(p, bytes, prot)) {
        os_release(p, bytes);
        return nullptr;
    }
    return p;
}

} // namespace

ScratchArena::Block *ScratchArena::add_block(size_t least) noexcept {
    size_t size = (current_ != nullptr) ? (current_->size + sizeof(Block)) * 2
                                        : kFirstBlock;
    if (size > kMaxBlock) size = kMaxBlock;
    // Y si lo que se pide no cabe ni asi, el bloque se hace a medida: una
    // reserva grande suelta no puede quedarse sin sitio solo porque el tamano
    // de bloque tenga tope.
    while (size < least + sizeof(Block))
        size *= 2;

    /* SI LA ARENA PIDIO SITIO, se pide.  `os_alloc_near` recorre la ventana
     * buscando el hueco mas cercano; si no hay, contesta nulo -- que NO es
     * quedarse sin memoria -- y entonces se cae al camino de siempre, donde
     * elige el sistema.  La arena sigue sirviendo; lo que cambia es que
     * `placed()` pasa a decir que no, y quien necesitaba la cercania puede
     * enterarse en vez de fallar mucho despues y en otro sitio. */
    /* CON ANCLA, SE LE PIDE AL ASIGNADOR, que es el unico que puede servir
     * DENTRO de la reserva donde vive el dato.  Pedirle al sistema un rango
     * pegado no vale cuando el ancla esta dentro de una reserva mayor que la
     * ventana -- medido: 22 regiones y hueco mayor cero, porque +-2 GB cabe
     * entero dentro de los 16 GiB de la region grande --, y ahi la unica salida
     * es servir desde dentro.  `host_alloc_pages` hace eso, y cae al sistema
     * solo cuando el ancla no es suya. */
    void *mem = nullptr;
    if (anchor_ != nullptr) {
        mem = host_alloc_pages(size, prot_, anchor_, window_, &placed_, &scan_);
    }
    if (mem == nullptr) {
        mem = system_block(size, prot_);
        if (anchor_ != nullptr) placed_ = false;
    }
    if (mem == nullptr) return nullptr;

    Block *b = static_cast<Block *>(mem);
    b->next = nullptr;
    b->size = size - sizeof(Block);
    b->used = 0;
    if (current_ != nullptr)
        current_->next = b;
    else
        head_ = b;
    current_ = b;
    reserved_ += size;
    return b;
}

void *ScratchArena::allocate(size_t n, size_t align) noexcept {
    if (n == 0) n = 1;
    if (align < alignof(void *)) align = alignof(void *);
    for (;;) {
        if (current_ != nullptr) {
            const uintptr_t base = reinterpret_cast<uintptr_t>(current_ + 1);
            const uintptr_t at =
                (base + current_->used + align - 1) & ~(uintptr_t)(align - 1);
            const size_t need = (at - base) + n;
            if (need <= current_->size) {
                current_->used = need;
                return reinterpret_cast<void *>(at);
            }
            if (current_->next != nullptr) {
                // Ya hay otro bloque de una fase anterior: se reaprovecha.
                current_ = current_->next;
                current_->used = 0;
                continue;
            }
        }
        if (add_block(n + align) == nullptr) return nullptr;
    }
}

void ScratchArena::release(Mark m) noexcept {
    /* Los bloques NO se devuelven al sistema: quedan encadenados y listos para
     * la siguiente fase.  Es justo lo que hace que reservar salga casi gratis a
     * partir de la segunda vuelta. */
    if (m.block == nullptr) {
        current_ = head_;
        for (Block *b = head_; b != nullptr; b = b->next)
            b->used = 0;
        return;
    }
    for (Block *b = m.block->next; b != nullptr; b = b->next)
        b->used = 0;
    m.block->used = m.used;
    current_ = m.block;
}

ScratchArena &scratch_arena() noexcept {
    g_arena_slot.ensure();
    if (void *p = g_arena_slot.get()) return *static_cast<ScratchArena *>(p);

    const uint32_t id = g_next_arena.fetch_add(1, std::memory_order_acq_rel);
    ScratchArena *a;
    if (id < kMaxArenaThreads) {
        a = &g_arenas[id];
    } else {
        /* Mas hilos de los previstos.  Se le da una arena propia igualmente --
         * compartir una entre dos hilos seria una carrera -- y se acepta que
         * esa memoria no se devuelva: pasa como mucho una vez por hilo extra.
         */
        void *mem = system_block(sizeof(ScratchArena), kOsReadWrite);
        if (mem == nullptr) return g_arenas[0]; // sin memoria ni para esto
        a = new (mem) ScratchArena();
    }
    g_arena_slot.set(a);
    return *a;
}

} // namespace util
