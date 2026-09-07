/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file util/alloc_sites_c.cpp
 * @brief La capa en C de los sitios.  Los motivos, en la cabecera.
 *
 * Capa DELGADA a proposito: cada funcion se apoya en la de C++ y no repite ni
 * una decision.  Y no hay ni una conversion, que es el punto de declarar la
 * estructura en la cabecera de C: los dos lenguajes nombran EL MISMO tipo, asi
 * que la foto se escribe directamente en el array del que llama.  Traducir
 * entre dos estructuras parecidas seria una copia por sitio y, peor, un sitio
 * donde las dos pueden dejar de coincidir.
 */

#include "util/report/alloc_sites_c.h"

#include "util/report/alloc_sites.h"
#include "util/alloc/alloc_tag.h"

extern "C" {

unsigned vesta_alloc_sites_snapshot(VestaAllocSite *out, unsigned max) {
    return util::alloc_sites_snapshot(out, max);
}

void vesta_alloc_dump_sites(unsigned top) { util::dump_alloc_sites(top); }

void vesta_alloc_record_site(const void *pc, size_t n, unsigned char tag) {
    util::record_alloc_site(pc, n, tag);
}

uint64_t vesta_alloc_sites_overflow(void) {
    return util::alloc_sites_overflow();
}

uint64_t vesta_alloc_sites_skipped(void) { return util::alloc_sites_skipped(); }

const char *vesta_alloc_tag_name(unsigned char tag) {
    /* Se recorta a los bits que la etiqueta usa en vez de rechazar un valor
     * fuera de rango: esto es diagnostico, y lo peor que puede pasar es que
     * salga el nombre de otra casilla.  Devolver NULL obligaria a todo el que
     * imprima a comprobarlo. */
    return util::alloc_tag_name(util::AllocTag::from_raw(tag & 0xF));
}

} // extern "C"
