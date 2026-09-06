/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/vesta_memset.h
 * @brief RELLENAR memoria sin pasar por la biblioteca C.  Es lo unico que hay
 *        que incluir para rellenar; el resto de @c util/mem es interior.
 *
 * DOS ENTRADAS, y la diferencia es SI PUEDE HABER UNA LLAMADA:
 *
 *   - @c vesta_memset         despacha por CPU.  Es la de uso general.
 *   - @c vesta_memset_inline  NO llama a nadie, nunca.
 *
 * La UNICA diferencia entre las dos es el camino de AVX2, y desaparece si el
 * binario se compila con @c -mavx2.  Ver @c util/vesta_memcpy.h, que lo cuenta
 * entero, y @c VESTA_MEM_TARGET_AVX2 en @c util/mem/mem_config.h.
 *
 * Valen en C y en C++.  En C++ estan ademas como @c util::vesta_memset y
 * @c util::vesta_memset_inline.
 *
 * Lo que cuesta cada camino, en @c bench/bench_memset.cpp.
 */
#ifndef VESTA_UTIL_VESTA_MEMSET_H
#define VESTA_UTIL_VESTA_MEMSET_H

#include "util/mem/mem_config.h"
#include "util/mem/mem_inline.h"

#if defined(VESTA_MEM_ARCH_X86)
#include "util/mem/x86/erms_memset.h"
#include "util/mem/x86/sse2_memset.h"
#if defined(VESTA_MEM_ARCH_X86_64)
#include "util/mem/x86/avx2_memset.h"
#include "util/mem/x86/x86_cpu.h"
#endif
#elif VESTA_ALLOC_FREESTANDING
#include "util/mem/generic/scalar_memset.h"
#endif

/**
 * @brief El despacho, con la eleccion de caminos como parametro.
 *
 * @p wide llega SIEMPRE como constante desde las dos entradas publicas, y como
 * esto se mete en linea a la fuerza, el camino que no se admite desaparece del
 * todo -- no queda ni la rama.
 *
 * Los cortes son los mismos que en la copia y por las mismas razones, con UN
 * umbral distinto para @c rep @c stosb: rellenar solo escribe, asi que el bucle
 * vectorial propio aguanta mas antes de que compense la instruccion.
 *
 * @param dst  Destino.
 * @param v    Byte a repetir.
 * @param n    Cuantos bytes.
 * @param wide Si se admite el camino de AVX2.
 *
 * @par Hilos
 * Segura, mientras el bufer sea de quien llama.
 *
 * @code
 *   vesta_mem_fill_dispatch(d, 0, n, 1);   // con todo
 * @endcode
 */
VESTA_MEM_ALWAYS_INLINE void
vesta_mem_fill_dispatch(void *dst, uint8_t v, size_t n,
                        int wide) VESTA_MEM_NOEXCEPT {
#if VESTA_ALLOC_FREESTANDING
    uint8_t *d = (uint8_t *)dst;

    if (n < 16) {
        vesta_mem_fill_small(d, vesta_mem_broadcast8(v), n);
        return;
    }
#if defined(VESTA_MEM_ARCH_X86)
    if (n <= 128) {
        vesta_mem_sse2_fill_le128(d, vesta_mem_x86_splat16(v), n);
        return;
    }
    if (n >= VESTA_MEM_ERMS_MIN_FILL && vesta_mem_x86_has_erms()) {
        vesta_mem_erms_fill(d, v, n);
        return;
    }
#if defined(VESTA_MEM_ARCH_X86_64)
    if (wide && n >= VESTA_MEM_AVX2_MIN && vesta_mem_x86_has_avx2()) {
        vesta_mem_avx2_fill(d, v, n);
        return;
    }
#endif
    vesta_mem_sse2_fill(d, v, n);
#else
    vesta_mem_scalar_fill(d, v, n);
#endif
    (void)wide;
#else
    (void)wide;
    memset(dst, v, n); // respaldo: compilador sin las extensiones
#endif
}

/**
 * @brief Pone @p n bytes al valor @p v eligiendo el mejor camino de esta CPU.
 *
 * @param dst Destino.
 * @param v   Byte a repetir.
 * @param n   Cuantos bytes.  Con 0 no hace nada.
 *
 * @par Hilos
 * Segura, mientras el bufer sea de quien llama.
 *
 * @code
 *   vesta_memset(bloque, 0, bytes);   // zerificar
 * @endcode
 */
VESTA_MEM_ALWAYS_INLINE void vesta_memset(void *dst, uint8_t v,
                                          size_t n) VESTA_MEM_NOEXCEPT {
    vesta_mem_fill_dispatch(dst, v, n, 1);
}

/**
 * @brief Igual, pero con la GARANTIA de que no se llama a nadie.
 *
 * Con tamano CONSTANTE lo expande el compilador; si no, el mismo despacho sin
 * el camino de AVX2 -- salvo que el binario ya se compile con @c -mavx2, donde
 * tampoco hay nada a lo que renunciar --.  @c rep @c stosb SI lo usa: es una
 * instruccion, no una funcion.
 *
 * @param dst Destino.
 * @param v   Byte a repetir.
 * @param n   Cuantos bytes.
 *
 * @par Hilos
 * Segura, mientras el bufer sea de quien llama.
 *
 * @code
 *   vesta_memset_inline(&cabecera, 0, sizeof cabecera);
 * @endcode
 */
VESTA_MEM_ALWAYS_INLINE void vesta_memset_inline(void *dst, uint8_t v,
                                                 size_t n) VESTA_MEM_NOEXCEPT {
#if VESTA_ALLOC_FREESTANDING
    if (__builtin_constant_p(n)) {
        __builtin_memset(dst, v, n);
        return;
    }
#if defined(VESTA_MEM_TARGET_AVX2)
    vesta_mem_fill_dispatch(dst, v, n, 1);
#else
    vesta_mem_fill_dispatch(dst, v, n, 0);
#endif
#else
    memset(dst, v, n);
#endif
}

#ifdef __cplusplus
namespace util {

/// @copydoc ::vesta_memset
[[gnu::always_inline]] inline void vesta_memset(void *dst, uint8_t v,
                                                size_t n) noexcept {
    ::vesta_memset(dst, v, n);
}

/// @copydoc ::vesta_memset_inline
[[gnu::always_inline]] inline void vesta_memset_inline(void *dst, uint8_t v,
                                                       size_t n) noexcept {
    ::vesta_memset_inline(dst, v, n);
}

} // namespace util
#endif // __cplusplus

#endif // VESTA_UTIL_VESTA_MEMSET_H
