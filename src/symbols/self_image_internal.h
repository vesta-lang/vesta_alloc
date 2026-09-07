/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file src/self_image_internal.h
 * @brief Lo que comparten las dos mitades de la lectura del propio binario.
 *
 * NO ES API: no se instala y nadie de fuera debe incluirlo.  Existe porque la
 * lectura se parte en dos por una costura clara -- lo que depende del FORMATO
 * (PE o ELF) y lo que no --, y las dos mitades necesitan los mismos cuatro
 * ayudantes de fichero.
 */
#ifndef VESTA_SRC_SELF_IMAGE_INTERNAL_H
#define VESTA_SRC_SELF_IMAGE_INTERNAL_H

#include <cstdint>
#include <cstdio>
#include <vector>

namespace util {
namespace image_detail {

/// Abre el propio binario para leer.  Nulo si no se puede, que aqui no es un
/// fallo: significa que el diagnostico no se va a poder dar.
FILE *open_self();

/// Lee @p bytes desde @p off.  Vacio si se sale del fichero o falla la lectura.
std::vector<unsigned char> read_at(FILE *f, uint64_t off, uint64_t bytes);

/**
 * @brief Busca una seccion y devuelve donde esta EN EL FICHERO.
 *
 * Lo unico que cambia entre PE y ELF, y por eso vive en su propia unidad.
 */
bool find_section(FILE *f, const char *name, uint64_t *off, uint64_t *size);

/// Lee un valor suelto de una posicion.  Plantilla, asi que vive aqui.
template <class T> bool peek(FILE *f, uint64_t off, T &out) {
    if (std::fseek(f, long(off), SEEK_SET) != 0) return false;
    return std::fread(&out, sizeof(T), 1, f) == 1;
}

} // namespace image_detail
} // namespace util

#endif // VESTA_SRC_SELF_IMAGE_INTERNAL_H
