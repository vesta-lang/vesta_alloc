/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/alloc/alloc_tag_c.h
 * @brief
 * \~english What an allocation can DECLARE about itself, for C.
 * \~spanish Lo que una reserva puede DECLARAR de si misma, para C.
 * \~
 *
 * \~english
 * THE VALUES LIVE HERE AND ONLY HERE.  The C++ side -- @c util::AllocUse and
 * the rest, in @c util/alloc/alloc_tag.h -- is defined in terms of these, so
 * the two languages cannot drift apart: there is one place where a value is
 * decided and one place to change it.  That matters more than it sounds,
 * because the numbers travel: they index counter tables, they cross the C
 * boundary in @c vesta_host_push_tag, and they end up in the CSV that a report
 * is read from.  Two lists that agree today and not tomorrow would not fail --
 * they would count in the wrong bucket, quietly.
 *
 * The C++ names are the readable ones and stay a strong enum there; these are
 * what makes them ASKABLE from C, which until now they were not: the entry
 * points took bare integers and the meaning lived in a doc comment.
 *
 * ZERO IS ALWAYS "NOBODY SAID" on every axis, which is what an uninitialised
 * structure gives for free, and it is the honest default: one that looks like
 * knowledge does not fail, it lies.
 *
 * \~spanish
 * LOS VALORES VIVEN AQUI Y SOLO AQUI.  El lado de C++ -- @c util::AllocUse y
 * los demas, en @c util/alloc/alloc_tag.h -- se define en terminos de estos,
 * asi que los dos lenguajes no pueden separarse: hay un sitio donde se decide
 * un valor y un sitio donde cambiarlo.  Importa mas de lo que parece, porque
 * los numeros VIAJAN: indexan tablas de contadores, cruzan la frontera de C en
 * @c vesta_host_push_tag y acaban en el CSV con el que se lee un informe.  Dos
 * listas que hoy coinciden y manana no, no fallarian -- contarian en la casilla
 * equivocada, en silencio.
 *
 * Los nombres de C++ son los legibles y alli siguen siendo un enum fuerte;
 * estos son lo que los hace PREGUNTABLES desde C, que hasta ahora no lo eran:
 * las entradas tomaban enteros pelados y el significado vivia en un comentario.
 *
 * EL CERO ES SIEMPRE "NADIE LO DIJO" en todos los ejes, que es lo que da gratis
 * una estructura sin inicializar, y es el valor por defecto honesto: uno que
 * parece saber no falla, miente.
 *
 * \~
 */
#ifndef VESTA_UTIL_ALLOC_TAG_C_H
#define VESTA_UTIL_ALLOC_TAG_C_H

/**
 * @brief
 * \~english How long the allocation lives.  The order matters: shortest first.
 * \~spanish Cuanto vive lo reservado.  El orden importa: va de menos a mas.
 * \~
 */
enum VestaAllocUse {
    /** \~english nobody said  \~spanish nadie lo dijo  \~ */
    VestaUse_Unknown = 0,
    /** \~english dies inside the operation that asked for it
     *  \~spanish muere dentro de la operacion que lo pidio  \~ */
    VestaUse_Instant = 1,
    /** \~english lives as long as one phase
     *  \~spanish vive lo que dure una fase  \~ */
    VestaUse_Medium = 2,
    /** \~english lives as long as the process
     *  \~spanish vive lo que dure el proceso  \~ */
    VestaUse_Long = 3
};

/**
 * @brief
 * \~english Whether the allocation changes size during its life.
 * \~spanish Si lo reservado cambia de tamano a lo largo de su vida.
 * \~
 */
enum VestaAllocShape {
    /** \~english nobody said  \~spanish nadie lo dijo  \~ */
    VestaShape_Unknown = 0,
    /** \~english asked for once and that is it
     *  \~spanish se pide una vez y ya  \~ */
    VestaShape_Fixed = 1,
    /** \~english a container that will grow and abandon this buffer
     *  \~spanish un contenedor que se hara mayor y abandonara este buffer  \~ */
    VestaShape_Growing = 2
};

/**
 * @brief
 * \~english How much of the block the caller is going to touch.
 * \~spanish Cuanto del bloque va a tocar el llamante.
 * \~
 *
 * \~english
 * THE ONE THE ALLOCATOR CANNOT WORK OUT FOR ITSELF, and the one that decides.
 * Handing back a big zeroed block has two ways to be right and they are
 * opposites: keep the block and CLEAR it, whose cost is flat in what gets read
 * because the whole thing is written; or ask the system for a fresh one, which
 * arrives already zero and costs a page fault for every page actually touched.
 * Measured across sizes and fractions, they cross at a FRACTION -- between 1/16
 * and 1/4 -- and NOT at a size: the same 8 MiB request wins one way read
 * sparsely and the other way read whole.  So no threshold on size can choose,
 * and the only one who knows is the caller.
 *
 * \~spanish
 * EL QUE EL ASIGNADOR NO PUEDE AVERIGUAR SOLO, y el que decide.  Entregar un
 * bloque grande a cero tiene dos formas de estar bien y son opuestas: quedarse
 * el bloque y LIMPIARLO, cuyo coste es plano en lo que se lea porque se escribe
 * entero; o pedirle uno fresco al sistema, que llega ya a cero y cuesta un
 * fallo de pagina por cada pagina que se toque de verdad.  Medido a lo largo de
 * tamanos y fracciones, se cruzan en una FRACCION -- entre 1/16 y 1/4 -- y NO
 * en un tamano: la misma peticion de 8 MiB gana de una forma leida a trozos y
 * de la otra leida entera.  Asi que ningun umbral por tamano puede elegir, y el
 * unico que lo sabe es quien llama.
 *
 * \~
 */
enum VestaAllocFill {
    /** \~english nobody said  \~spanish nadie lo dijo  \~ */
    VestaFill_Unknown = 0,
    /** \~english a small part of it gets touched
     *  \~spanish se toca una parte pequena  \~ */
    VestaFill_Sparse = 1,
    /** \~english a good part of it  \~spanish buena parte de el  \~ */
    VestaFill_Dense = 2,
    /** \~english all of it  \~spanish entero  \~ */
    VestaFill_All = 3
};

/**
 * @brief
 * \~english How many distinct purpose tags there are (4 uses x 4 shapes).
 * \~spanish Cuantas etiquetas de proposito distintas hay (4 usos x 4 formas).
 * \~
 *
 * \~english
 * A MACRO AND NOT AN ENUMERATOR, because it sizes arrays in structures that C
 * and C++ share, and it has to work where a constant expression is required in
 * both.  The two axes are packed into four bits so the byte can index the
 * counter table directly; four of the sixteen combinations go unused, and they
 * are cheaper than a multiplication per allocation.
 *
 * \~spanish
 * UNA MACRO Y NO UN ENUMERADOR, porque dimensiona arrays de estructuras que
 * comparten C y C++, y tiene que valer donde hace falta una expresion constante
 * en los dos.  Los dos ejes van empaquetados en cuatro bits para que el byte
 * indexe directamente la tabla de contadores; sobran cuatro combinaciones de
 * las dieciseis, y salen mas baratas que una multiplicacion por reserva.
 *
 * \~
 */
#define VESTA_ALLOC_TAG_SLOTS 16

/**
 * @brief
 * \~english How many values the "how much will be touched" axis takes.
 * \~spanish Cuantos valores toma el eje de "cuanto se va a tocar".
 * \~
 *
 * \~english
 * SEPARATE FROM THE TAG, so it does NOT multiply the number above.  Those two
 * axes are packed together precisely so the byte indexes the counter table, and
 * a third one there would take it from 16 slots to 64 -- 384 KiB more of static
 * memory across every cache, to split a count only one call ever asks about.
 * It gets a table of its own instead.
 *
 * \~spanish
 * APARTE DE LA ETIQUETA, asi que NO multiplica la cifra de arriba.  Esos dos
 * ejes van empaquetados juntos precisamente para que el byte indexe la tabla de
 * contadores, y un tercero ahi la llevaria de 16 ranuras a 64 -- 384 KiB mas de
 * memoria estatica entre todos los caches, para repartir una cuenta que solo
 * pregunta una llamada.  Tiene tabla propia en su lugar.
 *
 * \~
 */
#define VESTA_ALLOC_FILL_SLOTS 4

#endif /* VESTA_UTIL_ALLOC_TAG_C_H */
