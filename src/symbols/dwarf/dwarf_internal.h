/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file src/dwarf_internal.h
 * @brief El almacen del lector y lo que cada pieza le ofrece a las demas.
 *
 * NO ES API: no se instala y nadie de fuera debe incluirlo.  El lector estaba
 * en un solo fichero de mil setecientas lineas con todo dentro de un namespace
 * anonimo; partirlo obliga a poner por escrito que le pide cada pieza a las
 * otras, que es justamente lo que un namespace anonimo permitia no decir.
 */
#ifndef VESTA_SRC_DWARF_INTERNAL_H
#define VESTA_SRC_DWARF_INTERNAL_H

#include "dwarf_types.h"

#include "util/symbols/self_dwarf.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace util {
namespace dwarf {

struct Store {
    std::vector<unsigned char> abbrev, str, line_str, str_offsets, addr;
    std::vector<unsigned char> ranges, rnglists;
    /* La tabla de lineas se queda AQUI y no se pide en cada consulta.  Son 12,8
     * MiB en este binario, y el informe pregunta una vez por sitio: leerla
     * dentro de la consulta eran seis gigas de lectura por informe, y cuando la
     * reserva fallaba el vector salia vacio y se leia igual -- que es como se
     * llega a un cuelgue en vez de a un mensaje. */
    std::vector<unsigned char> line;
    std::vector<CuRange> index; ///< ordenado por `lo`
    uint64_t link_base = 0;
    uint64_t info_size = 0;
    size_t units = 0;
    bool usable = false;

    /**
     * @brief Los textos que se entregan hacia fuera.
     *
     * NO se usa el almacen compartido del proyecto (`util/name_pool.h`), y no
     * es por gusto: su propia cabecera avisa de que toma un cerrojo y no es
     * para llamarlo por elemento, y ademas esto corre desde un manejador de
     * SALIDA -- donde un estatico de otra unidad ya puede estar destruido --.
     * Se probo y reventaba justo ahi, dentro de su tabla.
     *
     * Un `deque` y no un `vector`: lo que se entrega son PUNTEROS a estas
     * cadenas, y un `vector` los invalidaria al crecer.  Este almacen no se
     * destruye nunca -- se cede a un puntero atomico y ahi se queda --, asi que
     * lo que se reparte vale mientras el proceso.
     */
    mutable std::deque<std::string> pool;
    mutable std::mutex pool_mu;

    /**
     * @brief Lo ya resuelto, por direccion.
     *
     * POR QUE AQUI Y NO EN QUIEN PREGUNTA.  Porque el coste esta aqui: medido
     * con VTune sobre una compilacion de verdad, recorrer los DIEs se lleva
     * 2,0 de los 4,5 segundos, y `find_cu` -- que es una busqueda binaria --
     * no aparece.  Cada consulta encuentra su unidad enseguida y luego se
     * recorre sus DIEs ENTERA, desde cero.
     *
     * Y se preguntaba dos veces por la misma direccion: el informe de texto
     * resuelve cada sitio y el volcado a CSV los vuelve a resolver todos.  Con
     * 491 sitios eran ~982 recorridos donde bastan 491.  Poniendo el memo aqui
     * lo arregla para los dos, y para cualquier consumidor que venga despues
     * sin que tenga que acordarse.
     *
     * Los marcos guardan PUNTEROS a `pool`, que no se destruye nunca, asi que
     * lo memorizado sigue valiendo mientras el proceso.
     */
    struct Memo {
        uint64_t addr;   ///< direccion de ENLACE, ya convertida
        unsigned first;  ///< indice en `memo_frames`
        unsigned count;
    };
    mutable std::vector<Memo> memo;          ///< ordenado por `addr`
    mutable std::vector<SelfFrame> memo_frames;
    mutable std::mutex memo_mu;

    /**
     * @brief Cuantas direcciones distintas se recuerdan.
     *
     * Acotado a proposito: la foto de sitios tiene 4.096 entradas, asi que con
     * esto sobra para el caso al que sirve.  Pasado el tope se deja de anadir
     * y se sigue resolviendo como siempre -- mas lento, nunca mal --, en vez
     * de crecer sin freno en un proceso que quiza pregunte por millones de
     * direcciones distintas.
     */
    static constexpr size_t kMemoMax = 8192;

    /**
     * @brief Cuantos marcos se guardan por direccion.
     *
     * Se resuelve SIEMPRE pidiendo este maximo, aunque quien pregunte quiera
     * menos: lo memorizado tiene que servir tambien al siguiente, que puede
     * traer mas sitio.  Guardar lo que pidio el primero dejaria cadenas
     * cortadas a su medida, y la de dieciocho marcos que se midio en este
     * binario saldria de tres para todos los demas.
     */
    static constexpr unsigned kMemoFrames = 64;

    const char *intern(const std::string &s) const {
        std::lock_guard<std::mutex> lk(pool_mu);
        pool.emplace_back(s);
        return pool.back().c_str();
    }
};

/* `inline` porque esto es una cabecera y la incluyen siete unidades: sin ella
 * cada una traeria su propia definicion y el enlazador se quejaria de siete
 * almacenes con el mismo nombre.  Variable en linea de C++17, que es
 * exactamente el caso para el que existe. */
inline std::atomic<Store *> g_store{nullptr};

/// Lo de la unidad que se esta leyendo y que hace falta para interpretar formas

// ===========================================================================
//  Lo que cada pieza ofrece.  Una linea por funcion que cruza de un fichero a
//  otro; lo que no aparece aqui es privado de su unidad.
// ===========================================================================

// -- dwarf_cursor.cpp: abreviaturas ----------------------------------------
std::vector<Abbrev> read_abbrev(const std::vector<unsigned char> &sec,
                                uint64_t off);

// -- dwarf_forms.cpp: cadenas, direcciones y el valor de un atributo -------
const char *str_from_section(const std::vector<unsigned char> &sec,
                             uint64_t off);
const char *str_by_index(const Store &st, const CuCtx &cu, uint64_t idx);
uint64_t addr_by_index(const Store &st, const CuCtx &cu, uint64_t idx);
bool read_form(Cursor &c, uint16_t form, const AttrSpec &spec, const Store &st,
               const CuCtx &cu, Value &out);

// -- dwarf_ranges.cpp: si una direccion cae dentro -------------------------
bool in_ranges_v4(const Store &st, const CuCtx &cu, uint64_t off,
                  uint64_t addr);
bool in_ranges_v5(const Store &st, const CuCtx &cu, uint64_t off,
                  uint64_t addr);
bool ranges_offset(const Store &st, const CuCtx &cu, const DieInfo &d,
                   uint64_t &out);
bool die_contains(const Store &st, const CuCtx &cu, const DieInfo &d,
                  uint64_t addr);

// -- dwarf_die.cpp: leer entradas ------------------------------------------
bool read_die(Cursor &c, const std::vector<Abbrev> &abbrevs, const Store &st,
              const CuCtx &cu, DieInfo &d, bool &is_null);
bool read_cu_root(Cursor &c, const std::vector<Abbrev> &abbrevs,
                  const Store &st, CuCtx &cu, DieInfo &root);
const char *die_name(const std::vector<unsigned char> &info, uint64_t cu_base,
                     const std::vector<Abbrev> &abbrevs, const Store &st,
                     const CuCtx &cu, const DieInfo &start);

// -- dwarf_lines.cpp: el programa de lineas --------------------------------
std::string join_path(const std::string &dir, const char *name);
LineFiles read_line_header(const std::vector<unsigned char> &line,
                           uint64_t off, const Store &st, const CuCtx &owner);
bool line_for_addr(const std::vector<unsigned char> &line, const LineFiles &h,
                   uint64_t addr, uint64_t &out_file, uint64_t &out_line);

// -- dwarf_index.cpp: que unidad cubre que direccion -----------------------
void index_from_aranges(const std::vector<unsigned char> &ar, Store &st);
void index_by_scanning(Store &st);
const Store *store();
uint64_t find_cu(const Store &st, uint64_t addr);

/// el indice, que necesita TODOS los tramos de cada unidad.
template <class F>
void each_range(const Store &st, const CuCtx &cu, const DieInfo &d, F fn) {
    if (d.has_low && d.has_high && d.high > d.low) {
        fn(d.low, d.high);
        return;
    }
    if (!d.has_ranges) return;
    uint64_t off = 0;
    if (!ranges_offset(st, cu, d, off)) return;

    if (cu.version >= 5) {
        if (off >= st.rnglists.size()) return;
        Cursor c(st.rnglists.data() + off, st.rnglists.size() - size_t(off));
        uint64_t base = cu.low_pc;
        for (;;) {
            const uint8_t kind = c.u8();
            if (!c.ok() || kind == DW_RLE_end_of_list) return;
            switch (kind) {
                case DW_RLE_base_addressx: base = addr_by_index(st, cu, c.uleb()); break;
                case DW_RLE_startx_endx: {
                    const uint64_t a = addr_by_index(st, cu, c.uleb());
                    const uint64_t b = addr_by_index(st, cu, c.uleb());
                    fn(a, b);
                    break;
                }
                case DW_RLE_startx_length: {
                    const uint64_t a = addr_by_index(st, cu, c.uleb());
                    fn(a, a + c.uleb());
                    break;
                }
                case DW_RLE_offset_pair: {
                    const uint64_t a = c.uleb();
                    const uint64_t b = c.uleb();
                    fn(base + a, base + b);
                    break;
                }
                case DW_RLE_base_address: base = c.un(cu.addr_size); break;
                case DW_RLE_start_end: {
                    const uint64_t a = c.un(cu.addr_size);
                    fn(a, c.un(cu.addr_size));
                    break;
                }
                case DW_RLE_start_length: {
                    const uint64_t a = c.un(cu.addr_size);
                    fn(a, a + c.uleb());
                    break;
                }
                default: return;
            }
            if (!c.ok()) return;
        }
    }

    if (off >= st.ranges.size()) return;
    Cursor c(st.ranges.data() + off, st.ranges.size() - size_t(off));
    uint64_t base = cu.low_pc;
    const uint64_t all_ones = (cu.addr_size >= 8)
                                  ? ~uint64_t(0)
                                  : ((uint64_t(1) << (cu.addr_size * 8)) - 1);
    for (;;) {
        const uint64_t a = c.un(cu.addr_size);
        const uint64_t b = c.un(cu.addr_size);
        if (!c.ok() || (a == 0 && b == 0)) return;
        if (a == all_ones) {
            base = b;
            continue;
        }
        fn(base + a, base + b);
    }
}

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

} // namespace dwarf
} // namespace util

#endif // VESTA_SRC_DWARF_INTERNAL_H
