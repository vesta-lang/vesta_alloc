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
vesta_mem_fill_dispatch(void *dst, uint8_t v, size_t n, int wide,
                        size_t known_align) VESTA_MEM_NOEXCEPT {
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
    vesta_mem_sse2_fill(d, v, n, known_align);
#else
    vesta_mem_scalar_fill(d, v, n);
#endif
    (void)wide;
    (void)known_align;
#else
    (void)wide;
    (void)known_align;
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
    /* Desde C no se sabe nada de la alineacion; desde C++ hay una version con
     * tipo que si la sabe. */
    vesta_mem_fill_dispatch(dst, v, n, 1, 1);
}

/**
 * @brief El mismo relleno, pero en una funcion de VERDAD: una llamada y ya.
 *
 * Ver @c vesta_memcpy_noinline, que lo explica: con tamanos grandes la
 * expansion en linea son cientos de instrucciones en cada sitio, y ahi sale
 * mas a cuenta la llamada.  Esta para poder ELEGIR; el camino en linea sigue.
 *
 * @param dst Destino.
 * @param v   Byte a repetir.
 * @param n   Cuantos bytes.
 *
 * @par Hilos
 * Segura, mientras el bufer sea de quien llama.
 *
 * @code
 *   vesta_memset_noinline(bloque, 0, muchos_bytes);
 * @endcode
 */
VESTA_MEM_NOINLINE void vesta_memset_noinline(void *dst, uint8_t v,
                                              size_t n) VESTA_MEM_NOEXCEPT {
    vesta_mem_fill_dispatch(dst, v, n, 1, 1);
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
    vesta_mem_fill_dispatch(dst, v, n, 1, 1);
#else
    vesta_mem_fill_dispatch(dst, v, n, 0, 1);
#endif
#else
    memset(dst, v, n);
#endif
}

#ifdef __cplusplus
#include <type_traits>

namespace util {

/**
 * @brief Pone a @p v los bytes de @p count objetos de tipo @p T.
 *
 * La version con TIPO, y se llama distinto de @c vesta_memset por lo mismo que
 * @c util::vesta_memcopy: @c memset cuenta BYTES y @c memfill cuenta OBJETOS.
 * Con el mismo nombre, la deduccion elegiria esta sin que nadie lo escriba y el
 * tercer argumento cambiaria de unidad en silencio.
 *
 * Lo que gana con saber el tipo: @c sizeof(T) hace constante el tamano y
 * @c alignof(T) quita el prologo de alineacion, que es lo que impedia
 * desenrollar -- una copia de 256 bytes pasaba de linea recta a 97
 * instrucciones con 5 ramas por culpa de ese prologo.  Ver
 * @c util::vesta_memcopy, que lo cuenta entero.
 *
 * @tparam T   Tipo de los objetos.  Tiene que ser trivialmente copiable.
 * @param dst   Destino.
 * @param v     Byte a repetir.
 * @param count Cuantos objetos.  Uno si no se dice.
 *
 * @par Hilos
 * Segura, mientras el bufer sea de quien llama.
 *
 * @code
 *   util::vesta_memfill(&cabecera, 0xFF);
 * @endcode
 */
template <class T>
[[gnu::always_inline]] inline void vesta_memfill(T *dst, uint8_t v,
                                              size_t count = 1) noexcept {
    static_assert(std::is_trivially_copyable<T>::value,
                  "vesta_memfill solo vale para tipos trivialmente copiables");
    ::vesta_mem_fill_dispatch(dst, v, sizeof(T) * count, 1, alignof(T));
}

/// @copydoc ::vesta_memset_noinline
inline void vesta_memset_noinline(void *dst, uint8_t v, size_t n) noexcept {
    ::vesta_memset_noinline(dst, v, n);
}

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
