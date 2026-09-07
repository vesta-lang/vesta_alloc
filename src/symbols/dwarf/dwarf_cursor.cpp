/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file src/dwarf_cursor.cpp
 * @brief Las abreviaturas: que atributos trae cada clase de entrada.
 *
 * Una pieza del lector de DWARF.  Lo que le pide a las demas y lo que les
 * ofrece esta escrito en `dwarf_internal.h`.
 */

#include "dwarf_internal.h"

namespace util {
namespace dwarf {

std::vector<Abbrev> read_abbrev(const std::vector<unsigned char> &sec,
                                uint64_t off) {
    std::vector<Abbrev> t;
    if (off >= sec.size()) return t;
    Cursor c(sec.data() + off, sec.size() - size_t(off));
    for (;;) {
        const uint64_t code = c.uleb();
        if (!c.ok() || code == 0) break;
        Abbrev a;
        a.tag = uint16_t(c.uleb());
        a.children = c.u8() != 0;
        a.used = true;
        for (;;) {
            AttrSpec s;
            s.at = uint16_t(c.uleb());
            s.form = uint16_t(c.uleb());
            if (s.form == DW_FORM_implicit_const) s.implicit = c.sleb();
            if (!c.ok()) break;
            if (s.at == 0 && s.form == 0) break;
            a.attrs.push_back(s);
        }
        if (!c.ok() || code >= kMaxAbbrevCode) break;
        if (t.size() <= code) t.resize(size_t(code) + 1);
        t[size_t(code)] = std::move(a);
    }
    return t;
}

// ===========================================================================
//  El almacen: lo que se lee UNA vez y se queda
// ===========================================================================

/**
 * @brief Que la cabecera de la unidad diga cosas posibles.
 *
 * EL TAMANO DE DIRECCION VIENE CRUDO DEL FICHERO -- un byte, sin mas -- y se
 * usa despues para decidir cuantos bytes se leen y se copian.  Con un DWARF
 * corrupto puede valer 255, y entonces: se copian 255 bytes a un `uint64_t`, y
 * el montaje byte a byte desplaza mas de 63, que no esta definido.  Ninguna de
 * las dos cosas avisa; las dos dan basura o algo peor.
 *
 * Lo unico legal es cuatro u ocho.  Comprobarlo AQUI, donde se lee, es lo que
 * permite que todo lo de abajo confie en el sin volver a preguntarse -- y es
 * ademas lo que hace que `vesta_memcpy` no reciba nunca un tamano que no quepa
 * en el destino, que era el aviso que salio en GCC 15 --.
 *
 * La version tambien: de la 2 a la 5 es lo que existe.  Un numero fuera de ahi
 * no es una unidad, es que se leyo en el sitio equivocado.
 */

} // namespace dwarf
} // namespace util
