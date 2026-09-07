/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file util/symbols/self_inspect_c.h
 * @brief
 * \~english Looking at oneself, with the signatures C understands.
 * \~spanish Mirarse a uno mismo, con las firmas que entiende C.
 * \~
 *
 * \~english
 * PARITY, NOT A SUBSET.  Everything a C++ program can ask about its own binary
 * can be asked from C, with the same answer.  It is not convenience: in this
 * project the C++ CALLS C code, and several of the dependencies it carries
 * inside are pure C.  A capability that only exists in C++ is a capability half
 * the program does not have -- and precisely the half whose memory was
 * invisible until now.
 *
 * WHAT CAN BE ASKED, in order of how much each one says:
 *
 *   - `vesta_self_frames`   the CHAIN of inlined functions of an address, with
 *                           file and line.  It is the only one that says which
 *                           call in one's own code ended up there: the symbol
 *                           names the outer function, and with the optimiser on
 *                           that one has eaten another twenty.
 *   - `vesta_self_symbol`   the name of the function containing the address.
 *   - `vesta_self_function_range`  where it starts and how big it is, even
 *                           without a name.  It survives `--strip-all`.
 *
 * And the image ones -- path, link base, sections -- for whoever wants to read
 * their own tables.
 *
 * THEY ALL RETURN "I DO NOT KNOW" DISTINGUISHABLY.  Zero frames, a null pointer
 * or false, as the case may be, and never an invented plausible value: not
 * being able to say whose an address is is a legitimate answer, and confusing
 * it with an answer is how reports that lie get read.
 *
 * THIS HEADER IS PURE C.  No templates, no references, no namespaces.  It can
 * be included from a `.c`, and it is: see `examples/c_symbol_report.c`.
 *
 * \~spanish
 * PARIDAD, NO UN SUBCONJUNTO.  Todo lo que un programa en C++ puede preguntar
 * sobre su propio binario se puede preguntar desde C, con la misma respuesta.
 * No es comodidad: en este proyecto el C++ LLAMA a codigo C, y varias de las
 * dependencias que lleva dentro son C puro.  Una capacidad que solo existe en
 * C++ es una capacidad que la mitad del programa no tiene -- y justo la mitad
 * cuya memoria era invisible hasta ahora.
 *
 * QUE SE PUEDE PREGUNTAR, en orden de cuanto dice cada una:
 *
 *   - `vesta_self_frames`   la CADENA de funciones inlineadas de una
 *                           direccion, con fichero y linea.  Es la unica que
 *                           dice que llamada del codigo de uno acabo ahi: el
 *                           simbolo nombra a la funcion de fuera, y con el
 *                           optimizador encendido esa se ha comido a otras
 *                           veinte.
 *   - `vesta_self_symbol`   el nombre de la funcion que contiene la direccion.
 *   - `vesta_self_function_range`  donde empieza y cuanto mide, aunque no haya
 *                           nombre.  Sobrevive a `--strip-all`.
 *
 * Y las de la imagen -- ruta, base de enlace, secciones -- para quien quiera
 * leer sus propias tablas.
 *
 * TODAS DEVUELVEN "NO SE" DE FORMA DISTINGUIBLE.  Cero marcos, puntero nulo o
 * falso, segun el caso, y nunca un valor plausible inventado: no poder decir
 * de quien es una direccion es una respuesta legitima, y confundirla con una
 * respuesta es como se leen informes que mienten.
 *
 * ESTA CABECERA ES C PURO.  Nada de plantillas, referencias ni namespaces.  Se
 * puede incluir desde un `.c`, y se hace: ver `examples/c_symbol_report.c`.
 *
 * \~
 */
#ifndef VESTA_UTIL_SELF_INSPECT_C_H
#define VESTA_UTIL_SELF_INSPECT_C_H

#include "util/report/alloc_csv_c.h" /* VestaAllocFrame */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 *  La cadena de una direccion
 * ------------------------------------------------------------------------- */

/**
 * @brief
 * \~english The frames containing @p pc, from innermost to outermost.
 * \~spanish Los marcos que contienen @p pc, de dentro hacia fuera.
 * \~
 *
 * \~english
 * The last one is the function that really exists in the binary; the previous
 * ones were inlined inside it and carry `inlined` set.
 *
 * It is the SAME function that gets installed as the export's resolver
 * (`vesta_self_resolver`, in `util/symbols/self_resolver.h`); it has two names
 * because it has two uses, and the hook's should not force anybody to read the
 * export's documentation in order to use it on its own -- from a crash handler,
 * say, which is where it is needed most.
 *
 * \~spanish
 * El ultimo es la funcion que existe de verdad en el binario; los anteriores
 * se inlinearon dentro de ella y llevan `inlined` a uno.
 *
 * Es la MISMA funcion que se instala como resolutor de la exportacion
 * (`vesta_self_resolver`, en `util/symbols/self_resolver.h`); tiene dos nombres porque
 * tiene dos usos, y el del gancho no deberia obligar a leer la documentacion
 * de la exportacion para usarla suelta -- por ejemplo desde un manejador de
 * fallos, que es donde mas falta hace.
 *
 * \~
 * @param pc
 * \~english a code address of this process.
 * \~spanish una direccion de codigo de este proceso.
 * \~
 * @param out
 * \~english the caller's array.
 * \~spanish array del que llama.
 * \~
 * @param max
 * \~english how many fit.
 * \~spanish cuantos caben.
 * \~
 * @return
 * \~english how many were written; zero when nothing is known about that
 *           address.
 * \~spanish cuantos se escribieron; cero si no se sabe nada de esa direccion.
 * \~
 */
unsigned vesta_self_frames(const void *pc, VestaAllocFrame *out, unsigned max);

/**
 * @brief
 * \~english Whether the debug information covers @p pc.
 * \~spanish Si la informacion de depuracion cubre @p pc.
 * \~
 *
 * \~english
 * It serves to know whether the answer above is going to be the good one or is
 * going to fall back to the symbol's name, without having to ask for it.
 *
 * \~spanish
 * Sirve para saber si la respuesta de arriba va a ser la buena o va a caer al
 * nombre del simbolo, sin tener que pedirla.
 *
 * \~
 * @param pc
 * \~english the address to ask about.
 * \~spanish la direccion por la que se pregunta.
 * \~
 * @return
 * \~english other than zero when it covers it.
 * \~spanish distinto de cero si la cubre.
 * \~
 */
int vesta_self_covers(const void *pc);

/* ---------------------------------------------------------------------------
 *  La tabla de simbolos
 * ------------------------------------------------------------------------- */

/**
 * @brief
 * \~english The name of the function containing @p pc.
 * \~spanish El nombre de la funcion que contiene @p pc.
 * \~
 *
 * \~english
 * RAW, exactly as it stands in the table: if the binary is C++ it will come
 * mangled.  Making it readable is another question and each project answers it
 * -- see the name hook in `util/report/alloc_csv_c.h` -- because a C binary, a
 * Rust one and a C++ one do not mangle the same way.
 *
 * \~spanish
 * CRUDO, tal como esta en la tabla: si el binario es de C++ vendra manglado.
 * Ponerlo legible es otra pregunta y la contesta cada proyecto -- ver el
 * gancho de nombres en `util/report/alloc_csv_c.h` --, porque un binario de C, uno de
 * Rust y uno de C++ no manglan igual.
 *
 * \~
 * @param pc
 * \~english a code address of this process.
 * \~spanish una direccion de codigo de este proceso.
 * \~
 * @param offset
 * \~english if not NULL, how far it is from the function's start.
 * \~spanish si no es NULL, cuanto hay desde el principio de la funcion.
 * \~
 * @return
 * \~english the name, valid as long as the process, or NULL when it is not
 *           known.
 * \~spanish el nombre, valido mientras el proceso, o NULL si no se sabe.
 * \~
 */
const char *vesta_self_symbol(const void *pc, size_t *offset);

/**
 * @brief
 * \~english The range of the function containing @p pc.
 * \~spanish El tramo de la funcion que contiene @p pc.
 * \~
 *
 * \~english
 * WHY, given the name.  Because a symbol says where a function STARTS and not
 * where it ends, so with the table alone the only possible answer is "from the
 * nearest previous symbol" -- and that is right while the address falls inside,
 * and lies in silence as soon as it falls into a gap.
 *
 * And it survives `--strip-all`: it comes out of `.pdata` on PE and of
 * `st_size` on ELF, which are SECTIONS and not a symbol table.
 *
 * \~spanish
 * POR QUE, teniendo el nombre.  Porque un simbolo dice donde EMPIEZA una
 * funcion y no donde acaba, asi que con la tabla sola la unica respuesta
 * posible es "del simbolo anterior mas cercano" -- y eso acierta mientras la
 * direccion caiga dentro, y miente en silencio en cuanto cae en un hueco.
 *
 * Y sobrevive a `--strip-all`: sale de `.pdata` en PE y de `st_size` en ELF,
 * que son SECCIONES y no una tabla de simbolos.
 *
 * \~
 * @param pc
 * \~english a code address of this process.
 * \~spanish una direccion de codigo de este proceso.
 * \~
 * @param begin
 * \~english if not NULL, where it starts.
 * \~spanish si no es NULL, donde empieza.
 * \~
 * @param bytes
 * \~english if not NULL, how big it is.
 * \~spanish si no es NULL, cuanto mide.
 * \~
 * @return
 * \~english other than zero when it could be told.
 * \~spanish distinto de cero si se supo.
 * \~
 */
int vesta_self_function_range(const void *pc, const void **begin,
                              size_t *bytes);

/* ---------------------------------------------------------------------------
 *  Cuanto se pudo leer, para poder decir por que no se sabe
 * ------------------------------------------------------------------------- */

/**
 * @brief
 * \~english How many symbols were read.
 * \~spanish Cuantos simbolos se leyeron.
 * \~
 * @return
 * \~english the count.  Zero = no symbol table.
 * \~spanish la cuenta.  Cero = sin tabla de simbolos.
 * \~
 */
size_t vesta_self_symbol_count(void);

/**
 * @brief
 * \~english How many function ranges were read.
 * \~spanish Cuantos tramos de funcion se leyeron.
 * \~
 * @return
 * \~english the count.  Zero = no unwind data either.
 * \~spanish la cuenta.  Cero = tampoco hay datos de desenrollado.
 * \~
 */
size_t vesta_self_function_count(void);

/**
 * @brief
 * \~english How many compilation units there are.
 * \~spanish Cuantas unidades de compilacion hay.
 * \~
 * @return
 * \~english the count.  Zero = no DWARF.
 * \~spanish la cuenta.  Cero = sin DWARF.
 * \~
 */
size_t vesta_self_dwarf_units(void);

/**
 * @brief
 * \~english How many entries the address index has.
 * \~spanish Cuantas entradas tiene el indice de direcciones.
 * \~
 * @return
 * \~english the count.  Units without ranges means the headers were read and
 *           their ranges were not understood.
 * \~spanish la cuenta.  Unidades sin tramos significa que se leyeron las
 *           cabeceras y no se entendieron sus rangos.
 * \~
 */
size_t vesta_self_dwarf_ranges(void);

/* ---------------------------------------------------------------------------
 *  La imagen
 * ------------------------------------------------------------------------- */

/**
 * @brief
 * \~english The path of the running executable.
 * \~spanish La ruta del ejecutable en marcha.
 * \~
 * @param buf
 * \~english where the path goes.
 * \~spanish donde va la ruta.
 * \~
 * @param cap
 * \~english the size of @p buf in bytes.
 * \~spanish el tamano de @p buf en bytes.
 * \~
 * @return
 * \~english its length, even when it does not fit -- that is how truncation
 *           shows.  Zero when it could not be found out.
 * \~spanish su longitud, aunque no quepa -- asi se ve que se trunco.  Cero si
 *           no se pudo averiguar.
 * \~
 */
size_t vesta_self_image_path(char *buf, size_t cap);

/**
 * @brief
 * \~english The address the binary was LINKED for.
 * \~spanish La direccion para la que se ENLAZO el binario.
 * \~
 *
 * \~english
 * It is not where it is loaded: with layout randomisation those change on every
 * run, and the debug tables speak in the link-time one.  Subtracting them is
 * what turns one into the other.
 *
 * \~spanish
 * No es donde esta cargado: con la disposicion aleatoria cambian en cada
 * corrida, y las tablas de depuracion hablan en la de enlace.  Restarlas es lo
 * que convierte una en la otra.
 *
 * \~
 * @return
 * \~english the link base, or zero when it cannot be told.
 * \~spanish la base de enlace, o cero si no se sabe.
 * \~
 */
uint64_t vesta_self_image_link_base(void);

/**
 * @brief
 * \~english Where a section is INSIDE THE FILE and how big it is.
 * \~spanish Donde esta una seccion DENTRO DEL FICHERO y cuanto mide.
 * \~
 * @param name
 * \~english the section's name.
 * \~spanish el nombre de la seccion.
 * \~
 * @param offset
 * \~english receives its displacement in the file.
 * \~spanish recibe su desplazamiento en el fichero.
 * \~
 * @param bytes
 * \~english receives its useful size.
 * \~spanish recibe su tamano util.
 * \~
 * @return
 * \~english other than zero when it exists.
 * \~spanish distinto de cero si existe.
 * \~
 */
int vesta_self_section_range(const char *name, uint64_t *offset,
                             uint64_t *bytes);

/**
 * @brief
 * \~english Copies a piece of a section into the caller's memory.
 * \~spanish Copia un trozo de una seccion en memoria del que llama.
 * \~
 *
 * \~english
 * The caller provides the memory, which is how things are done in C: that way
 * there is no block to return and nobody has to remember who frees it.  Asking
 * past the end is NOT quietly trimmed -- it returns zero -- because a short
 * piece reads as a truncated structure and that gives things that look valid.
 *
 * \~spanish
 * El que llama pone la memoria, que es como se hacen las cosas en C: asi no
 * hay que devolver un bloque y acordarse de quien lo libera.  Pedir mas alla
 * del final NO se recorta en silencio -- devuelve cero --, porque un trozo
 * corto se lee como una estructura truncada y eso da cosas que parecen
 * validas.
 *
 * \~
 * @param name
 * \~english the section.
 * \~spanish la seccion.
 * \~
 * @param offset
 * \~english from its start.
 * \~spanish desde su principio.
 * \~
 * @param dst
 * \~english where the bytes go.
 * \~spanish donde van los bytes.
 * \~
 * @param bytes
 * \~english how many.
 * \~spanish cuantos.
 * \~
 * @return
 * \~english how many bytes were copied; zero when the section is not there or
 *           the piece runs off it.
 * \~spanish cuantos bytes se copiaron; cero si la seccion no esta o el trozo se
 *           sale de ella.
 * \~
 */
size_t vesta_self_section_read(const char *name, uint64_t offset, void *dst,
                               size_t bytes);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VESTA_UTIL_SELF_INSPECT_C_H */
