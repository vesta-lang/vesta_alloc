/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/mem/x86/nt_memset.h
 * @brief
 * \~english Filling a block bigger than the cache, WITHOUT going through it.
 * \~spanish Rellenar un bloque mayor que la cache, SIN pasar por ella.
 * \~
 *
 * \~english
 * The other paths write through the caches, which for a big block does two
 * useless things at once: it reads every line before overwriting it -- to take
 * ownership of a value nobody will ever look at -- and it evicts everything
 * that was there to make room for a block that will not fit anyway.  A
 * streaming store does neither: whole lines, straight to memory.
 *
 * WHERE THE LINE IS, measured rather than copied from another allocator.
 * Filling a block and then reading a fraction of it back, on a machine with 30
 * MB of last-level cache:
 *
 *     size      read nothing   read 1/16   read 1/4   read all
 *     16 MiB      NT  1.33x    NT  1.23x   stos 1.09x  stos 1.49x
 *     20 MiB      NT  1.37x    NT  1.23x   stos 1.00x  stos 1.25x
 *     24 MiB      NT  1.78x    NT  1.58x   NT   1.24x  stos 1.07x
 *     28 MiB      NT  2.11x    NT  1.67x   NT   1.34x  NT   1.05x
 *     32 MiB      NT  2.12x    NT  2.01x   NT   1.44x  NT   1.20x
 *     40 MiB      NT  2.45x    NT  2.21x   NT   1.70x  NT   1.27x
 *
 * Streaming stops losing on ANY fraction between 24 and 28 MiB, which is the
 * cache -- so the threshold is @c vesta_mem_x86_llc_bytes and not a constant.
 * Below it the block was going to be useful where it landed; above it there is
 * nothing to keep.  A CPU that does not describe its cache answers
 * @c VESTA_MEM_X86_LLC_UNKNOWN, which is huge, so nothing comes down here and
 * the older path stays -- correct, just slower.
 *
 * WHAT IT IS WORTH, at 64 MiB: 15 GB/s through the caches against 52 streaming.
 * That is also what the C library does -- glibc switches to the same stores
 * above its own threshold, which is how it stayed flat at 47 GB/s where we fell
 * to 15.  msvcrt does not, and loses to this by 3.36x.
 *
 * \~spanish
 * Los otros caminos escriben por las caches, que para un bloque grande hace dos
 * cosas inutiles a la vez: lee cada linea antes de sobreescribirla -- para
 * hacer suyo un valor que nadie va a mirar -- y desaloja todo lo que hubiera
 * para hacer sitio a un bloque que no va a caber igualmente.  Un almacen no
 * temporal no hace ninguna: lineas enteras, directas a memoria.
 *
 * DONDE ESTA LA RAYA, medido y no copiado de otro asignador.  Rellenando un
 * bloque y leyendo despues una fraccion, en una maquina con 30 MB de cache de
 * ultimo nivel:
 *
 *     tamano    sin leer      leer 1/16   leer 1/4    leer todo
 *     16 MiB    NT  1,33x     NT  1,23x   stos 1,09x  stos 1,49x
 *     20 MiB    NT  1,37x     NT  1,23x   stos 1,00x  stos 1,25x
 *     24 MiB    NT  1,78x     NT  1,58x   NT   1,24x  stos 1,07x
 *     28 MiB    NT  2,11x     NT  1,67x   NT   1,34x  NT   1,05x
 *     32 MiB    NT  2,12x     NT  2,01x   NT   1,44x  NT   1,20x
 *     40 MiB    NT  2,45x     NT  2,21x   NT   1,70x  NT   1,27x
 *
 * El no temporal deja de perder en CUALQUIER fraccion entre 24 y 28 MiB, que es
 * la cache -- asi que el umbral es @c vesta_mem_x86_llc_bytes y no una
 * constante.  Por debajo el bloque iba a servir donde cayo; por encima no hay
 * nada que conservar.  Una CPU que no describa su cache contesta
 * @c VESTA_MEM_X86_LLC_UNKNOWN, que es enorme, asi que no baja nada aqui y se
 * queda el camino de antes -- correcto, solo que mas lento.
 *
 * LO QUE VALE, a 64 MiB: 15 GB/s por las caches contra 52 sin ellas.  Es
 * tambien lo que hace la libreria de C -- glibc cambia a estos mismos almacenes
 * por encima de su propio umbral, que es como se quedaba plana en 47 GB/s donde
 * nosotros caiamos a 15 --.  msvcrt no lo hace, y pierde contra esto 3,36x.
 *
 * \~
 */
#ifndef VESTA_UTIL_MEM_X86_NT_MEMSET_H
#define VESTA_UTIL_MEM_X86_NT_MEMSET_H

#include "util/mem/x86/sse2_memset.h"
#include "util/mem/x86/x86_cpu.h"

#if defined(VESTA_MEM_ARCH_X86_64)

/**
 * @brief
 * \~english Sets @p n bytes to @p v with 16-byte writes that skip the caches.
 * \~spanish Pone @p n bytes a @p v con escrituras de 16 que se saltan las
 *           caches.
 * \~
 *
 * \~english
 * The BASE form: SSE2 is guaranteed on x86-64, so this needs no @c target and
 * can be inlined anywhere.
 *
 * The head goes through ordinary writes, and that is a requirement rather than
 * a convenience: a streaming store must be aligned, and an unaligned one is not
 * slow, it is a fault.  The tail likewise.
 *
 * @par Threads
 * Safe, as long as the buffer belongs to the caller.  The fence at the end is
 * what makes the writes visible to everybody else in the usual order; see
 * @c VESTA_MEM_FENCE_NT.
 *
 * \~spanish
 * La forma BASE: SSE2 esta garantizado en x86-64, asi que esto no necesita
 * @c target y se puede meter en linea en cualquier sitio.
 *
 * La cabeza va con escrituras normales, y eso es una exigencia y no una
 * comodidad: un almacen no temporal tiene que ir alineado, y uno sin alinear no
 * es lento, es un fallo.  La cola, igual.
 *
 * @par Hilos
 * Segura, mientras el bufer sea de quien llama.  La barrera del final es lo que
 * hace visibles las escrituras a los demas en el orden de siempre; ver
 * @c VESTA_MEM_FENCE_NT.
 *
 * \~
 * @param d
 * \~english the destination.
 * \~spanish el destino.
 * \~
 * @param v
 * \~english the byte to repeat.
 * \~spanish el byte a repetir.
 * \~
 * @param n
 * \~english how many bytes; only worth calling past the cache.
 * \~spanish cuantos bytes; solo compensa pasada la cache.
 * \~
 */
VESTA_MEM_INLINE void vesta_mem_nt_fill16(uint8_t *d, uint8_t v,
                                          size_t n) VESTA_MEM_NOEXCEPT {
    const vesta_v16 pat = vesta_mem_x86_splat16(v);

    /* La cabeza hasta el limite de 16, con una escritura NORMAL: un almacen no
     * temporal sin alinear no es lento, es un fallo.  Y solo con 32 bytes por
     * delante, que es lo que garantiza que esos 16 caben -- la misma guarda que
     * `vesta_mem_sse2_fill`, y por la misma razon.  Un bloque tan corto no
     * deberia llegar aqui, pero esto es publico y no se decide fuera. */
    if (n < 32) {
        vesta_mem_sse2_fill(d, v, n, 1);
        return;
    }
    const size_t head = vesta_mem_x86_head_to_align(d, 16);
    if (head != 0) {
        *(vesta_v16 *)(d) = pat;
        d += head;
        n -= head;
    }

    VESTA_MEM_NO_UNROLL
    while (n >= 64) {
        VESTA_MEM_KEEP_LOOP(d); // que no lo cambie POR una llamada a memset
        VESTA_MEM_STORENT16(d, pat);
        VESTA_MEM_STORENT16(d + 16, pat);
        VESTA_MEM_STORENT16(d + 32, pat);
        VESTA_MEM_STORENT16(d + 48, pat);
        d += 64;
        n -= 64;
    }
    VESTA_MEM_NO_UNROLL
    while (n >= 16) {
        VESTA_MEM_KEEP_LOOP(d);
        VESTA_MEM_STORENT16(d, pat);
        d += 16;
        n -= 16;
    }

    /* Y la barrera ANTES de la cola, no despues: lo que queda se escribe por el
     * camino normal, y asi todo lo de este relleno queda ordenado igual que
     * siempre para quien lo lea despues. */
    VESTA_MEM_FENCE_NT();
    if (n != 0) vesta_mem_sse2_fill(d, v, n, 1);
}

/**
 * @brief
 * \~english The same with 32-byte writes, when the CPU has AVX2.
 * \~spanish Lo mismo con escrituras de 32, cuando la CPU tiene AVX2.
 * \~
 *
 * \~english
 * Worth 4% to 11% over the 16-byte form in the range where either is used, on
 * a quiet machine -- which is small next to the 2x to 3x that streaming itself
 * buys, and is why the base form exists and is not a fallback nobody exercises.
 * It carries the same two consequences of @c target as the rest of the AVX2
 * paths: the binary does not require AVX2 to start, and this cannot be inlined
 * into something that does not carry it.
 *
 * @par Threads
 * The same as @c vesta_mem_nt_fill16.
 *
 * \~spanish
 * Vale un 4% a 11% sobre la forma de 16 en el tramo donde se usa cualquiera de
 * las dos, en una maquina tranquila -- que es poco al lado del 2x a 3x que da
 * el no temporal en si, y por eso la forma base existe y no es un respaldo que
 * no ejecuta nadie --.  Arrastra las mismas dos consecuencias de @c target que
 * el resto de caminos AVX2: el binario no exige AVX2 para arrancar, y esto no
 * se puede meter en linea en algo que no lo lleve.
 *
 * @par Hilos
 * La misma que @c vesta_mem_nt_fill16.
 *
 * \~
 * @param d
 * \~english the destination.
 * \~spanish el destino.
 * \~
 * @param v
 * \~english the byte to repeat.
 * \~spanish el byte a repetir.
 * \~
 * @param n
 * \~english how many bytes.
 * \~spanish cuantos bytes.
 * \~
 */
VESTA_MEM_AVX2_FN void vesta_mem_nt_fill32(uint8_t *d, uint8_t v,
                                           size_t n) VESTA_MEM_NOEXCEPT {
    const vesta_v32 pat = vesta_mem_x86_splat32(v);

    if (n < 64) { // ver la nota de `vesta_mem_nt_fill16`
        vesta_mem_sse2_fill(d, v, n, 1);
        return;
    }
    const size_t head = vesta_mem_x86_head_to_align(d, 32);
    if (head != 0) {
        VESTA_MEM_STORE32(d, pat);
        d += head;
        n -= head;
    }

    VESTA_MEM_NO_UNROLL
    while (n >= 128) {
        VESTA_MEM_KEEP_LOOP(d);
        VESTA_MEM_STORENT32(d, pat);
        VESTA_MEM_STORENT32(d + 32, pat);
        VESTA_MEM_STORENT32(d + 64, pat);
        VESTA_MEM_STORENT32(d + 96, pat);
        d += 128;
        n -= 128;
    }
    VESTA_MEM_NO_UNROLL
    while (n >= 32) {
        VESTA_MEM_KEEP_LOOP(d);
        VESTA_MEM_STORENT32(d, pat);
        d += 32;
        n -= 32;
    }

    VESTA_MEM_FENCE_NT();
    if (n != 0) vesta_mem_sse2_fill(d, v, n, 1);
}

/**
 * @brief
 * \~english Whether a fill of @p n bytes should skip the caches.
 * \~spanish Si un relleno de @p n bytes debe saltarse las caches.
 * \~
 *
 * \~english
 * ONE PLACE, so that the two entry points cannot drift apart on the answer.
 * It is the last level of cache, and everything about why is in this file's
 * header; a CPU that does not say answers a size no block reaches, so this
 * comes out false and the older path stays.
 *
 * \~spanish
 * UN SOLO SITIO, para que las dos entradas no se separen en la respuesta.  Es
 * el ultimo nivel de cache, y el por que esta entero en la cabecera de este
 * fichero; una CPU que no lo diga contesta un tamano al que no llega ningun
 * bloque, asi que esto sale falso y se queda el camino de antes.
 *
 * \~
 * @param n
 * \~english the bytes about to be filled.
 * \~spanish los bytes que se van a rellenar.
 * \~
 * @return
 * \~english non-zero when streaming stores are the right path.
 * \~spanish distinto de cero cuando el camino son los almacenes no temporales.
 * \~
 */
VESTA_MEM_ALWAYS_INLINE int
vesta_mem_nt_worth_it(size_t n) VESTA_MEM_NOEXCEPT {
    return n >= (size_t)vesta_mem_x86_llc_bytes();
}

#endif // VESTA_MEM_ARCH_X86_64

#endif // VESTA_UTIL_MEM_X86_NT_MEMSET_H
