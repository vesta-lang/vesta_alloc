/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/mem/x86/sse2_memcpy.h
 * @brief
 * \~english Copying with 16-byte moves.  x86's BASE path.
 * \~spanish Copiar con movimientos de 16 bytes.  El camino BASE de x86.
 * \~
 *
 * \~english
 * Base means two things: that SSE2 is guaranteed on every x86-64, so this path
 * has nothing to check; and that it is the only one that can be INLINED into
 * any function, because it carries no @c target attribute.  Hence it being the
 * one @c vesta_memcpy_inline uses.
 *
 * \~spanish
 * Base quiere decir dos cosas: que SSE2 esta garantizado en todo x86-64, asi
 * que este camino no tiene que comprobar nada; y que es el unico que se puede
 * meter EN LINEA en cualquier funcion, porque no lleva atributo @c target.  De
 * ahi que sea el que usa @c vesta_memcpy_inline.
 *
 * \~
 */
#ifndef VESTA_UTIL_MEM_X86_SSE2_MEMCPY_H
#define VESTA_UTIL_MEM_X86_SSE2_MEMCPY_H

#include "util/mem/mem_inline.h"
#include "util/mem/x86/x86_vec.h"

#if defined(VESTA_MEM_ARCH_X86)

/**
 * @brief
 * \~english Copies @p n bytes with 16-byte moves.
 * \~spanish Copia @p n bytes con movimientos de 16.
 * \~
 *
 * \~english
 * Unrolled to 64 bytes per turn: the loop emits four independent loads and four
 * independent writes, which the superscalar core overlaps.  Going 16 at a time
 * without unrolling leaves the load unit at half capacity.
 *
 * The tail goes through @c vesta_mem_copy_small, with no byte loop.  The reason
 * is in @c util/mem/mem_inline.h.
 *
 * It is inlined ALWAYS, on purpose: without @c target there is nothing to stop
 * it, and that is precisely its reason for existing next to the AVX2 version.
 *
 * @par Threads
 * Safe, as long as the buffers belong to the caller.
 *
 * \~spanish
 * Desenrollado a 64 bytes por vuelta: el bucle emite cuatro cargas y cuatro
 * escrituras independientes, que el nucleo superescalar solapa.  Ir de 16 en 16
 * sin desenrollar deja la unidad de carga a media capacidad.
 *
 * La cola va por @c vesta_mem_copy_small, sin bucle de bytes.  El porque, en
 * @c util/mem/mem_inline.h.
 *
 * Se mete en linea SIEMPRE, a proposito: sin @c target no hay nada que lo
 * impida, y esa es justo su razon de ser frente a la version de AVX2.
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
 * \~english the source.  It must not overlap @p d.
 * \~spanish origen.  No puede solapar con @p d.
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
 *   vesta_mem_sse2_copy(dst, src, n, 1);   // without asking about the CPU
 * @endcode
 *
 * \~spanish
 * @code
 *   vesta_mem_sse2_copy(dst, src, n, 1);   // sin preguntar por la CPU
 * @endcode
 *
 * \~
 */
VESTA_MEM_ALWAYS_INLINE void
vesta_mem_sse2_copy(uint8_t *d, const uint8_t *s, size_t n,
                    size_t known_align) VESTA_MEM_NOEXCEPT {
    /* ALINEAR EL DESTINO antes del bucle.  Se escribe un bloque sin alinear y
     * se avanza hasta el siguiente limite de 16; los bytes que el bucle vuelve
     * a escribir llevan el MISMO dato, asi que pisarlos es inofensivo.  El
     * porque de todo esto -- y lo que costaba no hacerlo -- esta en
     * `VESTA_MEM_STORE16A`, en `x86_vec.h`.
     *
     * PERO SOLO SI HACE FALTA.  @p known_align dice lo que YA se sabe de la
     * alineacion del destino; con una constante >= 16 todo este bloque
     * desaparece al compilar.  Y eso importa mas de lo que parece: el prologo
     * hace `n -= head` con un @c head calculado en ejecucion, y eso convierte
     * un tamano CONSTANTE en variable, con lo que el bucle deja de poder
     * desenrollarse.
     *
     * Se dice EXPLICITAMENTE en vez de confiar en que el compilador lo deduzca
     * del tipo del puntero: a veces lo hace y a veces no, y de eso no puede
     * depender el rendimiento de una primitiva. */
    if (known_align < 16 && n >= 32) {
        const size_t head = vesta_mem_x86_head_to_align(d, 16);
        if (head != 0) {
            *(vesta_v16 *)(d) = *(const vesta_v16 *)(s);
            d += head;
            s += head;
            n -= head;
        }
    }
    VESTA_MEM_NO_UNROLL
    while (n >= 64) {
        VESTA_MEM_KEEP_LOOP(d); // que no lo cambie POR una llamada a memcpy
        const vesta_v16 a = *(const vesta_v16 *)(s);
        const vesta_v16 b = *(const vesta_v16 *)(s + 16);
        const vesta_v16 c = *(const vesta_v16 *)(s + 32);
        const vesta_v16 e = *(const vesta_v16 *)(s + 48);
        VESTA_MEM_STORE16A(d, a); // el destino ya esta alineado
        VESTA_MEM_STORE16A(d + 16, b);
        VESTA_MEM_STORE16A(d + 32, c);
        VESTA_MEM_STORE16A(d + 48, e);
        d += 64;
        s += 64;
        n -= 64;
    }
    VESTA_MEM_NO_UNROLL
    while (n >= 16) {
        VESTA_MEM_KEEP_LOOP(d);
        *(vesta_v16 *)(d) = *(const vesta_v16 *)(s);
        d += 16;
        s += 16;
        n -= 16;
    }
    vesta_mem_copy_small(d, s, n);
}

/**
 * @brief
 * \~english A copy of 16 to 128 bytes WITHOUT A LOOP and without calling
 *          anybody.
 * \~spanish Copia de 16 a 128 bytes SIN BUCLE y sin llamar a nadie.
 * \~
 *
 * \~english
 * The same overlapping-blocks trick as @c vesta_mem_copy_small, but with
 * 16-byte registers: two moves cover 16 to 32, four cover up to 64 and eight up
 * to 128, without a single turn of a loop.  Every access is addressed from BOTH
 * ends, so there are no pointers to advance and no condition to check; they
 * overlap in the middle and the same data gets written twice.
 *
 * WHY IT GOES UP TO 128 AND NOT TO 64.  Because that is what glibc does, and it
 * was seen by DISASSEMBLING it: its @c memcpy chains these blocks up to eight
 * vectors and only enters a loop above that.  We started iterating at 65, and
 * there it lost by 0.63x against it with half the work per byte.
 *
 * @par Threads
 * Safe, as long as the buffers belong to the caller.
 *
 * \~spanish
 * El mismo truco de bloques solapados que @c vesta_mem_copy_small, pero con
 * registros de 16: dos movimientos cubren de 16 a 32, cuatro cubren hasta 64 y
 * ocho hasta 128, sin una sola vuelta de bucle.  Todos los accesos se
 * direccionan desde los DOS extremos, asi que no hay punteros que avanzar ni
 * condicion que comprobar; se solapan en el medio y se escribe dos veces el
 * mismo dato.
 *
 * POR QUE LLEGA HASTA 128 Y NO HASTA 64.  Porque es lo que hace glibc, y se vio
 * DESENSAMBLANDOLA: su @c memcpy encadena estos bloques hasta ocho vectores y
 * solo entra en un bucle por encima de eso.  Nosotros empezabamos a iterar en
 * 65, y ahi se perdia por 0,63x contra ella con la mitad del trabajo por byte.
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
 * \~english the source.  It must not overlap @p d.
 * \~spanish origen.  No puede solapar con @p d.
 * \~
 * @param n
 * \~english how many bytes, from 16 to 128.
 * \~spanish cuantos bytes, de 16 a 128.
 * \~
 *
 * \~english
 * @code
 *   if (n >= 16 && n <= 128) vesta_mem_sse2_copy_le128(d, s, n);
 * @endcode
 *
 * \~spanish
 * @code
 *   if (n >= 16 && n <= 128) vesta_mem_sse2_copy_le128(d, s, n);
 * @endcode
 *
 * \~
 */
VESTA_MEM_ALWAYS_INLINE void
vesta_mem_sse2_copy_le128(uint8_t *d, const uint8_t *s,
                          size_t n) VESTA_MEM_NOEXCEPT {
    if (n > 64) {
        const vesta_v16 a0 = *(const vesta_v16 *)(s);
        const vesta_v16 a1 = *(const vesta_v16 *)(s + 16);
        const vesta_v16 a2 = *(const vesta_v16 *)(s + 32);
        const vesta_v16 a3 = *(const vesta_v16 *)(s + 48);
        const vesta_v16 b0 = *(const vesta_v16 *)(s + n - 64);
        const vesta_v16 b1 = *(const vesta_v16 *)(s + n - 48);
        const vesta_v16 b2 = *(const vesta_v16 *)(s + n - 32);
        const vesta_v16 b3 = *(const vesta_v16 *)(s + n - 16);
        *(vesta_v16 *)(d) = a0;
        *(vesta_v16 *)(d + 16) = a1;
        *(vesta_v16 *)(d + 32) = a2;
        *(vesta_v16 *)(d + 48) = a3;
        *(vesta_v16 *)(d + n - 64) = b0;
        *(vesta_v16 *)(d + n - 48) = b1;
        *(vesta_v16 *)(d + n - 32) = b2;
        *(vesta_v16 *)(d + n - 16) = b3;
        return;
    }
    if (n > 32) {
        const vesta_v16 a = *(const vesta_v16 *)(s);
        const vesta_v16 b = *(const vesta_v16 *)(s + 16);
        const vesta_v16 c = *(const vesta_v16 *)(s + n - 32);
        const vesta_v16 e = *(const vesta_v16 *)(s + n - 16);
        *(vesta_v16 *)(d) = a;
        *(vesta_v16 *)(d + 16) = b;
        *(vesta_v16 *)(d + n - 32) = c;
        *(vesta_v16 *)(d + n - 16) = e;
        return;
    }
    {
        const vesta_v16 a = *(const vesta_v16 *)(s);
        const vesta_v16 b = *(const vesta_v16 *)(s + n - 16);
        *(vesta_v16 *)(d) = a;
        *(vesta_v16 *)(d + n - 16) = b;
    }
}

/**
 * @brief
 * \~english Copies forwards WITHOUT aligning the destination.  For regions that
 *          overlap with the destination behind the source.
 * \~spanish Copia hacia adelante SIN alinear el destino.  Para regiones que
 *          solapan con el destino por detras del origen.
 * \~
 *
 * \~english
 * It exists for a correctness reason, not a speed one.  The normal path ALIGNS
 * the destination, and to do that it writes a whole block even when it only
 * needs the first bytes: the extra ones carry the right data and the loop
 * rewrites them anyway, so with disjoint regions it is harmless.  With OVERLAP
 * it is not -- that extra block tramples bytes of the SOURCE that have not been
 * read yet, and from there on rubbish gets copied.  @c tests/test_mem_ops.cpp
 * catches it.
 *
 * Here every turn reads before writing and does not write one byte extra, which
 * is what makes the forward copy safe with @p d behind @p s.
 *
 * @par Threads
 * Safe, as long as the buffers belong to the caller.
 *
 * \~spanish
 * Existe por una razon de correccion, no de velocidad.  El camino normal
 * ALINEA el destino, y para eso escribe un bloque entero aunque solo necesite
 * los primeros bytes: los de mas llevan el dato bueno y el bucle los reescribe
 * igual, asi que con regiones disjuntas es inofensivo.  Con SOLAPE no lo es --
 * ese bloque de mas pisa bytes del ORIGEN que aun no se han leido, y a partir
 * de ahi se copia basura --.  Lo caza @c tests/test_mem_ops.cpp.
 *
 * Aqui cada vuelta lee antes de escribir y no escribe ni un byte de mas, que es
 * lo que hace segura la copia hacia adelante con @p d por detras de @p s.
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
 *   vesta_mem_sse2_copy_forward(v, v + 3, n);   // shift downwards
 * @endcode
 *
 * \~spanish
 * @code
 *   vesta_mem_sse2_copy_forward(v, v + 3, n);   // desplazar hacia abajo
 * @endcode
 *
 * \~
 */
VESTA_MEM_ALWAYS_INLINE void
vesta_mem_sse2_copy_forward(uint8_t *d, const uint8_t *s,
                            size_t n) VESTA_MEM_NOEXCEPT {
    VESTA_MEM_NO_UNROLL
    while (n >= 16) {
        VESTA_MEM_KEEP_LOOP(d);
        *(vesta_v16 *)(d) = *(const vesta_v16 *)(s);
        d += 16;
        s += 16;
        n -= 16;
    }
    vesta_mem_copy_small(d, s, n);
}

/**
 * @brief
 * \~english Copies BACKWARDS, for when the regions overlap and the destination
 *          is in front of the source.
 * \~spanish Copia HACIA ATRAS, para cuando las regiones solapan y el destino va
 *          por delante del origen.
 * \~
 *
 * \~english
 * It lives with the copy and not with the dispatch because it is micro-ISA
 * code: the same 16-byte moves, walked in reverse.
 *
 * The tail goes through @c vesta_mem_copy_small, which ALSO works with overlap:
 * it reads both ends BEFORE writing either, so what it writes cannot trample
 * what it has left to read.  And it is not a matter of style -- the byte loop
 * that used to be here got turned by GCC into a call to @c memmove, which is
 * exactly the libc symbol this file exists in order not to have.
 *
 * @par Threads
 * Safe, as long as the buffers belong to the caller.
 *
 * \~spanish
 * Vive con la copia y no con el despacho porque es codigo de la micro-ISA: los
 * mismos movimientos de 16 bytes, recorridos al reves.
 *
 * La cola va por @c vesta_mem_copy_small, que TAMBIEN vale con solape: lee los
 * dos extremos ANTES de escribir ninguno, asi que lo que escribe no puede pisar
 * lo que aun le falta por leer.  Y no es una preferencia de estilo -- el bucle
 * de bytes que habia aqui lo convirtio GCC en una llamada a @c memmove, que es
 * exactamente el simbolo de la libc que este fichero existe para no tener.
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
 *   vesta_mem_sse2_copy_backward(v + 1, v, n);
 * @endcode
 *
 * \~spanish
 * @code
 *   vesta_mem_sse2_copy_backward(v + 1, v, n);
 * @endcode
 *
 * \~
 */
VESTA_MEM_ALWAYS_INLINE void
vesta_mem_sse2_copy_backward(uint8_t *d, const uint8_t *s,
                             size_t n) VESTA_MEM_NOEXCEPT {
    VESTA_MEM_NO_UNROLL
    while (n >= 16) {
        n -= 16;
        *(vesta_v16 *)(d + n) = *(const vesta_v16 *)(s + n);
    }
    vesta_mem_copy_small(d, s, n);
}

#endif // VESTA_MEM_ARCH_X86

#endif // VESTA_UTIL_MEM_X86_SSE2_MEMCPY_H
