/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file util/report/alloc_declare_tu.h
 * @brief
 * \~english Every source file says which file it is -- forced by the build.
 * \~spanish Cada fuente dice que fichero es -- forzado por el build.
 * \~
 *
 * \~english
 * NOT MEANT TO BE INCLUDED BY HAND.  The build pushes it into every translation
 * unit (`vesta_alloc_declare_files(<target>)`, which adds `-include`), and that
 * is the point: a convention written by hand in seven hundred files is a
 * convention that gets forgotten in the seven hundred and first -- and a file
 * that forgets does not simply go missing from the report.  Its allocations
 * land on the PREVIOUS file's marker and come out with that file's name on
 * them, which is worse than not answering.
 *
 * WHAT IT COSTS.  One anchor function that does nothing, one static object and
 * one call at start-up per unit; the call appends to a fixed table and
 * allocates nothing.  Nothing at all while the program runs.
 *
 * WHAT IT BUYS.  Telling files apart when there is no linker map and no symbols
 * -- where the whole report used to say "sin clasificar".  With a map the map
 * answers first and exactly; this is the floor, not the ceiling, and the report
 * marks it as `nearby` so an approximate name never reads as a measured one.
 *
 * WHEN IT DOES NOT WORK, AND IT IS NOT A DETAIL.  With `-ffunction-sections`
 * every function is its own section and the linker groups sections BY NAME.
 * Every anchor here has the same name, so they all end up one after another in
 * a single block -- measured, sixteen bytes apart -- and none of them sits
 * anywhere near the code of its own file.  A translation unit then has no
 * contiguous region for an anchor to mark, "the nearest anchor below" means
 * nothing, and the whole report ends up under one filename: measured, 414
 * anchors and 1.091 frames all attributed to the same file.
 *
 * So this is for builds WITHOUT function sections, where a unit's code is
 * contiguous.  With them, the linker map is the only thing that answers -- and
 * it answers better: exactly, per object, with no neighbourhoods.
 *
 * \~spanish
 * CUANDO NO FUNCIONA, Y NO ES UN DETALLE.  Con `-ffunction-sections` cada
 * funcion va a su propia seccion y el enlazador las agrupa POR NOMBRE.  Todas
 * las anclas de aqui se llaman igual, asi que acaban una detras de otra en un
 * solo bloque -- medido, a dieciseis bytes -- y ninguna queda cerca del codigo
 * de su propio fichero.  Entonces una unidad de compilacion no tiene region
 * contigua que un ancla pueda marcar, "el ancla anterior mas cercana" no
 * significa nada, y el informe entero sale bajo un solo nombre de fichero:
 * medido, 414 anclas y 1.091 marcos atribuidos todos al mismo.
 *
 * Asi que esto es para construcciones SIN secciones por funcion, donde el
 * codigo de una unidad si es contiguo.  Con ellas, el mapa del enlazador es lo
 * unico que contesta -- y contesta mejor: exacto, por objeto y sin vecindades.
 *
 * \~spanish
 * NO SE INCLUYE A MANO.  El build la mete en cada unidad de compilacion
 * (`vesta_alloc_declare_files(<objetivo>)`, que anade `-include`), y de eso se
 * trata: una convencion escrita a mano en setecientos ficheros es una
 * convencion que se olvida en el setecientos uno -- y un fichero que se olvida
 * no es que falte del informe.  Sus reservas caen en el marcador del fichero
 * ANTERIOR y salen con el nombre de aquel, que es peor que no contestar.
 *
 * QUE CUESTA.  Una funcion ancla que no hace nada, un objeto estatico y una
 * llamada al arrancar por unidad; la llamada anade a una tabla fija y no
 * reserva.  Nada mientras el programa corre.
 *
 * QUE APORTA.  Distinguir ficheros cuando no hay mapa del enlazador ni
 * simbolos -- donde el informe entero decia "sin clasificar".  Con mapa
 * contesta el mapa, antes y exacto; esto es el suelo, no el techo, y el informe
 * lo marca como `nearby` para que un nombre aproximado no se lea como uno
 * medido.
 * \~
 */

#ifndef VESTA_UTIL_ALLOC_DECLARE_TU_H
#define VESTA_UTIL_ALLOC_DECLARE_TU_H

#include "util/report/alloc_csv_c.h"

VESTA_ALLOC_FILE_TU();

#endif // VESTA_UTIL_ALLOC_DECLARE_TU_H
