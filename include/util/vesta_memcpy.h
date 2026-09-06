/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/vesta_memcpy.h
 * @brief COPIAR memoria sin pasar por la biblioteca C.  Es lo unico que hay que
 *        incluir para copiar; el resto de la carpeta @c util/mem es interior.
 *
 * TRES ENTRADAS, y la diferencia entre ellas es SI PUEDE HABER UNA LLAMADA:
 *
 *   - @c vesta_memcpy         despacha por CPU.  Es la de uso general.
 *   - @c vesta_memcpy_inline  NO llama a nadie, nunca.  Para el camino caliente
 *                             de un asignador, para un manejador de
 *                             interrupcion, o para donde una llamada no sea
 *                             aceptable.
 *   - @c vesta_memmove        tolera solape.  Es la que hay que usar cuando no
 *                             se puede demostrar que las regiones son
 *                             disjuntas.
 *
 * La UNICA diferencia entre las dos primeras es el camino de AVX2, porque una
 * funcion con @c target("avx2") no se puede meter en linea en otra que no lo
 * lleve.  Todo lo demas -- los tamanos pequenos, el tramo hasta 64 y
 * @c rep @c movsb -- lo usan las dos.  Y si el binario se compila con
 * @c -mavx2, la diferencia DESAPARECE: ahi tampoco hay llamada.  Ver
 * @c VESTA_MEM_TARGET_AVX2 en @c util/mem/mem_config.h.
 *
 * Valen en C y en C++.  En C++ estan ademas como @c util::vesta_memcpy y
 * companyia.
 *
 * Lo que cuesta cada camino, en @c bench/bench_memcpy.cpp.
 */
#ifndef VESTA_UTIL_VESTA_MEMCPY_H
#define VESTA_UTIL_VESTA_MEMCPY_H

#include "util/mem/mem_config.h"
#include "util/mem/mem_inline.h"

#if defined(VESTA_MEM_ARCH_X86)
#include "util/mem/x86/erms_memcpy.h"
#include "util/mem/x86/sse2_memcpy.h"
#if defined(VESTA_MEM_ARCH_X86_64)
#include "util/mem/x86/avx2_memcpy.h"
#include "util/mem/x86/x86_cpu.h"
#endif
#elif VESTA_ALLOC_FREESTANDING
#include "util/mem/generic/scalar_memcpy.h"
#endif

/**
 * @brief El despacho, con la eleccion de caminos como parametro.
 *
 * @p wide llega SIEMPRE como constante desde las dos entradas publicas, y como
 * esta funcion se mete en linea a la fuerza, el camino que no se admite
 * desaparece del todo -- no queda ni la rama --.  Es un parametro y no dos
 * copias del cuerpo porque el orden de los cortes es el mismo y duplicarlo
 * seria pedir que se separen.
 *
 * El orden va de mas barato a mas caro, y cada corte tiene su motivo medido:
 *
 *   1. menos de 16      bloques solapados: sin bucle, sin llamada
 *   2. hasta 64         cuatro movimientos de 16: sin bucle, sin llamada
 *   3. desde el umbral  `rep movsb`, que lo resuelve el microcodigo y es lo
 *                       que le faltaba a la copia de 1 a 4 KiB
 *   4. el resto         el bucle vectorial mas ancho disponible
 *
 * @param dst  Destino.
 * @param src  Origen.  No puede solapar con @p dst.
 * @param n    Cuantos bytes.
 * @param wide Si se admite el camino de AVX2, que es el unico que puede costar
 *             una llamada.
 *
 * @par Hilos
 * Segura, mientras los bufers sean de quien llama.
 *
 * @code
 *   vesta_mem_copy_dispatch(d, s, n, 1);   // con todo
 * @endcode
 */
VESTA_MEM_ALWAYS_INLINE void
vesta_mem_copy_dispatch(void *dst, const void *src, size_t n,
                        int wide) VESTA_MEM_NOEXCEPT {
#if VESTA_ALLOC_FREESTANDING
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    if (n < 16) {
        vesta_mem_copy_small(d, s, n);
        return;
    }
#if defined(VESTA_MEM_ARCH_X86)
    if (n <= 128) {
        vesta_mem_sse2_copy_le128(d, s, n);
        return;
    }
    if (n >= VESTA_MEM_ERMS_MIN_COPY && vesta_mem_x86_has_erms()) {
        vesta_mem_erms_copy(d, s, n);
        return;
    }
#if defined(VESTA_MEM_ARCH_X86_64)
    if (wide && n >= VESTA_MEM_AVX2_MIN && vesta_mem_x86_has_avx2()) {
        vesta_mem_avx2_copy(d, s, n);
        return;
    }
#endif
    vesta_mem_sse2_copy(d, s, n);
#else
    vesta_mem_scalar_copy(d, s, n);
#endif
    (void)wide;
#else
    (void)wide;
    memcpy(dst, src, n); // respaldo: compilador sin las extensiones
#endif
}

/**
 * @brief Copia @p n bytes eligiendo el mejor camino de esta CPU.
 *
 * Las regiones NO se pueden solapar; para eso esta @c vesta_memmove.
 *
 * @param dst Destino.
 * @param src Origen.
 * @param n   Cuantos bytes.  Con 0 no hace nada.
 *
 * @par Hilos
 * Segura, mientras los bufers sean de quien llama.
 *
 * @code
 *   vesta_memcpy(destino, origen, bytes);
 * @endcode
 */
VESTA_MEM_ALWAYS_INLINE void vesta_memcpy(void *dst, const void *src,
                                          size_t n) VESTA_MEM_NOEXCEPT {
    vesta_mem_copy_dispatch(dst, src, n, 1);
}

/**
 * @brief Igual, pero con la GARANTIA de que no se llama a nadie.
 *
 * Dos diferencias con la anterior, y las dos desaparecen si el binario se
 * compila con @c -mavx2:
 *
 *   1. Si el tamano es una CONSTANTE, lo expande el compilador -- que para eso
 *      sabe exactamente cuantos movimientos hacen falta.
 *   2. Renuncia al camino de AVX2, que es el unico que no se puede meter en
 *      linea.  Con tamanos grandes eso se paga; es un intercambio consciente,
 *      porque aqui la pregunta no es cual es mas rapida en un banco sino donde
 *      una llamada no es aceptable.
 *
 * @c rep @c movsb SI lo usa: es una instruccion, no una funcion.
 *
 * @param dst Destino.
 * @param src Origen.  No puede solapar con @p dst.
 * @param n   Cuantos bytes.
 *
 * @par Hilos
 * Segura, mientras los bufers sean de quien llama.
 *
 * @code
 *   vesta_memcpy_inline(cabecera, &plantilla, sizeof plantilla);
 * @endcode
 */
VESTA_MEM_ALWAYS_INLINE void vesta_memcpy_inline(void *dst, const void *src,
                                                 size_t n) VESTA_MEM_NOEXCEPT {
#if VESTA_ALLOC_FREESTANDING
    if (__builtin_constant_p(n)) {
        __builtin_memcpy(dst, src, n);
        return;
    }
#if defined(VESTA_MEM_TARGET_AVX2)
    /* Con la micro-ISA fijada al compilar, el camino ancho tambien se mete en
     * linea, asi que no hay nada a lo que renunciar. */
    vesta_mem_copy_dispatch(dst, src, n, 1);
#else
    vesta_mem_copy_dispatch(dst, src, n, 0);
#endif
#else
    memcpy(dst, src, n);
#endif
}

/**
 * @brief Copia tolerante a SOLAPAMIENTO, como @c memmove.
 *
 * Con solape y el destino por delante del origen, copiar hacia adelante pisaria
 * la fuente antes de leerla, asi que ese caso se recorre HACIA ATRAS.  El resto
 * va por el camino rapido.
 *
 * @param dst Destino.
 * @param src Origen.
 * @param n   Cuantos bytes.
 *
 * @par Hilos
 * Segura, mientras los bufers sean de quien llama.
 *
 * @code
 *   vesta_memmove(v + 1, v, n);   // desplazar un array sobre si mismo
 * @endcode
 */
VESTA_MEM_INLINE void vesta_memmove(void *dst, const void *src,
                                    size_t n) VESTA_MEM_NOEXCEPT {
#if VESTA_ALLOC_FREESTANDING
    uint8_t *const d = (uint8_t *)dst;
    const uint8_t *const s = (const uint8_t *)src;
    if (d == s || n == 0) return;

    /* Sin solape ninguno vale el camino rapido, con todo lo que trae.  Y la
     * comprobacion tiene que ser la de VERDAD -- que los dos rangos no se
     * cruzan --, no "el destino va delante": el camino rapido ALINEA el
     * destino, y para eso escribe un bloque entero aunque solo necesite unos
     * bytes.  Con regiones disjuntas eso es inofensivo; con solape pisa el
     * origen antes de leerlo.  Ver `vesta_mem_sse2_copy_forward`. */
    if (d >= s + n || s >= d + n) {
        vesta_memcpy(d, s, n);
        return;
    }
#if defined(VESTA_MEM_ARCH_X86)
    if (d < s) {
        vesta_mem_sse2_copy_forward(d, s, n);
        return;
    }
    vesta_mem_sse2_copy_backward(d, s, n);
#else
    if (d < s) {
        vesta_mem_scalar_copy_forward(d, s, n);
        return;
    }
    vesta_mem_scalar_copy_backward(d, s, n);
#endif
#else
    memmove(dst, src, n); // respaldo: compilador sin las extensiones
#endif
}

#ifdef __cplusplus
namespace util {

/// @copydoc ::vesta_memcpy
[[gnu::always_inline]] inline void vesta_memcpy(void *dst, const void *src,
                                                size_t n) noexcept {
    ::vesta_memcpy(dst, src, n);
}

/// @copydoc ::vesta_memcpy_inline
[[gnu::always_inline]] inline void
vesta_memcpy_inline(void *dst, const void *src, size_t n) noexcept {
    ::vesta_memcpy_inline(dst, src, n);
}

/// @copydoc ::vesta_memmove
inline void vesta_memmove(void *dst, const void *src, size_t n) noexcept {
    ::vesta_memmove(dst, src, n);
}

} // namespace util
#endif // __cplusplus

#endif // VESTA_UTIL_VESTA_MEMCPY_H
