/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file util/symbols/link_map.h
 * @brief
 * \~english De que OBJETO sale cada tramo de codigo, leido del mapa del
 *          enlazador.
 * \~spanish De que OBJETO sale cada tramo de codigo, leido del mapa del
 *          enlazador.
 * \~
 *
 * \~english
 * WHY THIS EXISTS.  Because in a release build there is nothing else.  Measured
 * on a real program: a stripped binary gives 1.174 allocation frames and NOT
 * ONE of them carries a file or a function, so every attribution that goes
 * through source paths -- declared or deduced -- is mute in the one
 * configuration that ships.  The address is all that is left.
 *
 * WHERE IT COMES FROM.  From the map the LINKER writes at build time
 * (`-Wl,-Map=...`), which is the only thing that knows which object each
 * stretch of code came from:
 *
 *     .text   0x0000000140013620   0x590   libvx_lib.a(ir_facts.cpp.obj)
 *
 * That gives the three things the report needs: the EXACT range -- no
 * neighbourhood, no approximation --, the source it was compiled from, and the
 * target it belongs to.  It is read when the report is asked for and handed to
 * @c vesta_alloc_declare_code; the binary does not change and the run pays
 * nothing.
 *
 * WHAT IT IS NOT.  It is not debug information: no lines, no function names, no
 * variables.  It answers "this address is code compiled from THIS file", which
 * is exactly the question a stripped build could not answer.
 *
 * WHY IN THIS LIBRARY and not in whoever links it: reading a GNU linker map has
 * nothing to do with any one project, it has two traps that cost a debugging
 * session each -- the map splits the row when the section name is long, and the
 * image base in the LOADED header has been patched by the loader -- and this
 * library already reads its own symbols and its own debug information.  A
 * consumer that had to write this again would write those two bugs again.
 *
 * \~spanish
 * POR QUE EXISTE.  Porque en una construccion de distribucion no hay nada mas.
 * Medido sobre un programa de verdad: un binario estripado da 1.174 marcos de
 * reserva y NI UNO lleva fichero ni funcion, asi que toda atribucion que pase
 * por rutas de fuente -- declarada o deducida -- esta muda justo en la unica
 * configuracion que se distribuye.  La direccion es lo unico que queda.
 *
 * DE DONDE SALE.  Del mapa que escribe el ENLAZADOR al construir
 * (`-Wl,-Map=...`), que es lo unico que sabe de que objeto vino cada tramo de
 * codigo:
 *
 *     .text   0x0000000140013620   0x590   libvx_lib.a(ir_facts.cpp.obj)
 *
 * De ahi salen las tres cosas que el informe necesita: el rango EXACTO -- ni
 * vecindad ni aproximacion --, el fuente del que se compilo y el objetivo al
 * que pertenece.  Se lee al pedir el informe y se entrega a
 * @c vesta_alloc_declare_code; el binario no cambia y la ejecucion no paga
 * nada.
 *
 * LO QUE NO ES.  No es informacion de depuracion: ni lineas, ni nombres de
 * funcion, ni variables.  Contesta "esta direccion es codigo compilado de ESTE
 * fichero", que es justo la pregunta que un binario estripado no sabia
 * contestar.
 *
 * POR QUE EN ESTA LIBRERIA y no en quien la enlaza: leer un mapa del enlazador
 * de GNU no tiene nada que ver con ningun proyecto en concreto, tiene dos
 * trampas que cuestan una sesion de depuracion cada una -- el mapa parte la
 * fila cuando el nombre de seccion es largo, y la base de imagen de la cabecera
 * CARGADA la ha parcheado el cargador -- y esta libreria ya se lee sus propios
 * simbolos y su propia informacion de depuracion.  Un consumidor que tuviera
 * que escribir esto otra vez escribiria esos dos fallos otra vez.
 *
 * \~
 */

#ifndef VESTA_UTIL_SYMBOLS_LINK_MAP_H
#define VESTA_UTIL_SYMBOLS_LINK_MAP_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief
 * \~english Reads `<executable>.map` and declares what it says.
 * \~spanish Lee `<ejecutable>.map` y declara lo que dice.
 * \~
 *
 * \~english
 * If the map is not there -- an installed copy, another build system -- nothing
 * happens: it answers zero and the report goes on with whatever it had.  No map
 * is LESS answer, never a wrong one.
 *
 * The memory it needs comes from the system directly, not from this allocator:
 * this runs inside the program being measured, and taking its memory from the
 * thing being measured is how a report ends up in its own figures.
 *
 * \~spanish
 * Si el mapa no esta -- una copia instalada, otro sistema de construccion -- no
 * pasa nada: contesta cero y el informe sigue con lo que tuviera.  Que no haya
 * mapa es MENOS respuesta, nunca una equivocada.
 *
 * La memoria que necesita la pide al sistema directamente, no a este asignador:
 * esto corre dentro del programa que se mide, y coger la memoria de lo que se
 * esta midiendo es como un informe acaba dentro de sus propias cifras.
 *
 * \~
 * @return
 * \~english how many stretches were declared; zero if there was no map.
 * \~spanish cuantos tramos se declararon; cero si no habia mapa.
 * \~
 */
unsigned vesta_alloc_load_link_map(void);

#ifdef __cplusplus
} // extern "C"

namespace util {

/// \~english The same, for C++.  \~spanish Lo mismo, para C++.  \~
inline unsigned alloc_load_link_map() { return vesta_alloc_load_link_map(); }

} // namespace util
#endif

#endif // VESTA_UTIL_SYMBOLS_LINK_MAP_H
