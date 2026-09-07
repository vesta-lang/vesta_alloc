/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file src/self_symbols_internal.h
 * @brief Lo que comparten las dos mitades de la lectura de simbolos.
 *
 * NO ES API: no se instala y nadie de fuera debe incluirlo.  La lectura se
 * parte por la misma costura que la de secciones -- lo que depende del FORMATO
 * y lo que no --, y las dos mitades necesitan la misma tabla y los mismos
 * ayudantes de fichero.
 */
#ifndef VESTA_SRC_SELF_SYMBOLS_INTERNAL_H
#define VESTA_SRC_SELF_SYMBOLS_INTERNAL_H

#include "util/mem/vesta_memcpy.h"

#include <cstdint>
#include <string>
#include <vector>

namespace util {
namespace symbols_detail {

/// Un simbolo: donde empieza (desplazamiento desde la base) y como se llama.
struct Sym {
    uint64_t rva;
    uint32_t name; ///< indice dentro del blob de nombres
};

/**
 * @brief El tramo que ocupa una funcion.  Desplazamientos, como todo aqui.
 *
 * POR QUE HACE FALTA, teniendo ya los simbolos.  Porque un simbolo dice donde
 * EMPIEZA una funcion y no donde acaba, asi que con la tabla de simbolos sola
 * la unica respuesta posible a "de quien es esta direccion" es "del simbolo
 * anterior mas cercano" -- y eso acierta mientras la direccion caiga dentro, y
 * miente en silencio en cuanto cae en un hueco o en una funcion sin nombre.
 *
 * Con el tramo se puede DECIR que no se sabe, que es la unica respuesta
 * correcta cuando no se sabe.
 */
struct Func {
    uint32_t begin;
    uint32_t end; ///< el primero que ya NO es de esta funcion
};

struct Table {
    std::vector<Sym> syms;   ///< ordenados por `rva`
    std::vector<Func> funcs; ///< ordenados por `begin`
    std::string names;       ///< los nombres seguidos, separados por '\0'
};

/// La ruta del ejecutable que esta corriendo.
std::string self_path();

/// Lee un fichero entero.  Vacio si no se puede: esto es un diagnostico, y no
/// poder darlo NO es motivo para que nada falle.
std::vector<unsigned char> slurp(const std::string &path);

/**
 * @brief Llena @p t a partir de los bytes del ejecutable.
 *
 * Es LO UNICO que cambia entre un PE y un ELF, y por eso es la unica funcion
 * que la mitad del formato le ofrece a la otra.  Falso si el fichero no se
 * pudo entender, que no es un fallo: significa que no habra nombres.
 */
bool build_table(const std::vector<unsigned char> &bytes, Table &t);

template <class T> bool read_at(const std::vector<unsigned char> &b, size_t off,
                                T &out) {
    if (off + sizeof(T) > b.size()) return false;
    vesta_memcpy(&out, b.data() + off, sizeof(T));
    return true;
}

} // namespace symbols_detail
} // namespace util

#endif // VESTA_SRC_SELF_SYMBOLS_INTERNAL_H
