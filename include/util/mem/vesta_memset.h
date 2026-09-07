/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/mem/vesta_memset.h
 * @brief
 * \~english FILLING memory without going through the C library.  It is the only
 *          thing to include in order to fill; the rest of @c util/mem is
 *          internal.
 * \~spanish RELLENAR memoria sin pasar por la biblioteca C.  Es lo unico que
 *          hay que incluir para rellenar; el resto de @c util/mem es interior.
 * \~
 *
 * \~english
 * TWO ENTRY POINTS, and the difference is WHETHER THERE CAN BE A CALL:
 *
 *   - @c vesta_memset         dispatches by CPU.  The general-purpose one.
 *   - @c vesta_memset_inline  calls NOBODY, ever.
 *
 * The ONLY difference between the two is the AVX2 path, and it disappears if
 * the binary is compiled with @c -mavx2.  See @c util/mem/vesta_memcpy.h, which
 * tells the whole story, and @c VESTA_MEM_TARGET_AVX2 in
 * @c util/mem/mem_config.h.
 *
 * They work in C and in C++.  In C++ they are also there as
 * @c util::vesta_memset and @c util::vesta_memset_inline.
 *
 * What each path costs is in @c bench/bench_memset.cpp.
 *
 * \~spanish
 * DOS ENTRADAS, y la diferencia es SI PUEDE HABER UNA LLAMADA:
 *
 *   - @c vesta_memset         despacha por CPU.  Es la de uso general.
 *   - @c vesta_memset_inline  NO llama a nadie, nunca.
 *
 * La UNICA diferencia entre las dos es el camino de AVX2, y desaparece si el
 * binario se compila con @c -mavx2.  Ver @c util/mem/vesta_memcpy.h, que lo cuenta
 * entero, y @c VESTA_MEM_TARGET_AVX2 en @c util/mem/mem_config.h.
 *
 * Valen en C y en C++.  En C++ estan ademas como @c util::vesta_memset y
 * @c util::vesta_memset_inline.
 *
 * Lo que cuesta cada camino, en @c bench/bench_memset.cpp.
 *
 * \~
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
 * @brief
 * \~english The dispatch, with the choice of paths as a parameter.
 * \~spanish El despacho, con la eleccion de caminos como parametro.
 * \~
 *
 * \~english
 * @p wide ALWAYS arrives as a constant from the two public entry points, and
 * since this is inlined by force, the path that is not allowed disappears
 * entirely -- not even the branch is left.
 *
 * The cuts are the same as in the copy and for the same reasons, with ONE
 * different threshold for @c rep @c stosb: filling only writes, so our own
 * vector loop holds out longer before the instruction pays.
 *
 * @par Threads
 * Safe, as long as the buffer belongs to the caller.
 *
 * \~spanish
 * @p wide llega SIEMPRE como constante desde las dos entradas publicas, y como
 * esto se mete en linea a la fuerza, el camino que no se admite desaparece del
 * todo -- no queda ni la rama.
 *
 * Los cortes son los mismos que en la copia y por las mismas razones, con UN
 * umbral distinto para @c rep @c stosb: rellenar solo escribe, asi que el bucle
 * vectorial propio aguanta mas antes de que compense la instruccion.
 *
 * @par Hilos
 * Segura, mientras el bufer sea de quien llama.
 *
 * \~
 * @param dst
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
 * @param wide
 * \~english whether the AVX2 path is allowed.
 * \~spanish si se admite el camino de AVX2.
 * \~
 * @param known_align
 * \~english the alignment the caller can promise; 1 when nothing is known.
 * \~spanish la alineacion que quien llama puede prometer; 1 si no se sabe nada.
 * \~
 *
 * \~english
 * @code
 *   vesta_mem_fill_dispatch(d, 0, n, 1, 1);   // with everything
 * @endcode
 *
 * \~spanish
 * @code
 *   vesta_mem_fill_dispatch(d, 0, n, 1, 1);   // con todo
 * @endcode
 *
 * \~
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
 * @brief
 * \~english Sets @p n bytes to the value @p v choosing the best path for this
 *          CPU.
 * \~spanish Pone @p n bytes al valor @p v eligiendo el mejor camino de esta
 *          CPU.
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
 * @param dst
 * \~english the destination.
 * \~spanish destino.
 * \~
 * @param v
 * \~english the byte to repeat.
 * \~spanish byte a repetir.
 * \~
 * @param n
 * \~english how many bytes.  With 0 it does nothing.
 * \~spanish cuantos bytes.  Con 0 no hace nada.
 * \~
 *
 * \~english
 * @code
 *   vesta_memset(block, 0, bytes);    // zero it
 * @endcode
 *
 * \~spanish
 * @code
 *   vesta_memset(bloque, 0, bytes);   // zerificar
 * @endcode
 *
 * \~
 */
VESTA_MEM_ALWAYS_INLINE void vesta_memset(void *dst, uint8_t v,
                                          size_t n) VESTA_MEM_NOEXCEPT {
    /* Desde C no se sabe nada de la alineacion; desde C++ hay una version con
     * tipo que si la sabe. */
    vesta_mem_fill_dispatch(dst, v, n, 1, 1);
}

/**
 * @brief
 * \~english The same fill, but in a REAL function: one call and that is it.
 * \~spanish El mismo relleno, pero en una funcion de VERDAD: una llamada y ya.
 * \~
 *
 * \~english
 * See @c vesta_memcpy_noinline, which explains it: at large sizes the inline
 * expansion is hundreds of instructions at every place, and there the call
 * works out better.  It is there so that there is a CHOICE; the inline path
 * stays.
 *
 * @par Threads
 * Safe, as long as the buffer belongs to the caller.
 *
 * \~spanish
 * Ver @c vesta_memcpy_noinline, que lo explica: con tamanos grandes la
 * expansion en linea son cientos de instrucciones en cada sitio, y ahi sale
 * mas a cuenta la llamada.  Esta para poder ELEGIR; el camino en linea sigue.
 *
 * @par Hilos
 * Segura, mientras el bufer sea de quien llama.
 *
 * \~
 * @param dst
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
 *   vesta_memset_noinline(block, 0, many_bytes);
 * @endcode
 *
 * \~spanish
 * @code
 *   vesta_memset_noinline(bloque, 0, muchos_bytes);
 * @endcode
 *
 * \~
 */
VESTA_MEM_NOINLINE void vesta_memset_noinline(void *dst, uint8_t v,
                                              size_t n) VESTA_MEM_NOEXCEPT {
    vesta_mem_fill_dispatch(dst, v, n, 1, 1);
}

/**
 * @brief
 * \~english The same, but with the GUARANTEE that nobody gets called.
 * \~spanish Igual, pero con la GARANTIA de que no se llama a nadie.
 * \~
 *
 * \~english
 * With a CONSTANT size the compiler expands it; otherwise, the same dispatch
 * without the AVX2 path -- unless the binary is already compiled with
 * @c -mavx2, where there is nothing to give up either.  @c rep @c stosb IS
 * used: it is an instruction, not a function.
 *
 * @par Threads
 * Safe, as long as the buffer belongs to the caller.
 *
 * \~spanish
 * Con tamano CONSTANTE lo expande el compilador; si no, el mismo despacho sin
 * el camino de AVX2 -- salvo que el binario ya se compile con @c -mavx2, donde
 * tampoco hay nada a lo que renunciar --.  @c rep @c stosb SI lo usa: es una
 * instruccion, no una funcion.
 *
 * @par Hilos
 * Segura, mientras el bufer sea de quien llama.
 *
 * \~
 * @param dst
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
 *   vesta_memset_inline(&header, 0, sizeof header);
 * @endcode
 *
 * \~spanish
 * @code
 *   vesta_memset_inline(&cabecera, 0, sizeof cabecera);
 * @endcode
 *
 * \~
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
 * @brief
 * \~english Sets to @p v the bytes of @p count objects of type @p T.
 * \~spanish Pone a @p v los bytes de @p count objetos de tipo @p T.
 * \~
 *
 * \~english
 * The TYPED version, and it is named differently from @c vesta_memset for the
 * same reason as @c util::vesta_memcopy: @c memset counts BYTES and @c memfill
 * counts OBJECTS.  With the same name, deduction would pick this one without
 * anybody writing it and the third argument would quietly change unit.
 *
 * What it gains from knowing the type: @c sizeof(T) makes the size constant and
 * @c alignof(T) removes the alignment prologue, which is what prevented
 * unrolling -- a 256-byte copy went from a straight line to 97 instructions
 * with 5 branches because of that prologue.  See @c util::vesta_memcopy, which
 * tells the whole story.
 *
 * @par Threads
 * Safe, as long as the buffer belongs to the caller.
 *
 * \~spanish
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
 * @par Hilos
 * Segura, mientras el bufer sea de quien llama.
 *
 * \~
 * @tparam T
 * \~english the type of the objects.  It has to be trivially copyable.
 * \~spanish tipo de los objetos.  Tiene que ser trivialmente copiable.
 * \~
 * @param dst
 * \~english the destination.
 * \~spanish destino.
 * \~
 * @param v
 * \~english the byte to repeat.
 * \~spanish byte a repetir.
 * \~
 * @param count
 * \~english how many objects.  One if not said.
 * \~spanish cuantos objetos.  Uno si no se dice.
 * \~
 *
 * \~english
 * @code
 *   util::vesta_memfill(&header, 0xFF);
 * @endcode
 *
 * \~spanish
 * @code
 *   util::vesta_memfill(&cabecera, 0xFF);
 * @endcode
 *
 * \~
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
