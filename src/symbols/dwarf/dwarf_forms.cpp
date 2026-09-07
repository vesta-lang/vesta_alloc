/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file src/dwarf_forms.cpp
 * @brief El VALOR de un atributo, sea cual sea la forma en que venga.
 *
 * Una pieza del lector de DWARF.  Lo que le pide a las demas y lo que les
 * ofrece esta escrito en `dwarf_internal.h`.
 */

#include "dwarf_internal.h"


namespace util {
namespace dwarf {

const char *str_from_section(const std::vector<unsigned char> &sec,
                             uint64_t off) {
    if (off >= sec.size()) return nullptr;
    const char *s = reinterpret_cast<const char *>(sec.data() + off);
    /* Tiene que acabar DENTRO de la seccion: una cadena sin terminador es una
     * lectura fuera del tramo disfrazada de dato. */
    const size_t max = sec.size() - size_t(off);
    if (::strnlen(s, max) == max) return nullptr;
    return s;
}

const char *str_by_index(const Store &st, const CuCtx &cu, uint64_t idx) {
    const uint64_t at = cu.str_offsets_base + idx * cu.offset_size;
    if (at + cu.offset_size > st.str_offsets.size()) return nullptr;
    /* DOS COPIAS DE TAMANO CONSTANTE en vez de una de tamano variable, y no es
     * por el aviso del compilador aunque tambien lo quite: es que el formato
     * solo admite estos dos anchos, y escribirlo asi lo DICE.  Con un tamano
     * que llega dentro de una estructura y por referencia, el compilador tiene
     * que dar por alcanzable el camino de dieciseis bytes de `vesta_memcpy` --
     * que sobre un destino de ocho escribiria fuera -- y avisa con razon.
     *
     * Hubo aqui una cota (`> sizeof(uint64_t)`) que lo callaba mientras esto
     * vivia en un fichero de mil setecientas lineas: al partirlo, el
     * compilador ve menos contexto y volvio a avisar.  Eso ya dice que la cota
     * no era la respuesta -- dependia de cuanto alcanzara a ver el
     * optimizador --, y esto no depende de nada. */
    const unsigned char *p = st.str_offsets.data() + at;
    uint64_t off = 0;
    if (cu.offset_size == 8) {
        vesta_memcpy(&off, p, 8);
    } else if (cu.offset_size == 4) {
        uint32_t v32 = 0;
        vesta_memcpy(&v32, p, 4);
        off = v32;
    } else {
        return nullptr; // el formato no tiene otros anchos
    }
    return str_from_section(st.str, off);
}

uint64_t addr_by_index(const Store &st, const CuCtx &cu, uint64_t idx) {
    const uint64_t at = cu.addr_base + idx * cu.addr_size;
    if (at + cu.addr_size > st.addr.size()) return 0;
    /* Constantes, por lo mismo que en `str_by_index`.  `header_is_sane` ya
     * rechaza cualquier otro ancho antes de llegar aqui, pero una funcion no
     * deberia apoyarse en algo que garantiza otra que no se ve desde aqui. */
    const unsigned char *p = st.addr.data() + at;
    uint64_t v = 0;
    if (cu.addr_size == 8) {
        vesta_memcpy(&v, p, 8);
    } else if (cu.addr_size == 4) {
        uint32_t v32 = 0;
        vesta_memcpy(&v32, p, 4);
        v = v32;
    } else {
        return 0;
    }
    return v;
}

/**
 * @brief Lee (o salta) un valor de la forma dada.
 *
 * Es el corazon del lector: si una forma se mide mal, todo lo que venga detras
 * se lee corrido y sale basura con forma de estructura.  Por eso estan todas las
 * de DWARF 2 a 5 aunque solo se usen unas pocas -- una forma desconocida no se
 * puede saltar, porque no se sabe cuanto ocupa, y ahi lo unico correcto es
 * parar.
 */
bool read_form(Cursor &c, uint16_t form, const AttrSpec &spec, const Store &st,
               const CuCtx &cu, Value &out) {
    switch (form) {
        case DW_FORM_addr: out.num = c.un(cu.addr_size); return c.ok();
        case DW_FORM_block2: c.skip(size_t(c.u16())); return c.ok();
        case DW_FORM_block4: c.skip(size_t(c.u32())); return c.ok();
        case DW_FORM_data2: out.num = c.u16(); return c.ok();
        case DW_FORM_data4: out.num = c.u32(); return c.ok();
        case DW_FORM_data8: out.num = c.u64(); return c.ok();
        case DW_FORM_data16: c.skip(16); return c.ok();
        case DW_FORM_string: out.str = c.cstr(); return c.ok();
        case DW_FORM_block:
        case DW_FORM_exprloc: c.skip(size_t(c.uleb())); return c.ok();
        case DW_FORM_block1: c.skip(size_t(c.u8())); return c.ok();
        case DW_FORM_data1: out.num = c.u8(); return c.ok();
        case DW_FORM_flag: out.num = c.u8(); return c.ok();
        case DW_FORM_sdata:
            out.snum = c.sleb();
            out.num = uint64_t(out.snum);
            return c.ok();
        case DW_FORM_strp:
            out.num = c.un(cu.offset_size);
            out.str = str_from_section(st.str, out.num);
            return c.ok();
        case DW_FORM_line_strp:
            out.num = c.un(cu.offset_size);
            out.str = str_from_section(st.line_str, out.num);
            return c.ok();
        case DW_FORM_udata: out.num = c.uleb(); return c.ok();
        case DW_FORM_ref_addr: out.num = c.un(cu.offset_size); return c.ok();
        /* `ref1..ref_udata` son RELATIVAS al principio de su unidad.  Se
         * devuelven tal cual y quien las use les suma la base: convertirlas
         * aqui obligaria a pasar esa base a una funcion que no la necesita para
         * nada mas. */
        case DW_FORM_ref1: out.num = c.u8(); return c.ok();
        case DW_FORM_ref2: out.num = c.u16(); return c.ok();
        case DW_FORM_ref4: out.num = c.u32(); return c.ok();
        case DW_FORM_ref8: out.num = c.u64(); return c.ok();
        case DW_FORM_ref_udata: out.num = c.uleb(); return c.ok();
        case DW_FORM_ref_sig8: c.skip(8); return c.ok();
        case DW_FORM_ref_sup4: c.skip(4); return c.ok();
        case DW_FORM_ref_sup8: c.skip(8); return c.ok();
        case DW_FORM_strp_sup: c.skip(cu.offset_size); return c.ok();
        case DW_FORM_indirect: {
            const uint16_t real = uint16_t(c.uleb());
            if (!c.ok() || real == DW_FORM_indirect) return false;
            return read_form(c, real, spec, st, cu, out);
        }
        case DW_FORM_sec_offset: out.num = c.un(cu.offset_size); return c.ok();
        case DW_FORM_flag_present: out.num = 1; return c.ok();
        case DW_FORM_implicit_const:
            out.snum = spec.implicit;
            out.num = uint64_t(spec.implicit);
            return c.ok();
        case DW_FORM_strx:
            out.num = c.uleb();
            out.str = str_by_index(st, cu, out.num);
            return c.ok();
        case DW_FORM_strx1:
        case DW_FORM_strx2:
        case DW_FORM_strx3:
        case DW_FORM_strx4: {
            const unsigned n = unsigned(form - DW_FORM_strx1) + 1;
            out.num = c.un(n);
            out.str = str_by_index(st, cu, out.num);
            return c.ok();
        }
        case DW_FORM_addrx:
            out.num = addr_by_index(st, cu, c.uleb());
            return c.ok();
        case DW_FORM_addrx1:
        case DW_FORM_addrx2:
        case DW_FORM_addrx3:
        case DW_FORM_addrx4: {
            const unsigned n = unsigned(form - DW_FORM_addrx1) + 1;
            out.num = addr_by_index(st, cu, c.un(n));
            return c.ok();
        }
        case DW_FORM_loclistx:
        case DW_FORM_rnglistx: out.num = c.uleb(); return c.ok();
        default: return false;
    }
}

// ===========================================================================
//  Rangos: dos formatos incompatibles para la misma pregunta
// ===========================================================================

/**
 * @brief Si @p addr cae en la lista de rangos de DWARF 4 (`.debug_ranges`).
 *
 * Pares (inicio, fin) RELATIVOS a una base, cerrados por un par de ceros.  Un
 * inicio con todos los bits a uno no es un rango: cambia la base para los que
 * siguen, y saltarselo desplazaria todo lo demas.
 */

} // namespace dwarf
} // namespace util
