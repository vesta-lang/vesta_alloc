/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file util/self_inspect_c.cpp
 * @brief La capa en C de mirarse a uno mismo.  Los motivos, en la cabecera.
 *
 * Capa DELGADA a proposito: cada funcion se apoya en la de C++ y no repite ni
 * una decision.  Dos implementaciones de lo mismo acabarian divergiendo, y la
 * que se quedara corta seria la que menos se mira.
 *
 * Lo unico que hay aqui de verdad es la ADAPTACION de forma: lo que en C++
 * devuelve un `std::string` o un `std::vector` aqui se copia a memoria del que
 * llama, que es como se hacen las cosas en C -- asi no hay que devolver un
 * bloque y acordarse de quien lo libera.
 */

#include "util/symbols/self_inspect_c.h"

#include "util/symbols/self_dwarf.h"
#include "util/symbols/self_image.h"
#include "util/symbols/self_resolver.h"
#include "util/symbols/self_symbols.h"
#include "util/mem/vesta_memcpy.h"

#include <string>
#include <vector>

extern "C" {

unsigned vesta_self_frames(const void *pc, VestaAllocFrame *out,
                           unsigned max) {
    /* El mismo cuerpo que el resolutor del gancho, porque es la misma
     * pregunta: los tres intentos en orden.  Un nombre por uso, una sola
     * implementacion. */
    return vesta_self_resolver(pc, out, max);
}

int vesta_self_covers(const void *pc) {
    return util::self_dwarf_covers(pc) ? 1 : 0;
}

const char *vesta_self_symbol(const void *pc, size_t *offset) {
    return util::self_symbol(pc, offset);
}

int vesta_self_function_range(const void *pc, const void **begin,
                              size_t *bytes) {
    return util::self_function_range(pc, begin, bytes) ? 1 : 0;
}

size_t vesta_self_symbol_count(void) { return util::self_symbol_count(); }
size_t vesta_self_function_count(void) { return util::self_function_count(); }
size_t vesta_self_dwarf_units(void) { return util::self_dwarf_units(); }
size_t vesta_self_dwarf_ranges(void) { return util::self_dwarf_ranges(); }

size_t vesta_self_image_path(char *buf, size_t cap) {
    const std::string path = util::self_image_path();
    if (buf != nullptr && cap > 0) {
        /* Se copia lo que quepa y SIEMPRE se termina en nulo, pero lo que se
         * devuelve es la longitud ENTERA: asi quien llama ve que se trunco
         * comparandola con `cap`, en vez de creerse una ruta a medias. */
        const size_t fit = path.size() < cap - 1 ? path.size() : cap - 1;
        vesta_memcpy(buf, path.c_str(), fit);
        buf[fit] = '\0';
    }
    return path.size();
}

uint64_t vesta_self_image_link_base(void) {
    return util::self_image_link_base();
}

int vesta_self_section_range(const char *name, uint64_t *offset,
                             uint64_t *bytes) {
    return util::self_section_range(name, offset, bytes) ? 1 : 0;
}

size_t vesta_self_section_read(const char *name, uint64_t offset, void *dst,
                               size_t bytes) {
    if (dst == nullptr || bytes == 0) return 0;
    const std::vector<unsigned char> part =
        util::self_section_part(name, offset, bytes);
    /* Vacio significa que la seccion no esta o que el trozo se sale de ella.
     * No se recorta: un trozo corto se lee como una estructura truncada, y eso
     * da cosas que parecen validas. */
    if (part.empty()) return 0;
    /* Acotado en el sitio de la copia aunque `part` no pueda ser mayor que
     * `bytes` por construccion: es lo que le falta al compilador para no
     * avisar, y es la misma leccion que ya costo una tarde con este `memcpy`
     * -- sabe cuanto mide el destino y no puede saber cuanto mide el origen. */
    const size_t n = part.size() < bytes ? part.size() : bytes;
    vesta_memcpy(dst, part.data(), n);
    return n;
}

} // extern "C"
