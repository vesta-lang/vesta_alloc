/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/mem/x86/sse2_memset.h
 * @brief
 * \~english Filling with 16-byte writes.  x86's BASE path.
 * \~spanish Rellenar con escrituras de 16 bytes.  El camino BASE de x86.
 * \~
 *
 * \~english
 * The same role as @c sse2_memcpy.h on the other axis: it is guaranteed on
 * every x86-64 and it is the only one that can be inlined, so it is the one
 * @c vesta_memset_inline uses.
 *
 * \~spanish
 * Mismo papel que @c sse2_memcpy.h en el otro eje: esta garantizado en todo
 * x86-64 y es el unico que se puede meter en linea, asi que es el que usa
 * @c vesta_memset_inline.
 *
 * \~
 */
#ifndef VESTA_UTIL_MEM_X86_SSE2_MEMSET_H
#define VESTA_UTIL_MEM_X86_SSE2_MEMSET_H

#include "util/mem/mem_inline.h"
#include "util/mem/x86/x86_vec.h"

#if defined(VESTA_MEM_ARCH_X86)

/**
 * @brief
 * \~english A fill of 16 to 128 bytes WITHOUT A LOOP and without calling
 *          anybody.
 * \~spanish Relleno de 16 a 128 bytes SIN BUCLE y sin llamar a nadie.
 * \~
 *
 * \~english
 * The equivalent of @c vesta_mem_sse2_copy_le128 on the other axis, and for the
 * same reasons: no loop, no pointers to advance, no call.
 *
 * @par Threads
 * Safe, as long as the buffer belongs to the caller.
 *
 * \~spanish
 * El equivalente de @c vesta_mem_sse2_copy_le128 para el otro eje, y por las
 * mismas razones: ni bucle, ni punteros que avanzar, ni llamada.
 *
 * @par Hilos
 * Segura, mientras el bufer sea de quien llama.
 *
 * \~
 * @param d
 * \~english the destination.
 * \~spanish destino.
 * \~
 * @param pat
 * \~english the byte already repeated across the 16 lanes.
 * \~spanish el byte ya repetido en los 16 carriles.
 * \~
 * @param n
 * \~english how many bytes, from 16 to 128.
 * \~spanish cuantos bytes, de 16 a 128.
 * \~
 *
 * \~english
 * @code
 *   vesta_mem_sse2_fill_le128(d, vesta_mem_x86_splat16(0), n);
 * @endcode
 *
 * \~spanish
 * @code
 *   vesta_mem_sse2_fill_le128(d, vesta_mem_x86_splat16(0), n);
 * @endcode
 *
 * \~
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

/**
 * @brief
 * \~english Sets @p n bytes to the value @p v with 16-byte writes.
 * \~spanish Pone @p n bytes al valor @p v con escrituras de 16.
 * \~
 *
 * \~english
 * Unrolled to 64 bytes per turn for the same reason as the copy: four
 * independent writes overlap in the core, and 16 at a time leaves the write
 * unit half idle.  The tail goes through @c vesta_mem_fill_small, with no byte
 * loop -- which is also the pattern the compiler recognises in order to turn it
 * INTO a call to @c memset.
 *
 * @par Threads
 * Safe, as long as the buffer belongs to the caller.
 *
 * \~spanish
 * Desenrollado a 64 bytes por vuelta por lo mismo que la copia: cuatro
 * escrituras independientes se solapan en el nucleo, y de 16 en 16 la unidad de
 * escritura se queda a medias.  La cola va por @c vesta_mem_fill_small, sin
 * bucle de bytes -- que ademas es el patron que el compilador reconoce para
 * convertirlo EN una llamada a @c memset.
 *
 * @par Hilos
 * Segura, mientras el bufer sea de quien llama.
 *
 * \~
 * @param d
 * \~english the destination.
 * \~spanish destino.
 * \~
 * @param v
 * \~english the byte to repeat.
 * \~spanish byte a repetir.
 * \~
 * @param n
 * \~english how many bytes.
 * \~spanish cuantos bytes.
 * \~
 * @param known_align
 * \~english the alignment the caller can promise; from 16 on, the alignment
 *           prologue disappears at compile time.
 * \~spanish la alineacion que quien llama puede prometer; de 16 en adelante, el
 *           prologo de alineacion desaparece al compilar.
 * \~
 *
 * \~english
 * @code
 *   vesta_mem_sse2_fill(dst, 0, n, 1);   // without asking about the CPU
 * @endcode
 *
 * \~spanish
 * @code
 *   vesta_mem_sse2_fill(dst, 0, n, 1);   // sin preguntar por la CPU
 * @endcode
 *
 * \~
 */
VESTA_MEM_ALWAYS_INLINE void
vesta_mem_sse2_fill(uint8_t *d, uint8_t v, size_t n,
                    size_t known_align) VESTA_MEM_NOEXCEPT {
    /* El patron, EN REGISTRO y fuera del bucle.  La forma de construirlo no da
     * igual: ver `vesta_mem_x86_splat16`, que cuenta lo que costaba la otra. */
    const vesta_v16 pat = vesta_mem_x86_splat16(v);
    /* ALINEAR EL DESTINO antes del bucle; ver `VESTA_MEM_STORE16A`.  Rellenar
     * solo escribe, asi que es donde mas duele que las escrituras se partan.
     * Y solo si hace falta: con @p known_align constante >= 16 esto desaparece
     * al compilar, y el tamano sigue siendo constante para el bucle.  Ver
     * `vesta_mem_sse2_copy`, que lo cuenta entero. */
    if (known_align < 16 && n >= 32) {
        const size_t head = vesta_mem_x86_head_to_align(d, 16);
        if (head != 0) {
            *(vesta_v16 *)(d) = pat;
            d += head;
            n -= head;
        }
    }
    VESTA_MEM_NO_UNROLL
    while (n >= 64) {
        VESTA_MEM_KEEP_LOOP(d); // que no lo cambie POR una llamada a memset
        VESTA_MEM_STORE16A(d, pat); // el destino ya esta alineado
        VESTA_MEM_STORE16A(d + 16, pat);
        VESTA_MEM_STORE16A(d + 32, pat);
        VESTA_MEM_STORE16A(d + 48, pat);
        d += 64;
        n -= 64;
    }
    VESTA_MEM_NO_UNROLL
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
