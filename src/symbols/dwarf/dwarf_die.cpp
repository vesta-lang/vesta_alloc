/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file src/dwarf_die.cpp
 * @brief Leer entradas del arbol de la unidad.
 *
 * Una pieza del lector de DWARF.  Lo que le pide a las demas y lo que les
 * ofrece esta escrito en `dwarf_internal.h`.
 */

#include "dwarf_internal.h"


namespace util {
namespace dwarf {


/**
 * @brief Lee una entrada entera y se queda con lo que hace falta.
 * @return false si la entrada no se pudo leer -- y entonces el recorrido se
 *         para, porque a partir de ahi ya no se sabe donde empieza la siguiente.
 */
bool read_die(Cursor &c, const std::vector<Abbrev> &abbrevs, const Store &st,
              const CuCtx &cu, DieInfo &d, bool &is_null) {
    is_null = false;
    const uint64_t code = c.uleb();
    if (!c.ok()) return false;
    if (code == 0) { // cierra una lista de hijos
        is_null = true;
        return true;
    }
    if (code >= abbrevs.size() || !abbrevs[size_t(code)].used) return false;
    const Abbrev &a = abbrevs[size_t(code)];
    d = DieInfo{};
    d.tag = a.tag;
    d.children = a.children;

    for (size_t i = 0; i < a.attrs.size(); ++i) {
        const AttrSpec &s = a.attrs[i];
        Value v;
        if (!read_form(c, s.form, s, st, cu, v)) return false;
        switch (s.at) {
            case DW_AT_low_pc: d.low = v.num; d.has_low = true; break;
            case DW_AT_high_pc:
                d.high = v.num;
                d.has_high = true;
                /* `high_pc` es una DIRECCION cuando la forma es de direccion y
                 * un TAMANO cuando es de dato.  Confundirlo da funciones que
                 * acaban antes de empezar, o que cubren medio binario. */
                if (s.form != DW_FORM_addr) d.high += d.low;
                break;
            case DW_AT_ranges:
                d.ranges = v.num;
                d.has_ranges = true;
                d.ranges_is_index = s.form == DW_FORM_rnglistx;
                break;
            case DW_AT_name: d.name = v.str; break;
            case DW_AT_linkage_name: d.linkage = v.str; break;
            case DW_AT_abstract_origin:
                d.abstract_origin = v.num;
                d.has_origin = true;
                break;
            case DW_AT_specification:
                d.specification = v.num;
                d.has_spec = true;
                break;
            case DW_AT_call_file: d.call_file = v.num; d.has_call = true; break;
            case DW_AT_call_line: d.call_line = v.num; d.has_call = true; break;
            case DW_AT_decl_file: d.decl_file = v.num; d.has_decl = true; break;
            case DW_AT_decl_line: d.decl_line = v.num; d.has_decl = true; break;
            case DW_AT_stmt_list:
                d.stmt_list = v.num;
                d.has_stmt_list = true;
                break;
            case DW_AT_comp_dir: d.comp_dir = v.str; break;
            case DW_AT_str_offsets_base: d.str_offsets_base = v.num; break;
            case DW_AT_addr_base: d.addr_base = v.num; break;
            case DW_AT_rnglists_base: d.rnglists_base = v.num; break;
            default: break;
        }
    }
    /* `high_pc` sin `low_pc` delante puede llegar antes en el orden de la
     * abreviatura; si paso, se corrige aqui. */
    if (d.has_high && d.has_low && d.high < d.low) d.high += d.low;
    return c.ok();
}

/**
 * @brief Lee la entrada raiz de la unidad DOS VECES, y hace falta.
 *
 * EL PROBLEMA.  En DWARF 5 muchos valores de la raiz no son el dato sino un
 * INDICE: `DW_AT_low_pc` puede venir como `DW_FORM_addrx`, que es el numero de
 * una direccion dentro de `.debug_addr`.  Para resolverlo hace falta
 * `DW_AT_addr_base`... que esta en esa misma entrada, y el productor la pone
 * donde quiere.  Clang la pone DESPUES.
 *
 * Leyendo de una pasada, `low_pc` se resuelve con base cero y sale una
 * direccion que no es.  Y no falla: da una unidad que dice cubrir un tramo
 * equivocado, asi que las direcciones de verdad no caen en ninguna y todo el
 * binario parece no tener informacion de depuracion.  Medido con Clang 19: cero
 * marcos en todo, mientras que con GCC funcionaba -- porque GCC escribe
 * `low_pc` como direccion literal y ahi no hay indice que resolver --.
 *
 * La segunda pasada ya tiene las bases, que son las unicas que NO pueden venir
 * indexadas (son desplazamientos de seccion), asi que la primera siempre puede
 * sacarlas.
 */
bool read_cu_root(Cursor &c, const std::vector<Abbrev> &abbrevs,
                  const Store &st, CuCtx &cu, DieInfo &root) {
    const unsigned char *start = c.p;
    bool is_null = false;
    if (!read_die(c, abbrevs, st, cu, root, is_null) || is_null) return false;

    cu.str_offsets_base = root.str_offsets_base;
    cu.addr_base = root.addr_base;
    cu.rnglists_base = root.rnglists_base;

    if (cu.version >= 5 &&
        (root.addr_base != 0 || root.str_offsets_base != 0 ||
         root.rnglists_base != 0)) {
        c.p = start;
        c.bad = false;
        if (!read_die(c, abbrevs, st, cu, root, is_null) || is_null) return false;
    }
    cu.low_pc = root.low;
    cu.comp_dir = root.comp_dir;
    cu.stmt_list = root.stmt_list;
    cu.has_stmt_list = root.has_stmt_list;
    return true;
}

/// Si @p addr cae dentro de la entrada, por rango simple o por lista.

/**
 * @brief El nombre de una entrada, siguiendo a donde haga falta.
 *
 * Una entrada inlineada casi nunca lleva el nombre: lleva un `abstract_origin`
 * que apunta a la entrada ABSTRACTA de esa funcion, que es la que lo tiene.  Y
 * la abstracta puede a su vez remitir a la `specification` -- lo que pasa con
 * un metodo definido fuera de su clase --.  Sin seguir las dos, la mitad de la
 * cadena sale sin nombre.
 *
 * El limite de saltos no es paranoia gratuita: son datos de un fichero, y un
 * ciclo ahi colgaria el informe.
 */
const char *die_name(const std::vector<unsigned char> &info, uint64_t cu_base,
                     const std::vector<Abbrev> &abbrevs, const Store &st,
                     const CuCtx &cu, const DieInfo &start) {
    const DieInfo *d = &start;
    DieInfo tmp;
    for (int hop = 0; hop < 8; ++hop) {
        if (d->linkage != nullptr) return d->linkage;
        if (d->name != nullptr) return d->name;
        uint64_t next = 0;
        if (d->has_origin) {
            next = d->abstract_origin;
        } else if (d->has_spec) {
            next = d->specification;
        } else {
            return nullptr;
        }
        /* Las referencias de dentro de la unidad son relativas a ella. */
        const uint64_t at = cu_base + next;
        if (at >= info.size()) return nullptr;
        Cursor c(info.data() + at, info.size() - size_t(at));
        bool is_null = false;
        if (!read_die(c, abbrevs, st, cu, tmp, is_null) || is_null)
            return nullptr;
        d = &tmp;
    }
    return nullptr;
}

// ===========================================================================

} // namespace dwarf
} // namespace util
