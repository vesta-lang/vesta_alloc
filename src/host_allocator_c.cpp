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

#include "util/alloc_sites.h"
#include "util/call_site.h"
#include "util/host_allocator.h"

#include <cstdlib>

namespace {

/**
 * @brief Apunta de donde vino esta reserva, si se ha pedido medir.
 *
 * POR QUE HACE FALTA AQUI.  Porque el apuntado del sitio vivia SOLO en el
 * camino de `operator new`, que es una construccion de C++.  Un programa en C
 * -- o una libreria en C enchufada por el gancho -- reservaba a traves nuestro
 * y salia en los contadores, pero no en NINGUN sitio: el arbol de "de donde
 * nacen las reservas" salia vacio para la mitad del codigo que usa esta
 * libreria.  Contar y saber de donde son dos cosas, y faltaba la segunda.
 *
 * @p ret es la direccion de retorno del envoltorio, o sea la instruccion
 * siguiente a la llamada EN EL CODIGO EN C.  Vale exactamente lo mismo que el
 * `[rsp]` que lee el parche de `operator new`, y por eso los envoltorios de
 * abajo no se pueden meter en linea: si el compilador los fundiera con quien
 * llama -- y con enlazado al vuelo puede --, la direccion seria la del
 * llamante DEL llamante y el sitio saldria una funcion mas arriba, que es un
 * dato equivocado con toda la pinta de ser correcto.
 *
 * Y solo cuando se ha pedido medir: una medida que nadie pidio no se paga.
 */
inline void note_site(const void *ret, size_t n) noexcept {
    /* Las DOS condiciones, y la segunda importa mas de lo que parece.  Medir
     * (`VESTA_HOST_ALLOC_STATS`) y apuntar sitios (`VESTA_HOST_ALLOC_SITES`)
     * son peticiones distintas: el parche sobre `operator new` solo se instala
     * con la segunda.  Sin esta comprobacion, pidiendo solo estadisticas la
     * tabla de sitios se llenaria con lo que reserva el C y con NADA de lo que
     * reserva el C++ -- una lista completa en apariencia que dejaria fuera la
     * mitad del programa. */
    if (!util::detail::g_measure || !util::call_site_patch_installed()) return;
    const util::detail::ThreadCache *c = util::detail::current_cache();
    util::record_alloc_site(ret, n, c != nullptr ? c->tag : 0);
}

} // namespace

extern "C" {

[[gnu::noinline]] void *vesta_host_alloc(size_t n) {
    void *p = util::host_alloc(n);
    note_site(__builtin_return_address(0), n);
    return p;
}

[[gnu::noinline]] void *vesta_host_calloc(size_t count, size_t size) {
    /* El desbordamiento del producto es un fallo clasico de `calloc`, y de los
     * caros: si se desborda se reserva de menos y se escribe de mas.  Se
     * comprueba antes de multiplicar. */
    if (count != 0 && size > (size_t(-1) / count)) return nullptr;
    /* `host_alloc_zeroed`, no `host_alloc` mas un `memset`: cuando la memoria
     * acaba de venir del sistema ya esta a cero y limpiarla otra vez es
     * escribir de balde.  Con bloques de 1 MiB eran veintitres veces mas lento
     * que `calloc`; ver la cabecera. */
    void *p = util::host_alloc_zeroed(count * size);
    note_site(__builtin_return_address(0), count * size);
    return p;
}

[[gnu::noinline]] void *vesta_host_realloc(void *p, size_t n) {
    /* Toda la logica -- incluido estirar un tramo en su sitio en vez de copiar
     * -- vive en la capa de C++.  Aqui no se repite ni una decision: dos
     * implementaciones del mismo `realloc` acabarian divergiendo, y la que se
     * quedara corta seria la que menos se mira. */
    void *q = util::host_realloc(p, n);
    /* Un `realloc` TAMBIEN se apunta, y no es un capricho: en C es la forma
     * normal de crecer -- es el `std::vector` de aqui --, asi que dejarlo
     * fuera esconderia justo el patron que mas interesa ver.  Se apunta con el
     * tamano NUEVO, que es lo que se acaba de pedir. */
    note_site(__builtin_return_address(0), n);
    return q;
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
