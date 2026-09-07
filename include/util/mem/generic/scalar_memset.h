/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/mem/generic/scalar_memset.h
 * @brief
 * \~english Filling word by word.  The companion of
 *          @c generic/scalar_memcpy.h, and for the same reasons.
 * \~spanish Rellenar de palabra en palabra.  El companero de
 *          @c generic/scalar_memcpy.h, y por las mismas razones.
 * \~
 */
#ifndef VESTA_UTIL_MEM_GENERIC_SCALAR_MEMSET_H
#define VESTA_UTIL_MEM_GENERIC_SCALAR_MEMSET_H

#include "util/mem/mem_inline.h"

#if VESTA_ALLOC_FREESTANDING

/**
 * @brief
 * \~english Sets @p n bytes to the value @p v, word by word.
 * \~spanish Pone @p n bytes al valor @p v, de palabra en palabra.
 * \~
 *
 * \~english
 * @par Threads
 * Safe, as long as the buffer belongs to the caller.
 *
 * \~spanish
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
 *
 * \~english
 * @code
 *   vesta_mem_scalar_fill(dst, 0, n);
 * @endcode
 *
 * \~spanish
 * @code
 *   vesta_mem_scalar_fill(dst, 0, n);
 * @endcode
 *
 * \~
 */
VESTA_MEM_ALWAYS_INLINE void vesta_mem_scalar_fill(uint8_t *d, uint8_t v,
                                                   size_t n)
    VESTA_MEM_NOEXCEPT {
    const uint64_t pat = vesta_mem_broadcast8(v);
    VESTA_MEM_NO_UNROLL
    while (n >= sizeof(size_t)) {
        VESTA_MEM_KEEP_LOOP(d); // que no lo cambie POR una llamada a memset
        __builtin_memcpy(d, &pat, sizeof(size_t)); // constante: se expande
        d += sizeof(size_t);
        n -= sizeof(size_t);
    }
    vesta_mem_fill_small(d, pat, n);
}

#endif // VESTA_ALLOC_FREESTANDING

#endif // VESTA_UTIL_MEM_GENERIC_SCALAR_MEMSET_H
