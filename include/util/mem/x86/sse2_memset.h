/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/mem/x86/sse2_memset.h
 * @brief Rellenar con escrituras de 16 bytes.  El camino BASE de x86.
 *
 * Mismo papel que @c sse2_memcpy.h en el otro eje: esta garantizado en todo
 * x86-64 y es el unico que se puede meter en linea, asi que es el que usa
 * @c vesta_memset_inline.
 */
#ifndef VESTA_UTIL_MEM_X86_SSE2_MEMSET_H
#define VESTA_UTIL_MEM_X86_SSE2_MEMSET_H

#include "util/mem/mem_inline.h"
#include "util/mem/x86/x86_vec.h"

#if defined(VESTA_MEM_ARCH_X86)

/**
 * @brief Pone @p n bytes al valor @p v con escrituras de 16.
 *
 * Desenrollado a 64 bytes por vuelta por lo mismo que la copia: cuatro
 * escrituras independientes se solapan en el nucleo, y de 16 en 16 la unidad de
 * escritura se queda a medias.  La cola va por @c vesta_mem_fill_small, sin
 * bucle de bytes -- que ademas es el patron que el compilador reconoce para
 * convertirlo EN una llamada a @c memset.
 *
 * @param d Destino.
 * @param v Byte a repetir.
 * @param n Cuantos bytes.
 *
 * @par Hilos
 * Segura, mientras el bufer sea de quien llama.
 *
 * @code
 *   vesta_mem_sse2_fill(dst, 0, n);   // sin preguntar por la CPU
 * @endcode
 */
/**
 * @brief Relleno de 16 a 128 bytes SIN BUCLE y sin llamar a nadie.
 *
 * El equivalente de @c vesta_mem_sse2_copy_le128 para el otro eje, y por las
 * mismas razones: ni bucle, ni punteros que avanzar, ni llamada.
 *
 * @param d   Destino.
 * @param pat El byte ya repetido en los 16 carriles.
 * @param n   Cuantos bytes, de 16 a 128.
 *
 * @par Hilos
 * Segura, mientras el bufer sea de quien llama.
 *
 * @code
 *   vesta_mem_sse2_fill_le128(d, vesta_mem_x86_splat16(0), n);
 * @endcode
 */
VESTA_MEM_ALWAYS_INLINE void
vesta_mem_sse2_fill_le128(uint8_t *d, vesta_v16 pat,
                          size_t n) VESTA_MEM_NOEXCEPT {
    if (n > 64) {
        *(vesta_v16 *)(d) = pat;
        *(vesta_v16 *)(d + 16) = pat;
        *(vesta_v16 *)(d + 32) = pat;
        *(vesta_v16 *)(d + 48) = pat;
        *(vesta_v16 *)(d + n - 64) = pat;
        *(vesta_v16 *)(d + n - 48) = pat;
        *(vesta_v16 *)(d + n - 32) = pat;
        *(vesta_v16 *)(d + n - 16) = pat;
        return;
    }
    if (n > 32) {
        *(vesta_v16 *)(d) = pat;
        *(vesta_v16 *)(d + 16) = pat;
        *(vesta_v16 *)(d + n - 32) = pat;
        *(vesta_v16 *)(d + n - 16) = pat;
        return;
    }
    *(vesta_v16 *)(d) = pat;
    *(vesta_v16 *)(d + n - 16) = pat;
}

VESTA_MEM_ALWAYS_INLINE void vesta_mem_sse2_fill(uint8_t *d, uint8_t v,
                                                 size_t n) VESTA_MEM_NOEXCEPT {
    /* El patron, EN REGISTRO y fuera del bucle.  La forma de construirlo no da
     * igual: ver `vesta_mem_x86_splat16`, que cuenta lo que costaba la otra. */
    const vesta_v16 pat = vesta_mem_x86_splat16(v);
    /* ALINEAR EL DESTINO antes del bucle; ver `VESTA_MEM_STORE16A`.  Rellenar
     * solo escribe, asi que es donde mas duele que las escrituras se partan. */
    if (n >= 32) {
        const size_t head = vesta_mem_x86_head_to_align(d, 16);
        if (head != 0) {
            *(vesta_v16 *)(d) = pat;
            d += head;
            n -= head;
        }
    }
    while (n >= 64) {
        VESTA_MEM_KEEP_LOOP(d); // que no lo cambie POR una llamada a memset
        VESTA_MEM_STORE16A(d, pat); // el destino ya esta alineado
        VESTA_MEM_STORE16A(d + 16, pat);
        VESTA_MEM_STORE16A(d + 32, pat);
        VESTA_MEM_STORE16A(d + 48, pat);
        d += 64;
        n -= 64;
    }
    while (n >= 16) {
        VESTA_MEM_KEEP_LOOP(d);
        *(vesta_v16 *)(d) = pat;
        d += 16;
        n -= 16;
    }
    vesta_mem_fill_small(d, vesta_mem_broadcast8(v), n);
}

#endif // VESTA_MEM_ARCH_X86

#endif // VESTA_UTIL_MEM_X86_SSE2_MEMSET_H
