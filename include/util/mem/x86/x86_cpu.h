/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/mem/x86/x86_cpu.h
 * @brief Que sabe hacer la CPU en la que estamos corriendo.
 *
 * Esto es lo que separa "el camino esta COMPILADO" de "el camino se USA".  Las
 * funciones de AVX2 se compilan siempre en x86-64 -- con @c target por funcion,
 * asi que el binario no exige AVX2 para arrancar --, y quien decide si se
 * llaman es esto, en ejecucion.
 *
 * Cada capacidad nueva (AVX-512, @c rep @c movsb rapido...) anade aqui su
 * pregunta, y NO en el codigo de la copia: asi el despachador sigue siendo una
 * lista de ramas y las rutinas no saben nada de deteccion.
 */
#ifndef VESTA_UTIL_MEM_X86_CPU_H
#define VESTA_UTIL_MEM_X86_CPU_H

#include "util/mem/mem_config.h"

#if defined(VESTA_MEM_ARCH_X86_64)

/* Las instrucciones estan en su propia cabecera; aqui solo se interpretan sus
 * banderas.  Preguntar y decidir son dos cosas distintas. */
#include "util/mem/x86/x86_cpuid.h"

/* Una bandera por capacidad.  Anadir una es anadir su bit aqui y su linea en
 * `vesta_mem_x86_detect`, sin tocar nada mas: ni las rutinas ni el despacho. */
#define VESTA_MEM_X86_READY (1u << 0) ///< ya se ha preguntado
#define VESTA_MEM_X86_AVX2 (1u << 1)  ///< movimientos de 32 bytes
#define VESTA_MEM_X86_ERMS (1u << 2)  ///< `rep movsb`/`rep stosb` RAPIDOS

/**
 * @brief Lo que sabe hacer esta CPU.  Cero mientras no se haya preguntado.
 *
 * Es @c static, o sea una copia por unidad de traduccion.  Suena a desperdicio
 * y no lo es: son cuatro bytes y una consulta mas por unidad, a cambio de que
 * la cabecera no necesite ningun simbolo global -- que es lo que la deja valer
 * en C, donde no hay variables @c inline.
 */
static unsigned int vesta_mem_x86_features_state = 0;

/**
 * @brief Pregunta a la CPU con @c CPUID por todo lo que aqui se usa.  Se hace
 *        una vez; el resultado no cambia.
 *
 * POR QUE @c CPUID A MANO Y NO @c __builtin_cpu_supports.  Porque el builtin no
 * es codigo, es una LLAMADA a la biblioteca de soporte del compilador: deja
 * @c __cpu_model y @c __cpu_indicator_init sin resolver, que los pone libgcc.
 * Eso rompe las dos cosas que esta libreria promete -- funcionar sin biblioteca
 * de sistema, y compilar con cualquier compilador --: con Clang apuntando a
 * MSVC no existe ese simbolo y el ENLACE FALLA, comprobado.  @c CPUID no
 * depende de nadie.
 *
 * Y LA COMPROBACION QUE NO SE PUEDE SALTAR es la del sistema operativo, no la
 * de la CPU.  Que el procesador tenga AVX2 no basta: el sistema tiene que estar
 * GUARDANDO los registros anchos al cambiar de tarea, y eso se pregunta con
 * @c XGETBV.  Si se usa AVX2 donde el sistema no salva YMM, lo que se corrompe
 * son los registros de otro hilo -- un fallo que no se parece a un fallo y que
 * aparece en cualquier otro sitio --.  El builtin hacia esta comprobacion por
 * dentro; al quitarlo hay que hacerla aqui.
 *
 * El estado se guarda con los atomicos del COMPILADOR (@c __atomic_store_n) y
 * no con @c <atomic>, porque esta cabecera la compila tambien un compilador de
 * C -- y de paso no arrastra una cabecera de C++ a un camino que quiere ser
 * freestanding.
 *
 * @return Las banderas @c VESTA_MEM_X86_*, siempre con @c READY puesto.
 *
 * @par Hilos
 * Segura.  Dos hilos que lleguen a la vez preguntan los dos y escriben lo
 * MISMO, asi que la carrera no puede dar un resultado incorrecto y no hace
 * falta sincronizar nada mas.
 *
 * @code
 *   const unsigned int f = vesta_mem_x86_detect();   // fuerza la consulta
 * @endcode
 */
VESTA_MEM_INLINE unsigned int vesta_mem_x86_detect(void) VESTA_MEM_NOEXCEPT {
    unsigned int f = VESTA_MEM_X86_READY;

    /* Todo lo de aqui se anuncia en la hoja 7, asi que una CPU que no llegue
     * hasta ella no tiene nada que decir. */
    if (vesta_mem_x86_cpuid_max() >= 7) {
        const vesta_cpuid_regs f1 = vesta_mem_x86_cpuid(1, 0);
        const vesta_cpuid_regs f7 = vesta_mem_x86_cpuid(7, 0);

        /* OSXSAVE es el SISTEMA diciendo que ha habilitado XGETBV; AVX es la
         * CPU diciendo que tiene los registros anchos.  Hacen falta los dos, y
         * en ese orden: sin OSXSAVE ni siquiera se puede ejecutar XGETBV.
         * Bits 1 y 2 de XCR0: el sistema guarda el estado de XMM y el de YMM.
         * Los dos, no uno: con YMM sin guardar, AVX2 pisa registros ajenos. */
        const int osxsave = (f1.ecx & (1u << 27)) != 0;
        const int avx = (f1.ecx & (1u << 28)) != 0;
        if (osxsave && avx && (vesta_mem_x86_xgetbv(0) & 0x6u) == 0x6u) {
            if ((f7.ebx & (1u << 5)) != 0) f |= VESTA_MEM_X86_AVX2;
        }

        /* ERMS (bit 9) es la CPU diciendo que su `rep movsb` esta acelerado por
         * microcodigo.  NO se pregunta al sistema nada: `rep movsb` no usa
         * registros que haya que salvar, asi que no hay un XGETBV que valga. */
        if ((f7.ebx & (1u << 9)) != 0) f |= VESTA_MEM_X86_ERMS;
    }

    __atomic_store_n(&vesta_mem_x86_features_state, f, __ATOMIC_RELAXED);
    return f;
}

/**
 * @brief Lo mismo por el camino caliente: una lectura relajada y una rama que
 *        acierta siempre menos la primera vez.
 *
 * @return Las banderas @c VESTA_MEM_X86_*.
 *
 * @par Hilos
 * Segura.
 *
 * @code
 *   if (vesta_mem_x86_features() & VESTA_MEM_X86_AVX2) { ... }
 * @endcode
 */
VESTA_MEM_ALWAYS_INLINE unsigned int
vesta_mem_x86_features(void) VESTA_MEM_NOEXCEPT {
    const unsigned int f =
        __atomic_load_n(&vesta_mem_x86_features_state, __ATOMIC_RELAXED);
    if ((f & VESTA_MEM_X86_READY) != 0) return f;
    return vesta_mem_x86_detect();
}

/**
 * @brief Se pueden usar los movimientos de 32 bytes?
 *
 * @return distinto de 0 si la CPU tiene AVX2 y el sistema lo respalda.
 *
 * @par Hilos
 * Segura.
 *
 * @code
 *   if (vesta_mem_x86_has_avx2()) vesta_mem_avx2_copy(d, s, n);
 * @endcode
 */
VESTA_MEM_ALWAYS_INLINE int vesta_mem_x86_has_avx2(void) VESTA_MEM_NOEXCEPT {
#if defined(VESTA_MEM_TARGET_AVX2)
    /* La micro-ISA vino fijada al compilar: el binario no arranca en una
     * maquina sin AVX2, asi que la respuesta es constante y con ella se pliegan
     * tambien la rama y todo el camino que no se toma. */
    return 1;
#else
    return (vesta_mem_x86_features() & VESTA_MEM_X86_AVX2) != 0;
#endif
}

/**
 * @brief Es rapido el @c rep @c movsb de esta CPU?
 *
 * La pregunta importa porque @c rep @c movsb existe desde el 8086 y en las
 * maquinas viejas es LENTO -- se ejecuta byte a byte --.  A partir de ERMS el
 * microcodigo lo convierte en el bucle mas rapido que la CPU sabe hacer, que
 * ademas conoce sus propios anchos y su propia jerarquia de cache.
 *
 * @return distinto de 0 si @c rep @c movsb esta acelerado.
 *
 * @par Hilos
 * Segura.
 *
 * @code
 *   if (vesta_mem_x86_has_erms()) vesta_mem_erms_copy(d, s, n);
 * @endcode
 */
VESTA_MEM_ALWAYS_INLINE int vesta_mem_x86_has_erms(void) VESTA_MEM_NOEXCEPT {
#if defined(VESTA_MEM_TARGET_ERMS)
    /* Aqui no hay un `__ERMS__` que mirar -- no es una extension de conjunto de
     * instrucciones, es una propiedad del microcodigo --, asi que esto solo se
     * activa a mano, para quien sepa en que maquina va a ejecutar. */
    return 1;
#else
    return (vesta_mem_x86_features() & VESTA_MEM_X86_ERMS) != 0;
#endif
}

#endif // VESTA_MEM_ARCH_X86_64

#endif // VESTA_UTIL_MEM_X86_CPU_H
