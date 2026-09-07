/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/mem/vesta_memcpy.h
 * @brief
 * \~english COPYING memory without going through the C library.  It is the only
 *          thing to include in order to copy; the rest of the @c util/mem
 *          folder is internal.
 * \~spanish COPIAR memoria sin pasar por la biblioteca C.  Es lo unico que hay
 *          que incluir para copiar; el resto de la carpeta @c util/mem es
 *          interior.
 * \~
 *
 * \~english
 * THREE ENTRY POINTS, and the difference between them is WHETHER THERE CAN BE A
 * CALL:
 *
 *   - @c vesta_memcpy         dispatches by CPU.  The general-purpose one.
 *   - @c vesta_memcpy_inline  calls NOBODY, ever.  For an allocator's hot path,
 *                             for an interrupt handler, or for anywhere a call
 *                             is not acceptable.
 *   - @c vesta_memmove        tolerates overlap.  The one to use when the
 *                             regions cannot be proven disjoint.
 *
 * The ONLY difference between the first two is the AVX2 path, because a
 * function with @c target("avx2") cannot be inlined into one that does not
 * carry it.  Everything else -- the small sizes, the stretch up to 64 and
 * @c rep @c movsb -- is used by both.  And if the binary is compiled with
 * @c -mavx2, the difference DISAPPEARS: there is no call there either.  See
 * @c VESTA_MEM_TARGET_AVX2 in @c util/mem/mem_config.h.
 *
 * They work in C and in C++.  In C++ they are also there as
 * @c util::vesta_memcpy and company.
 *
 * What each path costs is in @c bench/bench_memcpy.cpp.
 *
 * \~spanish
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
 *
 * \~
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
 * @brief
 * \~english The dispatch, with the choice of paths as a parameter.
 * \~spanish El despacho, con la eleccion de caminos como parametro.
 * \~
 *
 * \~english
 * @p wide ALWAYS arrives as a constant from the two public entry points, and
 * since this function is inlined by force, the path that is not allowed
 * disappears entirely -- not even the branch is left.  It is a parameter and
 * not two copies of the body because the order of the cuts is the same and
 * duplicating it would be asking for them to drift apart.
 *
 * The order goes from cheapest to dearest, and every cut has its measured
 * reason:
 *
 *   1. under 16          overlapping blocks: no loop, no call
 *   2. up to 64          four moves of 16: no loop, no call
 *   3. from the          `rep movsb`, which the microcode resolves and which is
 *      threshold on      what the 1 to 4 KiB copy was missing
 *   4. the rest          the widest vector loop available
 *
 * @par Threads
 * Safe, as long as the buffers belong to the caller.
 *
 * \~spanish
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
 * @par Hilos
 * Segura, mientras los bufers sean de quien llama.
 *
 * \~
 * @param dst
 * \~english the destination.
 * \~spanish destino.
 * \~
 * @param src
 * \~english the source.  It must not overlap @p dst.
 * \~spanish origen.  No puede solapar con @p dst.
 * \~
 * @param n
 * \~english how many bytes.
 * \~spanish cuantos bytes.
 * \~
 * @param wide
 * \~english whether the AVX2 path is allowed, which is the only one that can
 *           cost a call.
 * \~spanish si se admite el camino de AVX2, que es el unico que puede costar
 *           una llamada.
 * \~
 * @param known_align
 * \~english the alignment the caller can promise; 1 when nothing is known.  It
 *           is what lets the alignment prologue be left out.
 * \~spanish la alineacion que quien llama puede prometer; 1 si no se sabe nada.
 *           Es lo que permite quitar el prologo de alineacion.
 * \~
 *
 * \~english
 * @code
 *   vesta_mem_copy_dispatch(d, s, n, 1, 1);   // with everything
 * @endcode
 *
 * \~spanish
 * @code
 *   vesta_mem_copy_dispatch(d, s, n, 1, 1);   // con todo
 * @endcode
 *
 * \~
 */
VESTA_MEM_ALWAYS_INLINE void
vesta_mem_copy_dispatch(void *dst, const void *src, size_t n, int wide,
                        size_t known_align) VESTA_MEM_NOEXCEPT {
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
    vesta_mem_sse2_copy(d, s, n, known_align);
#else
    vesta_mem_scalar_copy(d, s, n);
#endif
    (void)wide;
    (void)known_align;
#else
    (void)wide;
    (void)known_align;
    memcpy(dst, src, n); // respaldo: compilador sin las extensiones
#endif
}

/**
 * @brief
 * \~english Copies @p n bytes choosing the best path for this CPU.
 * \~spanish Copia @p n bytes eligiendo el mejor camino de esta CPU.
 * \~
 *
 * \~english
 * The regions may NOT overlap; @c vesta_memmove is there for that.
 *
 * @par Threads
 * Safe, as long as the buffers belong to the caller.
 *
 * \~spanish
 * Las regiones NO se pueden solapar; para eso esta @c vesta_memmove.
 *
 * @par Hilos
 * Segura, mientras los bufers sean de quien llama.
 *
 * \~
 * @param dst
 * \~english the destination.
 * \~spanish destino.
 * \~
 * @param src
 * \~english the source.
 * \~spanish origen.
 * \~
 * @param n
 * \~english how many bytes.  With 0 it does nothing.
 * \~spanish cuantos bytes.  Con 0 no hace nada.
 * \~
 *
 * \~english
 * @code
 *   vesta_memcpy(destination, source, bytes);
 * @endcode
 *
 * \~spanish
 * @code
 *   vesta_memcpy(destino, origen, bytes);
 * @endcode
 *
 * \~
 */
VESTA_MEM_ALWAYS_INLINE void vesta_memcpy(void *dst, const void *src,
                                          size_t n) VESTA_MEM_NOEXCEPT {
    /* Desde C no se sabe nada de la alineacion, asi que se resuelve en
     * ejecucion.  Desde C++ hay una version con tipo que si la sabe. */
    vesta_mem_copy_dispatch(dst, src, n, 1, 1);
}

/**
 * @brief
 * \~english The same copy, but in a REAL function: one call and that is it.
 * \~spanish La misma copia, pero en una funcion de VERDAD: una llamada y ya.
 * \~
 *
 * \~english
 * @c vesta_memcpy's path is inlined ALWAYS, and that is what is wanted on an
 * allocator's hot path -- there the copy is a few bytes and the call would cost
 * more than the work.  But at large sizes the expansion is hundreds of
 * instructions AT EVERY PLACE it is called from, and then paying a call and not
 * bloating the instruction cache works out better.
 *
 * It exists so that there is a choice, not to replace: the inline path is still
 * there.
 *
 * @par Threads
 * Safe, as long as the buffers belong to the caller.
 *
 * \~spanish
 * El camino de @c vesta_memcpy se mete en linea SIEMPRE, y eso es lo que se
 * quiere en el camino caliente de un asignador -- ahi la copia es de unos pocos
 * bytes y la llamada costaria mas que el trabajo --.  Pero con tamanos grandes
 * la expansion son cientos de instrucciones EN CADA SITIO donde se llame, y
 * entonces sale mas a cuenta pagar una llamada y no hinchar la cache de
 * instrucciones.
 *
 * Existe para poder elegir, no para sustituir: el camino en linea sigue ahi.
 *
 * @par Hilos
 * Segura, mientras los bufers sean de quien llama.
 *
 * \~
 * @param dst
 * \~english the destination.
 * \~spanish destino.
 * \~
 * @param src
 * \~english the source.  It must not overlap @p dst.
 * \~spanish origen.  No puede solapar con @p dst.
 * \~
 * @param n
 * \~english how many bytes.
 * \~spanish cuantos bytes.
 * \~
 *
 * \~english
 * @code
 *   vesta_memcpy_noinline(destination, source, many_bytes);
 * @endcode
 *
 * \~spanish
 * @code
 *   vesta_memcpy_noinline(destino, origen, muchos_bytes);
 * @endcode
 *
 * \~
 */
VESTA_MEM_NOINLINE void vesta_memcpy_noinline(void *dst, const void *src,
                                              size_t n) VESTA_MEM_NOEXCEPT {
    vesta_mem_copy_dispatch(dst, src, n, 1, 1);
}

/**
 * @brief
 * \~english The same, but with the GUARANTEE that nobody gets called.
 * \~spanish Igual, pero con la GARANTIA de que no se llama a nadie.
 * \~
 *
 * \~english
 * Two differences from the previous one, and both disappear if the binary is
 * compiled with @c -mavx2:
 *
 *   1. If the size is a CONSTANT, the compiler expands it -- it knows exactly
 *      how many moves are needed.
 *   2. It gives up the AVX2 path, which is the only one that cannot be inlined.
 *      At large sizes that is paid for; it is a conscious trade, because the
 *      question here is not which is faster in a benchmark but where a call is
 *      not acceptable.
 *
 * @c rep @c movsb IS used: it is an instruction, not a function.
 *
 * @par Threads
 * Safe, as long as the buffers belong to the caller.
 *
 * \~spanish
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
 * @par Hilos
 * Segura, mientras los bufers sean de quien llama.
 *
 * \~
 * @param dst
 * \~english the destination.
 * \~spanish destino.
 * \~
 * @param src
 * \~english the source.  It must not overlap @p dst.
 * \~spanish origen.  No puede solapar con @p dst.
 * \~
 * @param n
 * \~english how many bytes.
 * \~spanish cuantos bytes.
 * \~
 *
 * \~english
 * @code
 *   vesta_memcpy_inline(header, &template_, sizeof template_);
 * @endcode
 *
 * \~spanish
 * @code
 *   vesta_memcpy_inline(cabecera, &plantilla, sizeof plantilla);
 * @endcode
 *
 * \~
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
    vesta_mem_copy_dispatch(dst, src, n, 1, 1);
#else
    vesta_mem_copy_dispatch(dst, src, n, 0, 1);
#endif
#else
    memcpy(dst, src, n);
#endif
}

/**
 * @brief
 * \~english A copy that tolerates OVERLAP, like @c memmove.
 * \~spanish Copia tolerante a SOLAPAMIENTO, como @c memmove.
 * \~
 *
 * \~english
 * With overlap and the destination in front of the source, copying forwards
 * would trample the source before reading it, so that case is walked BACKWARDS.
 * The rest goes down the fast path.
 *
 * @par Threads
 * Safe, as long as the buffers belong to the caller.
 *
 * \~spanish
 * Con solape y el destino por delante del origen, copiar hacia adelante pisaria
 * la fuente antes de leerla, asi que ese caso se recorre HACIA ATRAS.  El resto
 * va por el camino rapido.
 *
 * @par Hilos
 * Segura, mientras los bufers sean de quien llama.
 *
 * \~
 * @param dst
 * \~english the destination.
 * \~spanish destino.
 * \~
 * @param src
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
 *   vesta_memmove(v + 1, v, n);   // shift an array over itself
 * @endcode
 *
 * \~spanish
 * @code
 *   vesta_memmove(v + 1, v, n);   // desplazar un array sobre si mismo
 * @endcode
 *
 * \~
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
#include <type_traits>

namespace util {

/**
 * @brief
 * \~english Copies @p count objects of type @p T.  The TYPED version.
 * \~spanish Copia @p count objetos de tipo @p T.  La version con TIPO.
 * \~
 *
 * \~english
 * IT IS NAMED DIFFERENTLY ON PURPOSE, and it is not cosmetic: @c memcpy counts
 * BYTES and @c memcopy counts OBJECTS.  With the same name, a
 * `vesta_memcpy(p, q, n)` with @c p and @c q of the same type would pick this
 * one -- by deduction, without anybody writing anything -- and @c n would come
 * to mean objects: @c n*sizeof(T) bytes would be copied where @c n were wanted.
 * That does not give an error, it gives trampled memory.  Two names, two units.
 *
 * WHAT IT ADDS OVER THE C ONE, which is not little: the type carries two facts
 * that cannot be known from C, and both are resolved at compile time.
 *
 *   - @c sizeof(T).  With @p count constant the whole size is too, and the
 *     dispatcher's cascade folds down to whichever straight line applies.  Up
 *     to 128 bytes that already came out on its own -- measured: 19
 *     instructions and ZERO branches for a 128-byte copy -- but above that it
 *     did not, and the reason is the second fact.
 *   - @c alignof(T).  The general path has to ALIGN the destination before the
 *     loop, and for that it computes an offset AT RUN TIME.  That turns a
 *     constant size into a variable one and the loop stops being unrolled: a
 *     256-byte copy came out in 97 instructions with 5 branches.  Knowing the
 *     type is already aligned, that prologue is not emitted and it goes back to
 *     being straight.
 *
 * And a third thing that is not speed but correctness: copying the bytes of a
 * type that is not trivially copyable is undefined behaviour, and here it is
 * rejected at compile time instead of giving a broken object.
 *
 * @par Threads
 * Safe, as long as the buffers belong to the caller.
 *
 * \~spanish
 * SE LLAMA DISTINTO A PROPOSITO, y no es cosmetica: @c memcpy cuenta BYTES y
 * @c memcopy cuenta OBJETOS.  Con el mismo nombre, un `vesta_memcpy(p, q, n)`
 * con @c p y @c q del mismo tipo elegiria esta -- por deduccion, sin que nadie
 * escriba nada -- y @c n pasaria a significar objetos: se copiarian
 * @c n*sizeof(T) bytes donde se querian @c n.  Eso no da un error, da memoria
 * pisada.  Dos nombres, dos unidades.
 *
 * QUE APORTA SOBRE LA DE C, que no es poco: el tipo trae dos datos que desde C
 * no se pueden saber, y los dos se resuelven al compilar.
 *
 *   - @c sizeof(T).  Con @p count constante el tamano entero lo es, y la
 *     cascada del despachador se pliega hasta dejar la linea recta que
 *     corresponda.  Hasta 128 bytes eso ya salia solo -- medido: 19
 *     instrucciones y CERO ramas para una copia de 128 --, pero de ahi para
 *     arriba no, y el motivo es el segundo dato.
 *   - @c alignof(T).  El camino general tiene que ALINEAR el destino antes del
 *     bucle, y para eso calcula un desplazamiento EN EJECUCION.  Eso convierte
 *     un tamano constante en variable y el bucle deja de desenrollarse: una
 *     copia de 256 bytes salia en 97 instrucciones con 5 ramas.  Sabiendo que
 *     el tipo ya esta alineado, ese prologo no se emite y vuelve a ser recta.
 *
 * Y una tercera cosa que no es velocidad sino correccion: copiar bytes de un
 * tipo que no es trivialmente copiable es comportamiento indefinido, y aqui se
 * rechaza al compilar en vez de dar un objeto roto.
 *
 * @par Hilos
 * Segura, mientras los bufers sean de quien llama.
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
 * @param src
 * \~english the source.  It must not overlap @p dst.
 * \~spanish origen.  No puede solapar con @p dst.
 * \~
 * @param count
 * \~english how many objects.  One if not said.
 * \~spanish cuantos objetos.  Uno si no se dice.
 * \~
 *
 * \~english
 * @code
 *   util::vesta_memcopy(&destination, &source);    // ONE object
 *   util::vesta_memcopy(v_dst, v_src, how_many);   // `how_many` OBJECTS
 *   util::vesta_memcopy<Header>(d, s);             // the type, explicit
 * @endcode
 *
 * \~spanish
 * @code
 *   util::vesta_memcopy(&destino, &origen);        // UN objeto
 *   util::vesta_memcopy(v_dst, v_src, cuantos);    // `cuantos` OBJETOS
 *   util::vesta_memcopy<Cabecera>(d, s);           // el tipo, explicito
 * @endcode
 *
 * \~
 */
template <class T>
[[gnu::always_inline]] inline void vesta_memcopy(T *dst, const T *src,
                                              size_t count = 1) noexcept {
    static_assert(std::is_trivially_copyable<T>::value,
                  "vesta_memcopy solo vale para tipos trivialmente copiables: "
                  "copiar los bytes de cualquier otro es comportamiento "
                  "indefinido");
    ::vesta_mem_copy_dispatch(dst, src, sizeof(T) * count, 1, alignof(T));
}

/// @copydoc ::vesta_memcpy
[[gnu::always_inline]] inline void vesta_memcpy(void *dst, const void *src,
                                                size_t n) noexcept {
    ::vesta_memcpy(dst, src, n);
}

/// @copydoc ::vesta_memcpy_noinline
inline void vesta_memcpy_noinline(void *dst, const void *src,
                                  size_t n) noexcept {
    ::vesta_memcpy_noinline(dst, src, n);
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
