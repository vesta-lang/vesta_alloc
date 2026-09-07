/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file src/dwarf_spec.h
 * @brief Las constantes del formato DWARF.  Solo eso.
 *
 * NO ES API: no se instala y nadie de fuera debe incluirlo.  Esta aparte
 * porque es lo unico del lector que no es codigo sino VOCABULARIO -- numeros
 * que fija la especificacion --, y mezclarlo con la logica hacia que cualquier
 * busqueda dentro del lector tuviera que atravesar ciento veinte lineas de
 * tablas antes de llegar a lo que hace algo.
 */
#ifndef VESTA_SRC_DWARF_SPEC_H
#define VESTA_SRC_DWARF_SPEC_H

#include <cstdint>

namespace util {
namespace dwarf {

// ===========================================================================
//  Constantes del formato.  Solo las que se usan por nombre; el resto de
//  atributos se salta por su FORMA, que es lo que dice cuanto ocupan.
// ===========================================================================

enum : uint16_t {
    DW_TAG_compile_unit = 0x11,
    DW_TAG_inlined_subroutine = 0x1D,
    DW_TAG_subprogram = 0x2E,
};

enum : uint16_t {
    DW_AT_name = 0x03,
    DW_AT_stmt_list = 0x10,
    DW_AT_low_pc = 0x11,
    DW_AT_high_pc = 0x12,
    DW_AT_comp_dir = 0x1B,
    DW_AT_abstract_origin = 0x31,
    DW_AT_decl_file = 0x3A,
    DW_AT_decl_line = 0x3B,
    DW_AT_specification = 0x47,
    DW_AT_ranges = 0x55,
    DW_AT_call_file = 0x58,
    DW_AT_call_line = 0x59,
    DW_AT_linkage_name = 0x6E,
    DW_AT_str_offsets_base = 0x72,
    DW_AT_addr_base = 0x73,
    DW_AT_rnglists_base = 0x74,
};

enum : uint16_t {
    DW_FORM_addr = 0x01,
    DW_FORM_block2 = 0x03,
    DW_FORM_block4 = 0x04,
    DW_FORM_data2 = 0x05,
    DW_FORM_data4 = 0x06,
    DW_FORM_data8 = 0x07,
    DW_FORM_string = 0x08,
    DW_FORM_block = 0x09,
    DW_FORM_block1 = 0x0A,
    DW_FORM_data1 = 0x0B,
    DW_FORM_flag = 0x0C,
    DW_FORM_sdata = 0x0D,
    DW_FORM_strp = 0x0E,
    DW_FORM_udata = 0x0F,
    DW_FORM_ref_addr = 0x10,
    DW_FORM_ref1 = 0x11,
    DW_FORM_ref2 = 0x12,
    DW_FORM_ref4 = 0x13,
    DW_FORM_ref8 = 0x14,
    DW_FORM_ref_udata = 0x15,
    DW_FORM_indirect = 0x16,
    DW_FORM_sec_offset = 0x17,
    DW_FORM_exprloc = 0x18,
    DW_FORM_flag_present = 0x19,
    DW_FORM_strx = 0x1A,
    DW_FORM_addrx = 0x1B,
    DW_FORM_ref_sup4 = 0x1C,
    DW_FORM_strp_sup = 0x1D,
    DW_FORM_data16 = 0x1E,
    DW_FORM_line_strp = 0x1F,
    DW_FORM_ref_sig8 = 0x20,
    DW_FORM_implicit_const = 0x21,
    DW_FORM_loclistx = 0x22,
    DW_FORM_rnglistx = 0x23,
    DW_FORM_ref_sup8 = 0x24,
    DW_FORM_strx1 = 0x25,
    DW_FORM_strx2 = 0x26,
    DW_FORM_strx3 = 0x27,
    DW_FORM_strx4 = 0x28,
    DW_FORM_addrx1 = 0x29,
    DW_FORM_addrx2 = 0x2A,
    DW_FORM_addrx3 = 0x2B,
    DW_FORM_addrx4 = 0x2C,
};

/// Codigos de lista de rangos, DWARF 5 (`.debug_rnglists`).
enum : uint8_t {
    DW_RLE_end_of_list = 0x00,
    DW_RLE_base_addressx = 0x01,
    DW_RLE_startx_endx = 0x02,
    DW_RLE_startx_length = 0x03,
    DW_RLE_offset_pair = 0x04,
    DW_RLE_base_address = 0x05,
    DW_RLE_start_end = 0x06,
    DW_RLE_start_length = 0x07,
};

/// Que guarda cada columna de las tablas de ficheros de `.debug_line` v5.
enum : uint16_t {
    DW_LNCT_path = 0x1,
    DW_LNCT_directory_index = 0x2,
};

/// Opcodes estandar del programa de lineas que hacen falta nombrar.
enum : uint8_t {
    DW_LNS_copy = 0x01,
    DW_LNS_advance_pc = 0x02,
    DW_LNS_advance_line = 0x03,
    DW_LNS_set_file = 0x04,
    DW_LNS_const_add_pc = 0x08,
    DW_LNS_fixed_advance_pc = 0x09,
};

enum : uint8_t {
    DW_LNE_end_sequence = 0x01,
    DW_LNE_set_address = 0x02,
};

// ===========================================================================
//  Cursor con limites.  TODA lectura pasa por aqui.
// ===========================================================================

/**
 * @brief Un puntero que no se sale del tramo.
 *
 * Cuando una lectura no cabe se marca y a partir de ahi todo devuelve cero.
 * Asi lo de arriba puede encadenar lecturas sin comprobar una por una y
 * preguntar al final, que es lo que deja legible un lector binario sin dejar de
 * ser seguro.
 */

} // namespace dwarf
} // namespace util

#endif // VESTA_SRC_DWARF_SPEC_H
