/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/mem/mem_inline.h
 * @brief
 * \~english What gets resolved WITHOUT calling anybody and without a loop:
 *          under 16 bytes.
 * \~spanish Lo que se resuelve SIN llamar a nadie y sin bucle: menos de 16
 *          bytes.
 * \~
 *
 * \~english
 * It depends on no micro-ISA -- these are word moves, which exist on any
 * architecture -- so it lives outside the architecture folders and every path
 * shares it as its tail.
 *
 * WHY IT IS A SEPARATE FILE AND NOT THE START OF THE VECTOR LOOP.  Because this
 * is where the real gain is: below 16 bytes the WHOLE cost of a @c memcpy is
 * the call, and that is by far the common case in an allocator.  Measured, 0.34
 * ns against 1.53 for the C library -- 4.5x -- and the difference is not in the
 * copy's code but in not having left here.
 *
 * @par Threads
 * Everything is safe from any thread: there is no state, only the caller's
 * buffers.
 *
 * \~spanish
 * No depende de ninguna micro-ISA -- son movimientos de palabra, que existen en
 * cualquier arquitectura --, asi que vive fuera de las carpetas de arquitectura
 * y lo comparten todos los caminos como cola.
 *
 * POR QUE ES UN FICHERO APARTE Y NO EL PRINCIPIO DEL BUCLE VECTORIAL.  Porque
 * es donde esta la ganancia real: por debajo de 16 bytes el coste ENTERO de un
 * @c memcpy es la llamada, y ese es el caso comun con diferencia en un
 * asignador.  Medido, 0,34 ns contra 1,53 de la biblioteca C -- 4,5x -- y la
 * diferencia no esta en el codigo de la copia, sino en no haber salido de aqui.
 *
 * @par Hilos
 * Todo es seguro desde cualquier hilo: no hay estado, solo los bufers de quien
 * llama.
 *
 * \~
 */
#ifndef VESTA_UTIL_MEM_INLINE_H
#define VESTA_UTIL_MEM_INLINE_H

#include "util/mem/mem_config.h"

#if VESTA_ALLOC_FREESTANDING

/**
 * @brief
 * \~english One byte repeated across the eight of a word.
 * \~spanish Un byte repetido en los ocho de una palabra.
 * \~
 *
 * \~english
 * @par Threads
 * Safe.  It only computes.
 *
 * \~spanish
 * @par Hilos
 * Segura.  Solo calcula.
 *
 * \~
 * @param v
 * \~english the byte to repeat.
 * \~spanish byte a repetir.
 * \~
 * @return
 * \~english the same byte in all eight positions.
 * \~spanish el mismo byte en las ocho posiciones.
 * \~
 *
 * \~english
 * @code
 *   const uint64_t pat = vesta_mem_broadcast8(0xFF);   // 0xFFFF...FF
 * @endcode
 *
 * \~spanish
 * @code
 *   const uint64_t pat = vesta_mem_broadcast8(0xFF);   // 0xFFFF...FF
 * @endcode
 *
 * \~
 */
VESTA_MEM_ALWAYS_INLINE uint64_t vesta_mem_broadcast8(uint8_t v)
    VESTA_MEM_NOEXCEPT {
    return (uint64_t)v * 0x0101010101010101ull;
}

/**
 * @brief
 * \~english A copy of UNDER 16 bytes without calling anybody and without a
 *          loop.
 * \~spanish Copia de MENOS de 16 bytes sin llamar a nadie y sin bucle.
 * \~
 *
 * \~english
 * The technique is OVERLAPPING BLOCKS: two accesses of the widest size that
 * fits, one at the start and one at the end, cover any length in their range.
 * They overlap in the middle, and that does not matter: the same data gets
 * written twice.  No loop means no branch per byte, and also none of the
 * pattern @c -ftree-loop-distribute-patterns recognises in order to turn it
 * INTO a call to @c memcpy -- exactly the one being avoided.
 *
 * @c __builtin_memcpy with a CONSTANT size is not a call: the compiler expands
 * it into a move.  It is the only way to move eight bytes from an unaligned
 * address without dropping to assembly.
 *
 * IT WORKS WITH OVERLAP, and that is NOT a coincidence: it reads both ends
 * BEFORE writing either, and between them they cover the whole range, so what
 * it writes cannot trample what it has left to read.  The tails of
 * @c vesta_mem_sse2_copy_backward and of its scalar equivalent depend on that
 * property, which means that if somebody reorders this into interleaved loads
 * and stores, they break @c vesta_memmove without it showing in the ordinary
 * copy.
 *
 * @par Threads
 * Safe, as long as the buffers belong to the caller.
 *
 * \~spanish
 * La tecnica es la de BLOQUES SOLAPADOS: dos accesos del mayor ancho que quepa,
 * uno al principio y otro al final, cubren cualquier longitud de su rango.  Se
 * solapan en el medio, y eso da igual: se escribe dos veces el mismo dato.  Sin
 * bucle significa sin una rama por byte, y ademas sin el patron que
 * @c -ftree-loop-distribute-patterns reconoce para convertirlo EN una llamada a
 * @c memcpy -- justo la que se esta evitando.
 *
 * @c __builtin_memcpy con tamano CONSTANTE no es una llamada: el compilador lo
 * expande a un movimiento.  Es la unica forma de mover ocho bytes de una
 * direccion sin alinear sin bajar a ensamblador.
 *
 * VALE CON SOLAPE, y eso NO es casualidad: lee los dos extremos ANTES de
 * escribir ninguno, y entre los dos cubren el rango entero, asi que lo que
 * escribe no puede pisar lo que le falta por leer.  De esa propiedad dependen
 * las colas de @c vesta_mem_sse2_copy_backward y de su equivalente escalar, o
 * sea que si alguien reordena esto en cargas y escrituras intercaladas, rompe
 * @c vesta_memmove sin que se note en la copia normal.
 *
 * @par Hilos
 * Segura, mientras los bufers sean de quien llama.
 *
 * \~
 * @param d
 * \~english the destination.
 * \~spanish destino.
 * \~
 * @param s
 * \~english the source.  It may overlap @p d.
 * \~spanish origen.  Puede solapar con @p d.
 * \~
 * @param n
 * \~english how many bytes, from 0 to 15.  With 0 it does nothing.
 * \~spanish cuantos bytes, de 0 a 15.  Con 0 no hace nada.
 * \~
 *
 * \~english
 * @code
 *   vesta_mem_copy_small(dst, src, 7);   // no loop and no call
 * @endcode
 *
 * \~spanish
 * @code
 *   vesta_mem_copy_small(dst, src, 7);   // ni bucle ni llamada
 * @endcode
 *
 * \~
 */
VESTA_MEM_ALWAYS_INLINE void vesta_mem_copy_small(uint8_t *d, const uint8_t *s,
                                                  size_t n) VESTA_MEM_NOEXCEPT {
    if (n >= 8) {
        uint64_t a, b;
        __builtin_memcpy(&a, s, 8);
        __builtin_memcpy(&b, s + n - 8, 8);
        __builtin_memcpy(d, &a, 8);
        __builtin_memcpy(d + n - 8, &b, 8);
        return;
    }
    if (n >= 4) {
        uint32_t a, b;
        __builtin_memcpy(&a, s, 4);
        __builtin_memcpy(&b, s + n - 4, 4);
        __builtin_memcpy(d, &a, 4);
        __builtin_memcpy(d + n - 4, &b, 4);
        return;
    }
    if (n >= 2) {
        uint16_t a, b;
        __builtin_memcpy(&a, s, 2);
        __builtin_memcpy(&b, s + n - 2, 2);
        __builtin_memcpy(d, &a, 2);
        __builtin_memcpy(d + n - 2, &b, 2);
        return;
    }
    if (n == 1) *d = *s;
}

/**
 * @brief
 * \~english The equivalent of @c vesta_mem_copy_small for filling.
 * \~spanish El equivalente de @c vesta_mem_copy_small para rellenar.
 * \~
 *
 * \~english
 * The same overlapping blocks and the same absence of a loop and of a call.
 * The pattern arrives ALREADY repeated across the eight bytes (see
 * @c vesta_mem_broadcast8) so that the writes of 8, 4 and 2 all come out of the
 * same word.
 *
 * @par Threads
 * Safe, as long as the buffer belongs to the caller.
 *
 * \~spanish
 * Mismos bloques solapados y misma ausencia de bucle y de llamada.  El patron
 * llega YA repetido en los ocho bytes (ver @c vesta_mem_broadcast8) para que
 * las escrituras de 8, 4 y 2 salgan todas de la misma palabra.
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
 * \~english the byte repeated across the eight of a word.
 * \~spanish el byte repetido en los ocho de una palabra.
 * \~
 * @param n
 * \~english how many bytes, from 0 to 15.  With 0 it does nothing.
 * \~spanish cuantos bytes, de 0 a 15.  Con 0 no hace nada.
 * \~
 *
 * \~english
 * @code
 *   vesta_mem_fill_small(dst, vesta_mem_broadcast8(0), 5);
 * @endcode
 *
 * \~spanish
 * @code
 *   vesta_mem_fill_small(dst, vesta_mem_broadcast8(0), 5);
 * @endcode
 *
 * \~
 */
VESTA_MEM_ALWAYS_INLINE void vesta_mem_fill_small(uint8_t *d, uint64_t pat,
                                                  size_t n) VESTA_MEM_NOEXCEPT {
    if (n >= 8) {
        __builtin_memcpy(d, &pat, 8);
        __builtin_memcpy(d + n - 8, &pat, 8);
        return;
    }
    if (n >= 4) {
        __builtin_memcpy(d, &pat, 4);
        __builtin_memcpy(d + n - 4, &pat, 4);
        return;
    }
    if (n >= 2) {
        __builtin_memcpy(d, &pat, 2);
        __builtin_memcpy(d + n - 2, &pat, 2);
        return;
    }
    if (n == 1) *d = (uint8_t)pat;
}

#endif // VESTA_ALLOC_FREESTANDING

#endif // VESTA_UTIL_MEM_INLINE_H
