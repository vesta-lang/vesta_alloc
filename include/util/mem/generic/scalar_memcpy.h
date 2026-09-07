/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/mem/generic/scalar_memcpy.h
 * @brief
 * \~english Copying word by word.  What gets used where there is no
 *          architecture folder.
 * \~spanish Copiar de palabra en palabra.  Lo que se usa donde no hay carpeta
 *          de arquitectura.
 * \~
 *
 * \~english
 * It is NOT a call to the C library, and that is the point: on a target that
 * does not have its folder yet, going at the speed of a word copy beats
 * depending on a library existing.
 *
 * Adding an architecture means creating its folder with the same two functions
 * (@c copy and @c fill) and a branch in the dispatcher; while that is not
 * there, this answers.
 *
 * \~spanish
 * NO es una llamada a la biblioteca C, y esa es la idea: en un objetivo que
 * todavia no tiene su carpeta, mas vale ir a la velocidad de una copia de
 * palabras que depender de que exista una libreria.
 *
 * Anadir una arquitectura es crear su carpeta con las mismas dos funciones
 * (@c copy y @c fill) y una rama en el despachador; mientras no este, esto
 * responde.
 *
 * \~
 */
#ifndef VESTA_UTIL_MEM_GENERIC_SCALAR_MEMCPY_H
#define VESTA_UTIL_MEM_GENERIC_SCALAR_MEMCPY_H

#include "util/mem/mem_inline.h"

#if VESTA_ALLOC_FREESTANDING

/**
 * @brief
 * \~english Copies @p n bytes word by word.
 * \~spanish Copia @p n bytes de palabra en palabra.
 * \~
 *
 * \~english
 * @par Threads
 * Safe, as long as the buffers belong to the caller.
 *
 * \~spanish
 * @par Hilos
 * Segura, mientras los bufers sean de quien llama.
 *
 * \~
 * @param d
 * \~english the destination.
 * \~spanish destino.
 * \~
 * @param s
 * \~english the source.  It must not overlap @p d.
 * \~spanish origen.  No puede solapar con @p d.
 * \~
 * @param n
 * \~english how many bytes.
 * \~spanish cuantos bytes.
 * \~
 *
 * \~english
 * @code
 *   vesta_mem_scalar_copy(dst, src, n);
 * @endcode
 *
 * \~spanish
 * @code
 *   vesta_mem_scalar_copy(dst, src, n);
 * @endcode
 *
 * \~
 */
VESTA_MEM_ALWAYS_INLINE void
vesta_mem_scalar_copy(uint8_t *d, const uint8_t *s,
                      size_t n) VESTA_MEM_NOEXCEPT {
    VESTA_MEM_NO_UNROLL
    while (n >= sizeof(size_t)) {
        VESTA_MEM_KEEP_LOOP(d); // que no lo cambie POR una llamada a memcpy
        __builtin_memcpy(d, s, sizeof(size_t)); // constante: se expande inline
        d += sizeof(size_t);
        s += sizeof(size_t);
        n -= sizeof(size_t);
    }
    vesta_mem_copy_small(d, s, n);
}

/**
 * @brief
 * \~english Copies forwards with no tricks, for the overlap with the
 *          destination behind the source.
 * \~spanish Copia hacia adelante sin trucos, para el solape con el destino por
 *          detras del origen.
 * \~
 *
 * \~english
 * It is the same thing as @c vesta_mem_scalar_copy: the scalar path aligns
 * nothing, so it was already safe with overlap.  It exists so that the
 * dispatcher can ask for the same thing on both architectures -- on x86 there
 * IS a version that aligns, and that one is no good here.  See
 * @c vesta_mem_sse2_copy_forward.
 *
 * @par Threads
 * Safe, as long as the buffers belong to the caller.
 *
 * \~spanish
 * Es lo mismo que @c vesta_mem_scalar_copy: el camino escalar no alinea nada,
 * asi que ya era seguro con solape.  Existe para que el despachador pueda pedir
 * lo mismo en las dos arquitecturas -- en x86 SI hay una version que alinea, y
 * esa no vale aqui --.  Ver @c vesta_mem_sse2_copy_forward.
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
 * \~english the source.  It may overlap, with @p d at LOWER addresses.
 * \~spanish origen.  Puede solapar, con @p d en direcciones MENORES.
 * \~
 * @param n
 * \~english how many bytes.
 * \~spanish cuantos bytes.
 * \~
 *
 * \~english
 * @code
 *   vesta_mem_scalar_copy_forward(v, v + 3, n);
 * @endcode
 *
 * \~spanish
 * @code
 *   vesta_mem_scalar_copy_forward(v, v + 3, n);
 * @endcode
 *
 * \~
 */
VESTA_MEM_ALWAYS_INLINE void
vesta_mem_scalar_copy_forward(uint8_t *d, const uint8_t *s,
                              size_t n) VESTA_MEM_NOEXCEPT {
    vesta_mem_scalar_copy(d, s, n);
}

/**
 * @brief
 * \~english Copies BACKWARDS, for the overlap with the destination in front.
 * \~spanish Copia HACIA ATRAS, para el solape con el destino por delante.
 * \~
 *
 * \~english
 * Word by word and NOT byte by byte, for the same reason as the SSE2 version: a
 * loop of bytes going backwards is the pattern the compiler turns into a call
 * to @c memmove, which is exactly what cannot be here.  The tail goes through
 * @c vesta_mem_copy_small, which works with overlap because it reads both ends
 * before writing either.
 *
 * @par Threads
 * Safe, as long as the buffers belong to the caller.
 *
 * \~spanish
 * De palabra en palabra y NO byte a byte, por la misma razon que la version de
 * SSE2: un bucle de bytes hacia atras es el patron que el compilador convierte
 * en una llamada a @c memmove, que es justo lo que no puede haber aqui.  La
 * cola va por @c vesta_mem_copy_small, que vale con solape porque lee los dos
 * extremos antes de escribir ninguno.
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
 * \~english the source.
 * \~spanish origen.
 * \~
 * @param n
 * \~english how many bytes.
 * \~spanish cuantos bytes.
 * \~
 *
 * \~english
 * @code
 *   vesta_mem_scalar_copy_backward(v + 1, v, n);
 * @endcode
 *
 * \~spanish
 * @code
 *   vesta_mem_scalar_copy_backward(v + 1, v, n);
 * @endcode
 *
 * \~
 */
VESTA_MEM_ALWAYS_INLINE void
vesta_mem_scalar_copy_backward(uint8_t *d, const uint8_t *s,
                               size_t n) VESTA_MEM_NOEXCEPT {
    VESTA_MEM_NO_UNROLL
    while (n >= sizeof(size_t)) {
        n -= sizeof(size_t);
        __builtin_memcpy(d + n, s + n, sizeof(size_t)); // constante: en linea
    }
    vesta_mem_copy_small(d, s, n);
}

#endif // VESTA_ALLOC_FREESTANDING

#endif // VESTA_UTIL_MEM_GENERIC_SCALAR_MEMCPY_H
