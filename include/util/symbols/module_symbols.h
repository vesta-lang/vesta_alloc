/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file util/symbols/module_symbols.h
 * @brief
 * \~english Whose address is this, and what is it called THERE.
 * \~spanish De quien es esta direccion, y como se llama ALLI.
 * \~
 *
 * \~spanish
 * POR QUE EXISTE.  Todo lo demas de esta libreria lee la imagen PROPIA del
 * programa: `/proc/self/exe`, sus secciones, su DWARF.  Eso bastaba mientras
 * todas las reservas venian del programa -- y dejo de bastar en cuanto el
 * asignador empezo a servir a la biblioteca C desde dentro, donde la direccion
 * que pidio la memoria es de `libc.so` y no nuestra.
 *
 * LO QUE COSTABA NO TENERLO, que es la parte que merece la pena recordar: el
 * resolutor no decia "no lo se".  Se iba a su propia tabla de simbolos, no
 * encontraba nada en esa direccion, y devolvia el ultimo simbolo que tenia:
 * `_fini`.  Un nombre sencillamente falso, presentado con la misma seguridad
 * que uno cierto.  Un informe se lee como un hecho, y un nombre equivocado es
 * peor que uno que falta: el que falta hace una pregunta, el equivocado la
 * cierra.
 *
 * QUE CONTESTA.  Dos cosas, y estan separadas a proposito: de que modulo es una
 * direccion, y como se llama el simbolo mas cercano de dentro.  La primera es
 * la que convierte `_fini` en `libc.so.6 +0x9e64`, que ya es cierto y util; la
 * segunda lo convierte en `_IO_file_doallocate`, que es lo que de verdad quiere
 * quien lee un informe.
 *
 * QUE NO CONTESTA: fichero y linea de un modulo ajeno.  Eso necesita el DWARF
 * de un fichero que no es nuestro, y en una maquina normal la biblioteca C no
 * lo trae de todas formas -- los simbolos de depuracion son un paquete aparte
 * --.  Cuando el modulo ajeno SI es de los nuestros, eso merece la pena
 * tenerlo; queda apuntado como pendiente en vez de fingido.
 *
 * \~english
 * WHY THIS EXISTS.  Everything else in this library reads the program's OWN
 * image: `/proc/self/exe`, its own sections, its own DWARF.  That was enough
 * while every allocation came from the program -- and it stopped being enough
 * the moment the allocator started serving the C library from the inside, where
 * the address that asked for the memory belongs to `libc.so`, not to us.
 *
 * WHAT IT COST TO NOT HAVE IT, because this is the part worth remembering: the
 * resolver did not say "I do not know". It went to its own symbol table, found
 * nothing at that address, and returned the last symbol it had -- `_fini`. A
 * name that is simply false, presented with the same confidence as a true one.
 * A report is read as fact, and a wrong name is worse than a missing one: the
 * missing one asks a question, the wrong one closes it.
 *
 * WHAT IT ANSWERS.  Two things, and they are separate on purpose: which module
 * an address belongs to, and what the nearest symbol inside it is called.  The
 * first one is what turns `_fini` into `libc.so.6 +0x9e64`, which is already
 * true and useful; the second turns it into `_IO_file_doallocate`, which is
 * what somebody reading a report actually wants.
 *
 * WHAT IT DOES NOT ANSWER: file and line for a foreign module.  That needs the
 * DWARF of a file that is not ours, and on a normal machine the C library does
 * not ship it anyway -- the debug symbols are a separate package.  When the
 * foreign module IS one of ours, that is worth having; it is written down as
 * pending rather than pretended.
 *
 * \~
 *
 * \~english
 * @code
 *   VestaModuleInfo m;
 *   if (vesta_module_of(pc, &m)) {
 *       printf("%s +0x%zx", m.path != NULL ? m.path : "?", m.offset);
 *       if (m.symbol != NULL) printf("  %s+0x%zx", m.symbol, m.sym_offset);
 *   }
 * @endcode
 *
 * \~spanish
 * @code
 *   VestaModuleInfo m;
 *   if (vesta_module_of(pc, &m)) {
 *       printf("%s +0x%zx", m.path != NULL ? m.path : "?", m.offset);
 *       if (m.symbol != NULL) printf("  %s+0x%zx", m.symbol, m.sym_offset);
 *   }
 * @endcode
 *
 * \~
 */
#ifndef VESTA_UTIL_MODULE_SYMBOLS_H
#define VESTA_UTIL_MODULE_SYMBOLS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief
 * \~english Where an address lives, told by the system that loaded it.
 * \~spanish Donde vive una direccion, segun el sistema que la cargo.
 * \~
 *
 * \~english
 * The strings belong to the loader and stay valid while the module is loaded,
 * which for anything that allocated memory is the rest of the process.  They
 * are NOT to be freed.
 *
 * \~spanish
 * Las cadenas son del cargador y valen mientras el modulo este cargado, que
 * para cualquier cosa que haya reservado memoria es el resto del proceso.  NO
 * hay que liberarlas.
 *
 * \~
 */
typedef struct VestaModuleInfo {
    /// \~english The path of the module that contains the address.  NULL if
    ///           unknown.
    /// \~spanish La ruta del modulo que contiene la direccion.  NULL si no se
    ///           sabe.
    /// \~
    const char *path;
    /// \~english Where that module is loaded.  The address minus this is
    ///           @c offset.
    /// \~spanish Donde esta cargado ese modulo.  La direccion menos esto es
    ///           @c offset.
    /// \~
    const void *base;
    /// \~english The address, as a displacement inside the module.  This is
    ///           what stays the same between runs; the address itself moves
    ///           with every load.
    /// \~spanish La direccion, como desplazamiento dentro del modulo.  Es lo
    ///           que se mantiene igual entre corridas; la direccion en si se
    ///           mueve en cada carga.
    /// \~
    size_t offset;
    /// \~english The nearest symbol at or before the address, or NULL if the
    ///           module has none to offer -- which is normal for a stripped
    ///           one.
    /// \~spanish El simbolo mas cercano en la direccion o antes de ella, o NULL
    ///           si el modulo no tiene ninguno que ofrecer -- que es lo normal
    ///           en uno despojado.
    /// \~
    const char *symbol;
    /// \~english How far past that symbol the address is.  Zero means it IS the
    ///           entry.
    /// \~spanish Cuanto mas alla de ese simbolo esta la direccion.  Cero
    ///           significa que ES la entrada.
    /// \~
    size_t sym_offset;
} VestaModuleInfo;

/**
 * @brief
 * \~english Fills @p out for the module that owns @p pc.
 * \~spanish Rellena @p out con el modulo dueño de @p pc.
 * \~
 *
 * \~english
 * @par Threads
 * Safe from any thread.  It asks the dynamic loader, which is prepared for it.
 *
 * \~spanish
 * @par Hilos
 * Segura desde cualquier hilo.  Pregunta al cargador dinamico, que esta
 * preparado para ello.
 *
 * \~
 * @param pc
 * \~english the address to place.
 * \~spanish la direccion que hay que situar.
 * \~
 * @param out
 * \~english where the answer goes; it is only touched when 1 is returned.
 * \~spanish donde va la respuesta; solo se toca cuando se devuelve 1.
 * \~
 * @return
 * \~english 1 if the owner was found, 0 if not -- and 0 is a real answer, not a
 *           failure: an address can belong to memory no module claims (a JIT
 *           page, a mapping made by hand).  Saying so is the point.
 * \~spanish 1 si se encontro al dueño, 0 si no -- y 0 es una respuesta de
 *           verdad, no un fallo: una direccion puede ser de memoria que no
 *           reclama ningun modulo (una pagina del JIT, un mapeo hecho a mano).
 *           Decirlo es justo lo que se busca.
 * \~
 */
int vesta_module_of(const void *pc, VestaModuleInfo *out);

/**
 * @brief
 * \~english Whether @p pc belongs to the program's own image.
 * \~spanish Si @p pc es de la imagen propia del programa.
 * \~
 *
 * \~english
 * THE ONE THAT WAS MISSING.  Every other resolver in this library assumes the
 * address is the program's, and none of them checked -- so a foreign address
 * came back wearing a name from our symbol table.  Asking this first is what
 * separates "I can resolve this" from "this is not mine to resolve".
 *
 * \~spanish
 * LA QUE FALTABA.  Todos los demas resolutores de esta libreria dan por hecho
 * que la direccion es del programa, y ninguno lo comprobaba -- asi que una
 * direccion ajena volvia con un nombre de NUESTRA tabla de simbolos puesto --.
 * Preguntar esto primero es lo que separa "esto lo se resolver" de "esto no me
 * toca a mi resolverlo".
 *
 * \~
 * @param pc
 * \~english the address to ask about.
 * \~spanish la direccion por la que se pregunta.
 * \~
 * @return
 * \~english 1 if it is ours, 0 if it is not or cannot be told.
 * \~spanish 1 si es nuestra, 0 si no lo es o no se puede saber.
 * \~
 */
int vesta_module_is_self(const void *pc);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // VESTA_UTIL_MODULE_SYMBOLS_H
