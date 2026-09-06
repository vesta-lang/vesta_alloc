/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/mem/mem_inline.h
 * @brief Lo que se resuelve SIN llamar a nadie y sin bucle: menos de 16 bytes.
 *
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
 */
#ifndef VESTA_UTIL_MEM_INLINE_H
#define VESTA_UTIL_MEM_INLINE_H

#include "util/mem/mem_config.h"

#if VESTA_ALLOC_FREESTANDING

/**
 * @brief Un byte repetido en los ocho de una palabra.
 *
 * @param v Byte a repetir.
 * @return  El mismo byte en las ocho posiciones.
 *
 * @par Hilos
 * Segura.  Solo calcula.
 *
 * @code
 *   const uint64_t pat = vesta_mem_broadcast8(0xFF);   // 0xFFFF...FF
 * @endcode
 */
VESTA_MEM_ALWAYS_INLINE uint64_t vesta_mem_broadcast8(uint8_t v)
    VESTA_MEM_NOEXCEPT {
    return (uint64_t)v * 0x0101010101010101ull;
}

/**
 * @brief Copia de MENOS de 16 bytes sin llamar a nadie y sin bucle.
 *
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
 * @param d Destino.
 * @param s Origen.  Puede solapar con @p d.
 * @param n Cuantos bytes, de 0 a 15.  Con 0 no hace nada.
 *
 * @par Hilos
 * Segura, mientras los bufers sean de quien llama.
 *
 * @code
 *   vesta_mem_copy_small(dst, src, 7);   // ni bucle ni llamada
 * @endcode
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
 * @brief El equivalente de @c vesta_mem_copy_small para rellenar.
 *
 * Mismos bloques solapados y misma ausencia de bucle y de llamada.  El patron
 * llega YA repetido en los ocho bytes (ver @c vesta_mem_broadcast8) para que
 * las escrituras de 8, 4 y 2 salgan todas de la misma palabra.
 *
 * @param d   Destino.
 * @param pat El byte repetido en los ocho de una palabra.
 * @param n   Cuantos bytes, de 0 a 15.  Con 0 no hace nada.
 *
 * @par Hilos
 * Segura, mientras el bufer sea de quien llama.
 *
 * @code
 *   vesta_mem_fill_small(dst, vesta_mem_broadcast8(0), 5);
 * @endcode
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
