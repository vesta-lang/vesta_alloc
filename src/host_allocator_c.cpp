/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/host_allocator_c.cpp
 * @brief La capa en C del asignador.  Los motivos, en la cabecera.
 *
 * Es una capa DELGADA a proposito: cada funcion se apoya en la de C++ y no
 * repite ni una decision.  Dos implementaciones del mismo asignador acabarian
 * divergiendo, y la que se quedara corta seria la que menos se mira.
 */

#include "util/host_allocator_c.h"

#include "util/host_allocator.h"
#include "util/host_allocator_layout.h"

#include <cstring>
#include <cstdlib>

namespace {

/**
 * @brief Bytes utilizables de @p p, o 0 si no lo sabemos.
 *
 * Cero significa "no es nuestro": vino del asignador del sistema, sea porque el
 * nuestro esta apagado o porque en su momento no pudo servirlo.  No significa
 * que el bloque este vacio.
 */
size_t usable_of(const void *p) noexcept {
    if (p == nullptr || !util::in_region(p)) return 0;
    const util::ChunkHeader *h =
        util::chunk_of(const_cast<void *>(p));
    if (h->magic == util::kChunkMagic) return util::kSizes[h->cls];
    if (h->magic == util::kSpanMagic)
        return size_t(h->cls) * util::kChunkBytes - sizeof(util::ChunkHeader);
    return 0; // marca desconocida: no inventamos un tamano
}

} // namespace

extern "C" {

void *vesta_host_alloc(size_t n) {
    return util::host_alloc(n);
}

void *vesta_host_calloc(size_t count, size_t size) {
    /* El desbordamiento del producto es un fallo clasico de `calloc`, y de los
     * caros: si se desborda se reserva de menos y se escribe de mas.  Se
     * comprueba antes de multiplicar. */
    if (count != 0 && size > (size_t(-1) / count)) return nullptr;
    const size_t total = count * size;
    void *p = util::host_alloc(total);
    if (p != nullptr) std::memset(p, 0, total);
    return p;
}

void *vesta_host_realloc(void *p, size_t n) {
    if (p == nullptr) return util::host_alloc(n);
    if (n == 0) {
        util::host_free(p);
        return nullptr;
    }
    if (!util::in_region(p)) {
        /* No es nuestro: vino del sistema, asi que tiene que volver al sistema.
         * Mezclar los dos asignadores en un `realloc` seria pasarle a `free` un
         * puntero que no reconoce. */
        return std::realloc(p, n);
    }
    const size_t old = usable_of(p);
    /* Si ya cabe, no se toca.  El redondeo a clase juega a favor aqui: pedir de
     * 40 a 48 bytes no mueve nada porque los dos caen en la clase de 48. */
    if (old >= n) return p;

    void *fresh = util::host_alloc(n);
    if (fresh == nullptr) return nullptr; // `p` sigue siendo valido, como manda
    std::memcpy(fresh, p, old < n ? old : n);
    util::host_free(p);
    return fresh;
}

void vesta_host_free(void *p) {
    util::host_free(p);
}

size_t vesta_host_usable_size(const void *p) {
    return usable_of(p);
}

unsigned vesta_host_push_tag(unsigned use, unsigned shape) {
    util::detail::ThreadCache *c = util::detail::ensure_cache();
    if (c == nullptr) return 0;
    const unsigned previous = c->tag;
    /* Se recorta en vez de rechazar: esto es diagnostico, y un numero fuera de
     * rango no debe cambiar lo que el programa HACE.  Lo peor que pasa es que
     * se cuente en otra casilla. */
    const util::AllocTag t{
        static_cast<util::AllocUse>(use & 0x3),
        static_cast<util::AllocShape>(shape & 0x3)};
    c->tag = t.raw();
    return previous;
}

void vesta_host_pop_tag(unsigned previous) {
    util::detail::ThreadCache *c = util::detail::current_cache();
    if (c != nullptr) c->tag = static_cast<uint8_t>(previous & 0xF);
}

} // extern "C"
