/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/mem/x86/avx2_memcpy.h
 * @brief
 * \~english Copying with 32-byte moves.  Only if the CPU has AVX2.
 * \~spanish Copiar con movimientos de 32 bytes.  Solo si la CPU tiene AVX2.
 * \~
 *
 * \~english
 * @c target("avx2") compiles this function with AVX2 enabled WITHOUT requiring
 * it of the whole binary: whoever calls it asks about the CPU first
 * (@c vesta_mem_x86_has_avx2).  It is the way to take advantage of the
 * extension without losing the executable's portability.
 *
 * IN EXCHANGE IT CANNOT BE INLINED into a function that does not carry the same
 * @c target -- GCC forbids it, and rightly: it would inherit instructions its
 * caller cannot run.  That is why there are two distinct public entry points:
 * @c vesta_memcpy dispatches here and pays ONE call, and
 * @c vesta_memcpy_inline stays on the base path and pays none.  Compiling with
 * @c -mavx2 the distinction disappears; see @c VESTA_MEM_TARGET_AVX2.
 *
 * MIND the detail that already bit in the previous version of this code:
 * wrapping the body in `#if defined(__AVX2__)` does NOT work.  That macro is
 * only defined if the WHOLE binary is compiled with AVX2, which never happens
 * here -- the baseline is @c -march=x86-64 -- so the body vanished and the
 * dispatch, which DID detect AVX2 on the CPU, ended up calling the SSE2 path
 * anyway.  With @c target the body is always compiled.
 *
 * The accesses go through @c VESTA_MEM_LOAD32 / @c VESTA_MEM_STORE32 and not
 * through a plain assignment.  The reason -- that GCC splits every 32-byte
 * access into two of 16 -- is in @c util/mem/x86/x86_vec.h, with the
 * measurement.
 *
 * \~spanish
 * @c target("avx2") compila esta funcion con AVX2 habilitado SIN exigirlo al
 * binario entero: quien la llama pregunta antes por la CPU
 * (@c vesta_mem_x86_has_avx2).  Es la forma de aprovechar la extension sin
 * perder portabilidad del ejecutable.
 *
 * A CAMBIO NO SE PUEDE METER EN LINEA en una funcion que no lleve el mismo
 * @c target -- GCC lo prohibe, y con razon: heredaria instrucciones que quien
 * la llama no puede ejecutar --.  Por eso hay dos entradas publicas distintas:
 * @c vesta_memcpy despacha aqui y paga UNA llamada, y @c vesta_memcpy_inline se
 * queda en el camino base y no paga ninguna.  Compilando con @c -mavx2 la
 * distincion desaparece; ver @c VESTA_MEM_TARGET_AVX2.
 *
 * OJO al detalle que ya mordio en la version anterior de este codigo: NO vale
 * envolver el cuerpo en `#if defined(__AVX2__)`.  Ese macro solo esta definido
 * si el binario ENTERO se compila con AVX2, cosa que aqui no pasa nunca -- la
 * linea base es @c -march=x86-64 --, asi que el cuerpo desaparecia y el
 * despacho, que SI detectaba AVX2 en la CPU, terminaba llamando al camino de
 * SSE2 de todas formas.  Con @c target el cuerpo se compila siempre.
 *
 * Los accesos van por @c VESTA_MEM_LOAD32 / @c VESTA_MEM_STORE32 y no por una
 * asignacion normal.  El motivo -- que GCC parte cada acceso de 32 bytes en dos
 * de 16 -- esta en @c util/mem/x86/x86_vec.h, con la medida.
 *
 * \~
 */
#ifndef VESTA_UTIL_MEM_X86_AVX2_MEMCPY_H
#define VESTA_UTIL_MEM_X86_AVX2_MEMCPY_H

#include "util/mem/x86/sse2_memcpy.h"

#if defined(VESTA_MEM_ARCH_X86_64)

/**
 * @brief
 * \~english Copies @p n bytes with 32-byte moves.
 * \~spanish Copia @p n bytes con movimientos de 32.
 * \~
 *
 * \~english
 * The loop's step -- how many bytes per turn -- is fixed by
 * @c VESTA_MEM_AVX2_STEP, and it comes out of a profile, not out of intuition.
 * Below 32 it ends up on the base path, which already covers 16 and the tail.
 *
 * @par Threads
 * Safe, as long as the buffers belong to the caller.
 *
 * \~spanish
 * El escalon del bucle -- cuantos bytes por vuelta -- lo fija
 * @c VESTA_MEM_AVX2_STEP, y sale de un perfil, no de la intuicion.  Por debajo
 * de 32 termina en el camino base, que ya cubre 16 y la cola.
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
 *
 * \~english
 * @code
 *   if (vesta_mem_x86_has_avx2()) vesta_mem_avx2_copy(dst, src, n);
 * @endcode
 *
 * \~spanish
 * @code
 *   if (vesta_mem_x86_has_avx2()) vesta_mem_avx2_copy(dst, src, n);
 * @endcode
 *
 * \~
 */
VESTA_MEM_AVX2_FN void vesta_mem_avx2_copy(uint8_t *d, const uint8_t *s,
                                           size_t n) VESTA_MEM_NOEXCEPT {
    /* ALINEAR EL DESTINO a 32 antes del bucle.  Ver `VESTA_MEM_STORE32A` en
     * `x86_vec.h`: es lo que separaba este bucle de el de glibc, y vale casi
     * el doble en la banda de 128 B a 2 KiB. */
    if (n >= 64) {
        const size_t head = vesta_mem_x86_head_to_align(d, 32);
        if (head != 0) {
            VESTA_MEM_STORE32(d, VESTA_MEM_LOAD32(s));
            d += head;
            s += head;
            n -= head;
        }
    }
    /* PROBADO Y DESCARTADO: llevar un solo puntero y direccionar el origen como
     * `d + (s - d)`, que es lo que hace el `memcpy` del CRT de MSVC
     * (`movups (%rcx,%rdx)`, un solo `subq` por vuelta).  Medido tres veces:
     * 6,09-6,26 ns en la copia de 1 KiB contra 5,88-6,06 con dos punteros, o
     * sea nada o algo peor.  GCC ya emitia el direccionamiento bueno.  Se anota
     * para que nadie lo vuelva a intentar creyendo que queda ahi. */
#if VESTA_MEM_AVX2_STEP >= 256
    VESTA_MEM_NO_UNROLL
    while (n >= 256) {
        VESTA_MEM_KEEP_LOOP(d);
        const vesta_v32 a0 = VESTA_MEM_LOAD32(s);
        const vesta_v32 a1 = VESTA_MEM_LOAD32(s + 32);
        const vesta_v32 a2 = VESTA_MEM_LOAD32(s + 64);
        const vesta_v32 a3 = VESTA_MEM_LOAD32(s + 96);
        const vesta_v32 a4 = VESTA_MEM_LOAD32(s + 128);
        const vesta_v32 a5 = VESTA_MEM_LOAD32(s + 160);
        const vesta_v32 a6 = VESTA_MEM_LOAD32(s + 192);
        const vesta_v32 a7 = VESTA_MEM_LOAD32(s + 224);
        VESTA_MEM_STORE32A(d, a0); // el destino ya esta alineado
        VESTA_MEM_STORE32A(d + 32, a1);
        VESTA_MEM_STORE32A(d + 64, a2);
        VESTA_MEM_STORE32A(d + 96, a3);
        VESTA_MEM_STORE32A(d + 128, a4);
        VESTA_MEM_STORE32A(d + 160, a5);
        VESTA_MEM_STORE32A(d + 192, a6);
        VESTA_MEM_STORE32A(d + 224, a7);
        d += 256;
        s += 256;
        n -= 256;
    }
#endif
    VESTA_MEM_NO_UNROLL
    while (n >= 128) {
        VESTA_MEM_KEEP_LOOP(d); // que no lo cambie POR una llamada a memcpy
        const vesta_v32 a = VESTA_MEM_LOAD32(s);
        const vesta_v32 b = VESTA_MEM_LOAD32(s + 32);
        const vesta_v32 c = VESTA_MEM_LOAD32(s + 64);
        const vesta_v32 e = VESTA_MEM_LOAD32(s + 96);
        VESTA_MEM_STORE32A(d, a);
        VESTA_MEM_STORE32A(d + 32, b);
        VESTA_MEM_STORE32A(d + 64, c);
        VESTA_MEM_STORE32A(d + 96, e);
        d += 128;
        s += 128;
        n -= 128;
    }
    VESTA_MEM_NO_UNROLL
    while (n >= 32) {
        VESTA_MEM_KEEP_LOOP(d);
        VESTA_MEM_STORE32(d, VESTA_MEM_LOAD32(s));
        d += 32;
        s += 32;
        n -= 32;
    }
    if (n >= 16) {
        *(vesta_v16 *)(d) = *(const vesta_v16 *)(s);
        d += 16;
        s += 16;
        n -= 16;
    }
    vesta_mem_copy_small(d, s, n);
}

#endif // VESTA_MEM_ARCH_X86_64

#endif // VESTA_UTIL_MEM_X86_AVX2_MEMCPY_H
