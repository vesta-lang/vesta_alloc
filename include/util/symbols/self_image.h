/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file util/symbols/self_image.h
 * @brief
 * \~english Reading sections of the binary that is RUNNING, off the disk.
 * \~spanish Leer secciones del binario que esta CORRIENDO, desde el disco.
 * \~
 *
 * \~english
 * WHAT FOR.  Several things need to look inside the executable itself: the
 * symbol table and the function ranges (`util/symbols/self_symbols.h`), and the
 * debug information (`util/symbols/self_dwarf.h`).  All of them need the same
 * thing before starting -- finding a section by its name -- and that work was
 * about to be written twice.
 *
 * OFF THE DISK AND NOT OUT OF MEMORY, which is the surprising part.  What the
 * loader puts in memory are the sections needed to RUN: the symbol table and
 * the debug ones do not get loaded, so they are not there.  The file does have
 * them, and the process knows which one is its own.
 *
 * AND IN STRETCHES, not whole.  This binary takes 179 MiB in Profile and its
 * debug sections are 120 of them; reading all of it to look at a 280 KiB table
 * would be paying a thousand times what gets used.  Here it is opened once and
 * whatever is asked for is read.
 *
 * RULE OF THIS MODULE: it does not throw and it does not fail loudly.  It is
 * the foundation of some diagnostics, and a diagnostic that cannot be given is
 * a diagnostic that is not given -- never a reason for the program to die.
 * What cannot be read comes out as an empty stretch.
 *
 * \~spanish
 * PARA QUE.  Varias cosas necesitan mirar dentro del propio ejecutable: la
 * tabla de simbolos y los tramos de funcion (`util/symbols/self_symbols.h`), y la
 * informacion de depuracion (`util/symbols/self_dwarf.h`).  Todas necesitan lo mismo
 * antes de empezar -- encontrar una seccion por su nombre -- y ese trabajo
 * estaba a punto de escribirse dos veces.
 *
 * DEL DISCO Y NO DE MEMORIA, que es la parte que sorprende.  Lo que el cargador
 * pone en memoria son las secciones que hacen falta para EJECUTAR: la tabla de
 * simbolos y las de depuracion no se cargan, asi que ahi no estan.  El fichero
 * si las tiene, y el proceso sabe cual es el suyo.
 *
 * Y POR TRAMOS, no entero.  Este binario ocupa 179 MiB en Profile y sus
 * secciones de depuracion son 120 de ellos; leerlo entero para mirar una tabla
 * de 280 KiB seria pagar mil veces lo que se usa.  Aqui se abre una vez y se
 * lee lo que se pida.
 *
 * REGLA DE ESTE MODULO: no lanza y no falla ruidosamente.  Es el cimiento de
 * unos diagnosticos, y un diagnostico que no se puede dar es un diagnostico que
 * no se da -- nunca un motivo para que el programa muera --.  Lo que no se
 * puede leer sale como un tramo vacio.
 *
 * \~
 */

#ifndef VESTA_UTIL_SELF_IMAGE_H
#define VESTA_UTIL_SELF_IMAGE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace util {

/**
 * @brief
 * \~english The path of the executable that is running.
 * \~spanish La ruta del ejecutable que esta corriendo.
 * \~
 * @return
 * \~english the path, or empty when the system does not give it.
 * \~spanish la ruta, o vacia si el sistema no la da.
 * \~
 */
std::string self_image_path();

/**
 * @brief
 * \~english The bytes of a section of the binary itself.
 * \~spanish Los bytes de una seccion del propio binario.
 * \~
 *
 * \~english
 * Empty does not mean "error": it means the binary does not carry it, which is
 * normal in a stripped build.  Whoever asks has to tell the two apart and say
 * so, instead of showing a blank column.
 *
 * @par Threads
 * Safe: it opens, reads and closes without touching anything shared.
 *
 * \~spanish
 * Vacio no significa "error": significa que el binario no la lleva, que es lo
 * normal en una construccion despojada.  Quien pregunte tiene que distinguir
 * las dos cosas y decirlo, en vez de ensenar una columna en blanco.
 *
 * @par Hilos
 * Segura: abre, lee y cierra sin tocar nada compartido.
 *
 * \~
 * @param name
 * \~english the name as it stands: `.debug_info`, `.pdata`, `.symtab`...
 * \~spanish el nombre tal cual: `.debug_info`, `.pdata`, `.symtab`...
 * \~
 * @return
 * \~english the bytes, or an EMPTY vector if that section is not there.
 * \~spanish los bytes, o un vector VACIO si esa seccion no esta.
 * \~
 *
 * \~english
 * @code
 *   const auto ar = util::self_section(".debug_aranges");
 *   if (ar.empty()) return;   // no debug information, and it IS said
 * @endcode
 *
 * \~spanish
 * @code
 *   const auto ar = util::self_section(".debug_aranges");
 *   if (ar.empty()) return;   // sin informacion de depuracion, y se DICE
 * @endcode
 *
 * \~
 */
std::vector<unsigned char> self_section(const char *name);

/**
 * @brief
 * \~english A stretch of a section, without bringing in the whole section.
 * \~spanish Un tramo de una seccion, sin traerse la seccion entera.
 * \~
 *
 * \~english
 * It is what makes looking at `.debug_info` practical: it is 81 MiB, but a
 * single compilation unit runs to about ten KiB and that is all it takes to
 * answer for one address.
 *
 * @par Threads
 * Safe, the same as @c self_section.
 *
 * \~spanish
 * Es lo que hace practicable mirar `.debug_info`: son 81 MiB, pero una unidad
 * de compilacion suelta ronda los diez KiB y es lo unico que hace falta para
 * contestar por una direccion.
 *
 * @par Hilos
 * Segura, igual que @c self_section.
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
 * @param bytes
 * \~english how many.
 * \~spanish cuantos.
 * \~
 * @return
 * \~english the bytes, or empty if it runs off the section or cannot be read.
 * \~spanish los bytes, o vacio si se sale de la seccion o no se puede leer.
 * \~
 */
std::vector<unsigned char> self_section_part(const char *name, uint64_t offset,
                                             uint64_t bytes);

/**
 * @brief
 * \~english An open section, to read it IN PIECES many times.
 * \~spanish Una seccion abierta, para leerla A TROZOS muchas veces.
 * \~
 *
 * \~english
 * WHY, given that @c self_section_part exists.  Because that one opens the
 * file, walks the section table, reads and closes, ON EVERY CALL -- and there
 * is a case that needs thousands: walking the headers of the sixteen thousand
 * compilation units this binary has, reading a couple of KiB from each.
 * Through the single-call route that is sixteen thousand opens.
 *
 * It opens on construction and closes on destruction; in between, every read is
 * a seek and a read.  It is not meant to be kept: it is used inside the work
 * that needs it and released.
 *
 * @par Threads
 * NOT thread safe: it has a cursor.  One per thread.
 *
 * \~spanish
 * POR QUE, teniendo ya @c self_section_part.  Porque aquella abre el fichero,
 * recorre la tabla de secciones, lee y cierra, EN CADA LLAMADA -- y hay un caso
 * que necesita miles: recorrer las cabeceras de las dieciseis mil unidades de
 * compilacion que tiene este binario, leyendo un par de KiB de cada una.  Por
 * el camino de una llamada suelta eso son dieciseis mil aperturas.
 *
 * Se abre al construir y se cierra al destruir; mientras tanto cada lectura es
 * un salto y una lectura.  No es para guardarla: se usa dentro del trabajo que
 * la necesita y se suelta.
 *
 * @par Hilos
 * NO es segura entre hilos: tiene un cursor.  Cada hilo, la suya.
 *
 * \~
 */
class SelfSection {
  public:
    /**
     * @brief
     * \~english Opens the binary and locates the section.
     * \~spanish Abre el binario y localiza la seccion.
     * \~
     * @param name
     * \~english the section's name.
     * \~spanish el nombre de la seccion.
     * \~
     */
    explicit SelfSection(const char *name);

    /**
     * @brief
     * \~english Closes the file.
     * \~spanish Cierra el fichero.
     * \~
     */
    ~SelfSection();

    SelfSection(const SelfSection &) = delete;
    SelfSection &operator=(const SelfSection &) = delete;

    /**
     * @brief
     * \~english Whether there is anything to read.
     * \~spanish Si hay algo que leer.
     * \~
     * @return
     * \~english false when the section is not there or the binary could not be
     *           opened.
     * \~spanish false si la seccion no esta o no se pudo abrir el binario.
     * \~
     */
    bool ok() const { return file_ != nullptr && size_ != 0; }

    /**
     * @brief
     * \~english How big the section is.
     * \~spanish Cuanto mide la seccion.
     * \~
     * @return
     * \~english its size in bytes.  Zero when it is not there.
     * \~spanish su tamano en bytes.  Cero si no esta.
     * \~
     */
    uint64_t size() const { return size_; }

    /**
     * @brief
     * \~english Reads @p bytes from @p offset of the section.
     * \~spanish Lee @p bytes desde @p offset de la seccion.
     * \~
     * @param offset
     * \~english from the section's start.
     * \~spanish desde el principio de la seccion.
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
     * \~english false if it runs off the section or the read fails; @p dst is
     *           then left untouched, because half a datum is worse than none.
     * \~spanish false si se sale de la seccion o la lectura falla; @p dst queda
     *           entonces sin tocar, porque medio dato es peor que ninguno.
     * \~
     */
    bool read(uint64_t offset, void *dst, uint64_t bytes) const;

  private:
    /// \~english a `FILE*`, without dragging `<cstdio>` into the header
    /// \~spanish un `FILE*`, sin arrastrar `<cstdio>` a la cabecera  \~
    void *file_ = nullptr;
    /// \~english where it starts in the file
    /// \~spanish donde empieza en el fichero  \~
    uint64_t base_ = 0;
    /// \~english how big it is, already trimmed to what is useful
    /// \~spanish cuanto mide, ya recortado a lo util  \~
    uint64_t size_ = 0;
};

/**
 * @brief
 * \~english The base the binary was LINKED for.
 * \~spanish La base para la que se ENLAZO el binario.
 * \~
 *
 * \~english
 * It is NOT where it is loaded.  The addresses the debug information keeps are
 * link-time ones, and the module is loaded somewhere else -- address space
 * layout randomisation changes it on every run -- so to look a run-time address
 * up in those tables it has to be converted first:
 *
 *     link_time = run_time - os_module_base() + self_image_link_base()
 *
 * And it is read FROM THE FILE on purpose.  On Windows the loader PATCHES that
 * header field in memory when relocating, so asking the loaded module returns
 * where it is, not what it was linked for: exactly the datum that is no use.
 * The file on disk keeps the original.
 *
 * @par Threads
 * Safe.
 *
 * \~spanish
 * NO es donde esta cargado.  Las direcciones que guarda la informacion de
 * depuracion son de enlace, y el modulo se carga en otro sitio -- la
 * disposicion aleatoria del espacio de direcciones lo cambia en cada corrida
 * --, asi que para buscar una direccion de ejecucion en esas tablas hay que
 * pasarla primero:
 *
 *     de_enlace = de_ejecucion - os_module_base() + self_image_link_base()
 *
 * Y se lee DEL FICHERO a proposito.  En Windows el cargador PARCHEA ese campo
 * de la cabecera en memoria al reubicar, asi que preguntarselo al modulo
 * cargado devuelve donde esta, no para donde se enlazo: justo el dato que no
 * sirve.  El fichero en disco conserva el original.
 *
 * @par Hilos
 * Segura.
 *
 * \~
 * @return
 * \~english the link base, or zero when it cannot be told.
 * \~spanish la base de enlace, o cero si no se sabe.
 * \~
 */
uint64_t self_image_link_base();

/**
 * @brief
 * \~english Where a section starts and how big it is, without reading its
 *          contents.
 * \~spanish Donde empieza y cuanto mide una seccion, sin leer su contenido.
 * \~
 *
 * \~english
 * @par Threads
 * Safe.
 *
 * \~spanish
 * @par Hilos
 * Segura.
 *
 * \~
 * @param name
 * \~english the section.
 * \~spanish la seccion.
 * \~
 * @param offset
 * \~english receives its displacement INSIDE THE FILE.
 * \~spanish recibe su desplazamiento DENTRO DEL FICHERO.
 * \~
 * @param bytes
 * \~english receives its useful size (the virtual one when it is smaller than
 *           the recorded one, because what is recorded gets rounded up and the
 *           padding is not its own).
 * \~spanish recibe su tamano util (el virtual cuando es menor que el grabado,
 *           porque lo grabado se redondea y el relleno no es suyo).
 * \~
 * @return
 * \~english false when it is not there.
 * \~spanish false si no esta.
 * \~
 */
bool self_section_range(const char *name, uint64_t *offset, uint64_t *bytes);

} // namespace util

#endif // VESTA_UTIL_SELF_IMAGE_H
