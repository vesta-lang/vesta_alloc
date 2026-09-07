/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file src/dwarf_lines.cpp
 * @brief El programa de lineas: de una direccion a fichero y linea.
 *
 * Una pieza del lector de DWARF.  Lo que le pide a las demas y lo que les
 * ofrece esta escrito en `dwarf_internal.h`.
 */

#include "dwarf_internal.h"

#include <string>

namespace util {
namespace dwarf {


/// Junta directorio y nombre como lo haria quien los escribio.
std::string join_path(const std::string &dir, const char *name) {
    if (name == nullptr) return std::string();
    const std::string n(name);
    /* Absoluta se queda como esta: en Windows puede venir `C:/...` y en Linux
     * empezar por `/`. */
    if (!n.empty() && (n[0] == '/' || n[0] == '\\')) return n;
    if (n.size() > 1 && n[1] == ':') return n;
    if (dir.empty()) return n;
    return dir + "/" + n;
}

/**
 * @brief Lee la CABECERA del programa de lineas: la tabla de ficheros.
 *
 * Es lo que convierte el numero que guarda `DW_AT_call_file` en una ruta.  Y es
 * el sitio donde DWARF 5 se separa mas de DWARF 4: alli eran dos listas de
 * cadenas terminadas en vacio y aqui es una tabla con descriptores de columna,
 * cada una con su forma.  Encima cambia desde que numero se cuenta -- 1 en la
 * version vieja, 0 en la nueva --, asi que leer una con las reglas de la otra
 * no falla: devuelve el fichero de al lado.
 */
LineFiles read_line_header(const std::vector<unsigned char> &line,
                           uint64_t off, const Store &st, const CuCtx &owner) {
    LineFiles out;
    if (off >= line.size()) return out;
    Cursor c(line.data() + off, line.size() - size_t(off));

    const InitialLength il = read_initial_length(c);
    const uint64_t unit_end = uint64_t(c.p - line.data()) + il.length;
    const uint16_t version = c.u16();
    if (!c.ok() || version < 2 || version > 5) return out;

    CuCtx lc;
    lc.version = version;
    lc.offset_size = il.offset_size;
    lc.addr_size = owner.addr_size;
    lc.str_offsets_base = owner.str_offsets_base;
    lc.addr_base = owner.addr_base;

    if (version >= 5) {
        c.u8(); // address_size
        c.u8(); // segment_selector_size
    }
    const uint64_t header_len = c.un(il.offset_size);
    const uint64_t prog_off = uint64_t(c.p - line.data()) + header_len;

    out.min_inst = c.u8();
    if (version >= 4) out.max_ops = c.u8();
    out.default_is_stmt = c.u8() != 0;
    out.line_base = int8_t(c.u8());
    out.line_range = c.u8();
    out.opcode_base = c.u8();
    for (unsigned i = 1; i < out.opcode_base && c.ok(); ++i)
        out.std_lens.push_back(c.u8());
    if (!c.ok()) return out;

    std::vector<std::string> dirs;
    if (version <= 4) {
        /* Directorios: cadenas seguidas, cerradas por una vacia.  El indice 0
         * es el directorio de compilacion, que no aparece en la lista. */
        dirs.emplace_back(owner.comp_dir != nullptr ? owner.comp_dir : "");
        for (;;) {
            const char *s = c.cstr();
            if (!c.ok() || s == nullptr || *s == '\0') break;
            dirs.emplace_back(s);
        }
        /* Ficheros: nombre, directorio, fecha y tamano.  Se cuenta DESDE UNO,
         * asi que el hueco cero se rellena para que el indice sea directo. */
        out.files.emplace_back();
        for (;;) {
            const char *s = c.cstr();
            if (!c.ok() || s == nullptr || *s == '\0') break;
            const uint64_t dir = c.uleb();
            c.uleb(); // fecha
            c.uleb(); // tamano
            out.files.emplace_back(
                join_path(dir < dirs.size() ? dirs[size_t(dir)] : std::string(),
                          s));
        }
    } else {
        /* DWARF 5: primero se declara QUE columnas trae cada fila y con que
         * forma, y luego las filas.  Hay que leer todas las columnas aunque
         * solo interesen dos, porque si no la siguiente fila se lee corrida. */
        for (int table = 0; table < 2; ++table) {
            const uint8_t ncols = c.u8();
            SmallVector<uint16_t, 8> types;
            SmallVector<uint16_t, 8> forms;
            for (unsigned i = 0; i < ncols && c.ok(); ++i) {
                types.push_back(uint16_t(c.uleb()));
                forms.push_back(uint16_t(c.uleb()));
            }
            const uint64_t nrows = c.uleb();
            if (!c.ok()) return out;
            for (uint64_t r = 0; r < nrows && c.ok(); ++r) {
                const char *path = nullptr;
                uint64_t dir = 0;
                for (unsigned i = 0; i < ncols; ++i) {
                    AttrSpec spec;
                    spec.form = forms[i];
                    Value v;
                    if (!read_form(c, forms[i], spec, st, lc, v)) return out;
                    if (types[i] == DW_LNCT_path) path = v.str;
                    else if (types[i] == DW_LNCT_directory_index) dir = v.num;
                }
                if (table == 0)
                    dirs.emplace_back(path != nullptr ? path : "");
                else
                    out.files.emplace_back(join_path(
                        dir < dirs.size() ? dirs[size_t(dir)] : std::string(),
                        path));
            }
        }
    }

    out.program_off = prog_off;
    out.end_off = unit_end;
    out.ok = c.ok() && !out.files.empty();
    return out;
}

/**
 * @brief La linea EXACTA de una direccion, corriendo el programa de lineas.
 *
 * Hace falta para el marco mas interno y solo para ese: los de fuera saben
 * donde llamaron por `DW_AT_call_line`, pero el de dentro es donde esta el
 * codigo de verdad y eso solo lo dice esta tabla.  Sin ella se podria dar la
 * linea donde la funcion se DECLARO, que casi nunca es donde reserva.
 *
 * La maquina es la del estandar, sin florituras: se avanza y cada vez que se
 * emite una fila se mira si la direccion cae entre la anterior y esta.
 */
bool line_for_addr(const std::vector<unsigned char> &line, const LineFiles &h,
                   uint64_t addr, uint64_t &out_file, uint64_t &out_line) {
    /* Las tres condiciones, y las tres importan: el final tiene que ir DESPUES
     * del principio y los dos dentro de la seccion.  Con una cabecera rara la
     * resta de abajo se daba la vuelta -- son sin signo -- y salia un tramo de
     * miles de millones de bytes que el cursor daba por bueno. */
    if (!h.ok || line.empty() || h.program_off >= line.size() ||
        h.end_off > line.size() || h.end_off <= h.program_off)
        return false;
    Cursor c(line.data() + h.program_off, size_t(h.end_off - h.program_off));

    uint64_t address = 0, file = 1, ln = 1;
    uint64_t prev_addr = 0, prev_file = 1, prev_line = 1;
    bool have_prev = false;
    bool found = false;

    const auto emit = [&](bool end_seq) {
        if (have_prev && addr >= prev_addr && addr < address) {
            out_file = prev_file;
            out_line = prev_line;
            found = true;
        }
        if (end_seq) {
            have_prev = false;
        } else {
            prev_addr = address;
            prev_file = file;
            prev_line = ln;
            have_prev = true;
        }
    };

    while (c.ok() && !found) {
        const uint8_t op = c.u8();
        if (!c.ok()) break;
        if (op == 0) { // extendido
            const uint64_t len = c.uleb();
            const unsigned char *next = c.p + len;
            const uint8_t sub = c.u8();
            /* El largo del opcode extendido tambien sale del fichero, y aqui se
             * usa como cuantos bytes tiene la direccion.  Mas de ocho no cabe en
             * el entero y el montaje desplazaria mas de 63, que no esta
             * definido; se ignora la orden y se sigue, porque el resto del
             * programa de lineas puede seguir valiendo. */
            if (sub == DW_LNE_set_address && len >= 2 && len <= 9) {
                address = c.un(unsigned(len - 1));
            } else if (sub == DW_LNE_end_sequence) {
                emit(true);
                address = 0;
                file = 1;
                ln = 1;
            }
            if (next > c.end) break;
            c.p = next;
        } else if (op < h.opcode_base) {
            switch (op) {
                case DW_LNS_copy: emit(false); break;
                case DW_LNS_advance_pc: address += c.uleb() * h.min_inst; break;
                case DW_LNS_advance_line: ln = uint64_t(int64_t(ln) + c.sleb()); break;
                case DW_LNS_set_file: file = c.uleb(); break;
                case DW_LNS_const_add_pc:
                    address += uint64_t((255 - h.opcode_base) / h.line_range) *
                               h.min_inst;
                    break;
                case DW_LNS_fixed_advance_pc: address += c.u16(); break;
                default: {
                    /* Cualquier otro estandar se salta por su numero de
                     * argumentos, que la cabecera declara.  Por eso se guardan:
                     * un opcode nuevo no rompe el recorrido. */
                    const unsigned idx = op - 1;
                    const unsigned n = idx < h.std_lens.size() ? h.std_lens[idx] : 0;
                    for (unsigned i = 0; i < n; ++i) c.uleb();
                    break;
                }
            }
        } else {
            const unsigned adj = op - h.opcode_base;
            address += uint64_t(adj / h.line_range) * h.min_inst;
            ln = uint64_t(int64_t(ln) + h.line_base + int(adj % h.line_range));
            emit(false);
        }
    }
    return found;
}

// ===========================================================================
//  El indice de unidades
// ===========================================================================

/// Lee `.debug_aranges`, que es un indice YA HECHO de direccion a unidad.

} // namespace dwarf
} // namespace util
