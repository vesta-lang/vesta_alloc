/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/mem/x86/sse2_memcpy.h
 * @brief Copiar con movimientos de 16 bytes.  El camino BASE de x86.
 *
 * Base quiere decir dos cosas: que SSE2 esta garantizado en todo x86-64, asi
 * que este camino no tiene que comprobar nada; y que es el unico que se puede
 * meter EN LINEA en cualquier funcion, porque no lleva atributo @c target.  De
 * ahi que sea el que usa @c vesta_memcpy_inline.
 */
#ifndef VESTA_UTIL_MEM_X86_SSE2_MEMCPY_H
#define VESTA_UTIL_MEM_X86_SSE2_MEMCPY_H

#include "util/mem/mem_inline.h"
#include "util/mem/x86/x86_vec.h"

#if defined(VESTA_MEM_ARCH_X86)

/**
 * @brief Copia @p n bytes con movimientos de 16.
 *
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
 * @param d Destino.
 * @param s Origen.  No puede solapar con @p d.
 * @param n Cuantos bytes.
 *
 * @par Hilos
 * Segura, mientras los bufers sean de quien llama.
 *
 * @code
 *   vesta_mem_sse2_copy(dst, src, n);   // sin preguntar por la CPU
 * @endcode
 */
VESTA_MEM_ALWAYS_INLINE void vesta_mem_sse2_copy(uint8_t *d, const uint8_t *s,
                                                 size_t n) VESTA_MEM_NOEXCEPT {
    /* ALINEAR EL DESTINO antes del bucle.  Se escribe un bloque sin alinear y
     * se avanza hasta el siguiente limite de 16; los bytes que el bucle vuelve
     * a escribir llevan el MISMO dato, asi que pisarlos es inofensivo.  El
     * porque de todo esto -- y lo que costaba no hacerlo -- esta en
     * `VESTA_MEM_STORE16A`, en `x86_vec.h`. */
    if (n >= 32) {
        const size_t head = vesta_mem_x86_head_to_align(d, 16);
        if (head != 0) {
            *(vesta_v16 *)(d) = *(const vesta_v16 *)(s);
            d += head;
            s += head;
            n -= head;
        }
    }
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
 * @brief Copia de 16 a 128 bytes SIN BUCLE y sin llamar a nadie.
 *
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
 * @param d Destino.
 * @param s Origen.  No puede solapar con @p d.
 * @param n Cuantos bytes, de 16 a 128.
 *
 * @par Hilos
 * Segura, mientras los bufers sean de quien llama.
 *
 * @code
 *   if (n >= 16 && n <= 128) vesta_mem_sse2_copy_le128(d, s, n);
 * @endcode
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
 * @brief Copia hacia adelante SIN alinear el destino.  Para regiones que
 *        solapan con el destino por detras del origen.
 *
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
 * @param d Destino.
 * @param s Origen.  Puede solapar, con @p d en direcciones MENORES.
 * @param n Cuantos bytes.
 *
 * @par Hilos
 * Segura, mientras los bufers sean de quien llama.
 *
 * @code
 *   vesta_mem_sse2_copy_forward(v, v + 3, n);   // desplazar hacia abajo
 * @endcode
 */
VESTA_MEM_ALWAYS_INLINE void
vesta_mem_sse2_copy_forward(uint8_t *d, const uint8_t *s,
                            size_t n) VESTA_MEM_NOEXCEPT {
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
 * @brief Copia HACIA ATRAS, para cuando las regiones solapan y el destino va
 *        por delante del origen.
 *
 * Vive con la copia y no con el despacho porque es codigo de la micro-ISA: los
 * mismos movimientos de 16 bytes, recorridos al reves.
 *
 * La cola va por @c vesta_mem_copy_small, que TAMBIEN vale con solape: lee los
 * dos extremos ANTES de escribir ninguno, asi que lo que escribe no puede pisar
 * lo que aun le falta por leer.  Y no es una preferencia de estilo -- el bucle
 * de bytes que habia aqui lo convirtio GCC en una llamada a @c memmove, que es
 * exactamente el simbolo de la libc que este fichero existe para no tener.
 *
 * @param d Destino.
 * @param s Origen.
 * @param n Cuantos bytes.
 *
 * @par Hilos
 * Segura, mientras los bufers sean de quien llama.
 *
 * @code
 *   vesta_mem_sse2_copy_backward(v + 1, v, n);
 * @endcode
 */
VESTA_MEM_ALWAYS_INLINE void
vesta_mem_sse2_copy_backward(uint8_t *d, const uint8_t *s,
                             size_t n) VESTA_MEM_NOEXCEPT {
    while (n >= 16) {
        n -= 16;
        *(vesta_v16 *)(d + n) = *(const vesta_v16 *)(s + n);
    }
    vesta_mem_copy_small(d, s, n);
}

#endif // VESTA_MEM_ARCH_X86

#endif // VESTA_UTIL_MEM_X86_SSE2_MEMCPY_H
