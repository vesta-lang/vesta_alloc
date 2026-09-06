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

#include <cstdlib>


extern "C" {

void *vesta_host_alloc(size_t n) {
    return util::host_alloc(n);
}

void *vesta_host_calloc(size_t count, size_t size) {
    /* El desbordamiento del producto es un fallo clasico de `calloc`, y de los
     * caros: si se desborda se reserva de menos y se escribe de mas.  Se
     * comprueba antes de multiplicar. */
    if (count != 0 && size > (size_t(-1) / count)) return nullptr;
    /* `host_alloc_zeroed`, no `host_alloc` mas un `memset`: cuando la memoria
     * acaba de venir del sistema ya esta a cero y limpiarla otra vez es
     * escribir de balde.  Con bloques de 1 MiB eran veintitres veces mas lento
     * que `calloc`; ver la cabecera. */
    return util::host_alloc_zeroed(count * size);
}

void *vesta_host_realloc(void *p, size_t n) {
    /* Toda la logica -- incluido estirar un tramo en su sitio en vez de copiar
     * -- vive en la capa de C++.  Aqui no se repite ni una decision: dos
     * implementaciones del mismo `realloc` acabarian divergiendo, y la que se
     * quedara corta seria la que menos se mira. */
    return util::host_realloc(p, n);
}

void vesta_host_free(void *p) {
    util::host_free(p);
}

size_t vesta_host_usable_size(const void *p) {
    return util::host_usable_size(p);
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
