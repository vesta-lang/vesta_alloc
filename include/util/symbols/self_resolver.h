/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file util/symbols/self_resolver.h
 * @brief
 * \~english The resolver that comes ready made: names from THIS very binary.
 * \~spanish El resolutor que ya viene hecho: nombres de ESTE mismo binario.
 * \~
 *
 * \~english
 * WHAT WAS MISSING.  The library already knew how to read itself -- sections,
 * symbol table, DWARF with its inlining chain -- and it already had a hook
 * through which the export asks for names.  What was not there was the wire
 * between the two: every program had to write it, and they would all write it
 * the same way.
 *
 *     vesta_alloc_set_symbol_resolver(vesta_self_resolver);
 *
 * With that one line the export comes out with names, files and lines instead
 * of offsets.  It is still a HOOK and not a decision of the library's: whoever
 * has a better way to resolve -- a symbol server, a map generated at link time
 * -- installs theirs and this one gets out of the way.
 *
 * THIS HEADER IS PURE C, so that a program written in C has exactly the same
 * capability as one in C++.  See `examples/c_symbol_report.c`, which uses it
 * and is compiled AS C precisely so that this does not stop being true.
 *
 * \~spanish
 * QUE FALTABA.  La libreria ya sabia leerse a si misma -- secciones, tabla de
 * simbolos, DWARF con su cadena de inline -- y ya tenia un gancho por el que
 * la exportacion pide nombres.  Lo que no habia era el cable entre las dos
 * cosas: cada programa tenia que escribirlo, y todos lo escribirian igual.
 *
 *     vesta_alloc_set_symbol_resolver(vesta_self_resolver);
 *
 * Con esa linea la exportacion sale con nombres, ficheros y lineas en vez de
 * desplazamientos.  Sigue siendo un GANCHO y no una decision de la libreria:
 * quien tenga una forma mejor de resolver -- un servidor de simbolos, un mapa
 * generado al enlazar -- instala la suya y esta no estorba.
 *
 * ESTA CABECERA ES C PURO, para que un programa en C tenga exactamente la
 * misma capacidad que uno en C++.  Ver `examples/c_symbol_report.c`, que la
 * usa y se compila COMO C precisamente para que eso no deje de ser cierto.
 *
 * \~
 */
#ifndef VESTA_UTIL_SELF_RESOLVER_H
#define VESTA_UTIL_SELF_RESOLVER_H

#include "util/report/alloc_csv_c.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief
 * \~english What this binary knows about an address of its own.
 * \~spanish Que sabe este binario de una direccion suya.
 * \~
 *
 * \~english
 * It tries THREE things, in order, and each one gives less than the previous:
 *
 *   1. The debug information, which gives the whole chain of inlined functions
 *      with their file and line.  It is the only one that says which call in
 *      one's own code ended up allocating: the symbol names the outer function,
 *      and with the optimiser on that one has eaten another twenty.
 *   2. The symbol table, which gives ONE frame and no file.
 *   3. `.pdata` -- or `st_size` on ELF -- which gives no name but does give
 *      where the function containing the address starts.  In a stripped build
 *      it is the only thing left, and with it the tree at least groups every
 *      site of the same function under the same node.
 *
 * What it does NOT fill in is `module`: which module a file belongs to depends
 * on how each project is laid out -- here a directory under `src/`, elsewhere a
 * package or a crate -- so that is put in by whoever knows.
 *
 * @par Threads
 * It can be called from several, but it is meant for the end of the process.
 *
 * \~spanish
 * Intenta TRES cosas, en orden, y cada una da menos que la anterior:
 *
 *   1. La informacion de depuracion, que da la cadena entera de funciones
 *      inlineadas con su fichero y su linea.  Es la unica que dice que
 *      llamada del codigo de uno acabo reservando: el simbolo nombra a la
 *      funcion de fuera, y con el optimizador encendido esa se ha comido a
 *      otras veinte.
 *   2. La tabla de simbolos, que da UN marco y sin fichero.
 *   3. `.pdata` -- o `st_size` en ELF --, que no da nombre pero si donde
 *      empieza la funcion que contiene la direccion.  En una construccion
 *      despojada es lo unico que queda, y con ello el arbol al menos agrupa
 *      todos los sitios de una misma funcion bajo el mismo nodo.
 *
 * Lo que NO rellena es `module`: de que modulo es un fichero depende de como
 * este organizado cada proyecto -- aqui un directorio bajo `src/`, en otro
 * sitio un paquete o un crate --, asi que eso lo pone quien lo sepa.
 *
 * @par Hilos
 * Se puede llamar desde varios, pero esta pensado para el final del proceso.
 *
 * \~
 * @param pc
 * \~english the address, as it is loaded.
 * \~spanish la direccion, tal como esta cargada.
 * \~
 * @param out
 * \~english frames, from innermost to outermost.
 * \~spanish marcos, de dentro hacia fuera.
 * \~
 * @param max
 * \~english how many fit.
 * \~spanish cuantos caben.
 * \~
 * @return
 * \~english how many were written.  Zero means "nothing is known about it",
 *           which is a legitimate answer and not an error.
 * \~spanish cuantos se escribieron.  Cero significa "no se sabe nada de ella",
 *           que es una respuesta legitima y no un error.
 * \~
 */
unsigned vesta_self_resolver(const void *pc, VestaAllocFrame *out,
                             unsigned max);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VESTA_UTIL_SELF_RESOLVER_H */
