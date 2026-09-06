/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/mem/x86/avx2_memset.h
 * @brief Rellenar con escrituras de 32 bytes.  Solo si la CPU tiene AVX2.
 *
 * Las mismas dos consecuencias de @c target que en @c avx2_memcpy.h, y por las
 * mismas razones: el binario no exige AVX2 para arrancar, pero esta funcion no
 * se puede meter en linea en una que no lo lleve.  Por eso
 * @c vesta_memset_inline se queda en el camino base.
 *
 * Las escrituras van por @c VESTA_MEM_STORE32 y no por una asignacion normal;
 * el motivo -- que GCC las parte en dos de 16 bytes, y lo que costaba -- esta
 * en @c util/mem/x86/x86_vec.h.
 */
#ifndef VESTA_UTIL_MEM_X86_AVX2_MEMSET_H
#define VESTA_UTIL_MEM_X86_AVX2_MEMSET_H

#include "util/mem/x86/sse2_memset.h"

#if defined(VESTA_MEM_ARCH_X86_64)

/**
 * @brief Pone @p n bytes al valor @p v con escrituras de 32.
 *
 * El escalon del bucle lo fija @c VESTA_MEM_AVX2_STEP.  Por debajo de 32
 * termina en el camino base, que ya cubre 16 y la cola.
 *
 * @param d Destino.
 * @param v Byte a repetir.
 * @param n Cuantos bytes.
 *
 * @par Hilos
 * Segura, mientras el bufer sea de quien llama.
 *
 * @code
 *   if (vesta_mem_x86_has_avx2()) vesta_mem_avx2_fill(dst, 0, n);
 * @endcode
 */
VESTA_MEM_AVX2_FN void vesta_mem_avx2_fill(uint8_t *d, uint8_t v,
                                           size_t n) VESTA_MEM_NOEXCEPT {
    /* Ver `vesta_mem_x86_splat32`: en registro, y fuera del bucle. */
    const vesta_v32 pat = vesta_mem_x86_splat32(v);
    /* ALINEAR EL DESTINO a 32 antes del bucle; ver `VESTA_MEM_STORE32A`. */
    if (n >= 64) {
        const size_t head = vesta_mem_x86_head_to_align(d, 32);
        if (head != 0) {
            VESTA_MEM_STORE32(d, pat);
            d += head;
            n -= head;
        }
    }
#if VESTA_MEM_AVX2_STEP >= 256
    /* Ocho escrituras por vuelta.  Ver la nota de `avx2_memcpy.h`: lo que
     * sobraba era contabilidad del bucle, no trabajo. */
    while (n >= 256) {
        VESTA_MEM_KEEP_LOOP(d);
        VESTA_MEM_STORE32A(d, pat); // el destino ya esta alineado
        VESTA_MEM_STORE32A(d + 32, pat);
        VESTA_MEM_STORE32A(d + 64, pat);
        VESTA_MEM_STORE32A(d + 96, pat);
        VESTA_MEM_STORE32A(d + 128, pat);
        VESTA_MEM_STORE32A(d + 160, pat);
        VESTA_MEM_STORE32A(d + 192, pat);
        VESTA_MEM_STORE32A(d + 224, pat);
        d += 256;
        n -= 256;
    }
#endif
    while (n >= 128) {
        VESTA_MEM_KEEP_LOOP(d); // que no lo cambie POR una llamada a memset
        VESTA_MEM_STORE32A(d, pat);
        VESTA_MEM_STORE32A(d + 32, pat);
        VESTA_MEM_STORE32A(d + 64, pat);
        VESTA_MEM_STORE32A(d + 96, pat);
        d += 128;
        n -= 128;
    }
    while (n >= 32) {
        VESTA_MEM_KEEP_LOOP(d);
        VESTA_MEM_STORE32(d, pat);
        d += 32;
        n -= 32;
    }
    if (n >= 16) {
        *(vesta_v16 *)(d) = *(const vesta_v16 *)(&pat);
        d += 16;
        n -= 16;
    }
    vesta_mem_fill_small(d, vesta_mem_broadcast8(v), n);
}

#endif // VESTA_MEM_ARCH_X86_64

#endif // VESTA_UTIL_MEM_X86_AVX2_MEMSET_H
