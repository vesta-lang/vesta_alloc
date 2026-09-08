/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/alloc/alloc_tag.h
 * @brief
 * \~english What is known about an allocation: how long it will live, and
 *          whether it will grow.
 * \~spanish Que se sabe de una reserva: cuanto va a vivir y si va a crecer.
 * \~
 *
 * \~english
 * WHY IT IS NEEDED.  An allocation can be served about five times faster than
 * today -- a bump arena runs at 1.95 ns against the general allocator's 10.03
 * -- but only for one particular kind: the one that dies soon and does not
 * change size.  And today there is no way to tell which is which, because 81%
 * of them arrive through `operator new`, which only ever sees a byte count.
 *
 * Applying it blindly has been tried and measured: putting range analysis into
 * an arena took memory from 2,414 to 5,455 MB (+126%) in exchange for 4% speed,
 * because a container that grows ABANDONS its previous buffer and an arena
 * never takes it back.  The two axes come out of that failure.
 *
 * WHY TWO AXES AND NOT ONE.  They govern different decisions: the USE says when
 * it dies, the SHAPE says whether the buffer moves.  An arena wins at
 * (Instant, Fixed) and loses at anything Growing, so with a single axis those
 * two cases cannot be told apart and the optimisation cannot be applied safely.
 *
 * WHY ZERO MEANS "UNKNOWN".  Because that way the default costs nothing: the
 * structure the tag lives in is a POD that is zeroed without running any code.
 * And because it is HONEST -- a default that appears to know something does not
 * fail, it lies, and that is not noticed until much later.
 *
 * IT IS NOT A SOURCE OF TRUTH.  What is written here is a CLAIM.  The
 * diagnostic mode measures how long each allocation really lived and its size
 * split, so the tag can be checked against what actually happens.
 *
 * \~spanish
 * POR QUE HACE FALTA.  Sabemos servir una reserva cinco veces mas rapido que
 * hoy -- una arena de golpe va a 1,95 ns frente a los 10,03 del asignador
 * general --, pero solo para una clase concreta de reserva: la que muere pronto
 * y no cambia de tamano.  Y hoy no hay forma de saber cual es cual, porque el
 * 81% llega por `operator new`, que solo ve un numero de bytes.
 *
 * Aplicarlo a ciegas ya se probo y esta medido: meter el analisis de rangos en
 * una arena subio la memoria de 2.414 a 5.455 MB (+126%) a cambio de un 4% de
 * velocidad, porque un contenedor que crece ABANDONA su buffer anterior y en
 * una arena eso no vuelve.  De ese fracaso salen los dos ejes.
 *
 * POR QUE DOS EJES Y NO UNO.  Gobiernan decisiones distintas: el USO dice
 * cuando muere y la FORMA dice si el buffer se muda.  La arena gana en
 * (Instant, Fixed) y pierde en cualquier cosa Growing, asi que con un solo eje
 * los dos casos no se pueden separar y la optimizacion no se puede aplicar sin
 * riesgo.
 *
 * POR QUE EL CERO ES "NO SE".  Porque asi el valor por defecto sale gratis: la
 * estructura donde vive la etiqueta es un POD que se inicializa a cero sin
 * ejecutar codigo.  Y porque es lo HONESTO -- un valor por defecto que parece
 * saber algo no falla, miente, y eso no se detecta hasta mucho despues.
 *
 * NO ES UNA FUENTE DE VERDAD.  Lo que se escribe aqui es una AFIRMACION, igual
 * que `@complexity` lo es para el coste.  El modo de diagnostico mide la vida
 * real de cada reserva y su reparto de tamanos, asi que la etiqueta se puede
 * contrastar con lo que de verdad pasa.
 *
 * \~
 */
#ifndef VESTA_UTIL_ALLOC_TAG_H
#define VESTA_UTIL_ALLOC_TAG_H

/* Los VALORES viven en la cabecera de C, y aqui solo se les pone nombre
 * fuerte: asi los dos lenguajes no pueden separarse.  Ver `alloc_tag_c.h`. */
#include "util/alloc/alloc_tag_c.h"

#include <cstdint>

namespace util {

/**
 * @brief
 * \~english How long the allocation lives.  The order matters: shortest first.
 * \~spanish Cuanto vive lo reservado.  El orden importa: va de menos a mas.
 * \~
 */
enum class AllocUse : uint8_t {
    /// \~english nobody said  \~spanish nadie lo dijo  \~
    Unknown = VestaUse_Unknown,
    /// \~english dies inside the operation that asked for it
    /// \~spanish muere dentro de la operacion que lo pidio  \~
    Instant = VestaUse_Instant,
    /// \~english lives as long as one phase  \~spanish vive lo que dure una fase  \~
    Medium = VestaUse_Medium,
    /// \~english lives as long as the process  \~spanish vive lo que dure el proceso  \~
    Long = VestaUse_Long
};

/**
 * @brief
 * \~english Whether the allocation changes size during its life.
 * \~spanish Si lo reservado cambia de tamano a lo largo de su vida.
 * \~
 */
enum class AllocShape : uint8_t {
    /// \~english nobody said  \~spanish nadie lo dijo  \~
    Unknown = VestaShape_Unknown,
    /// \~english asked for once and that is it  \~spanish se pide una vez y ya  \~
    Fixed = VestaShape_Fixed,
    /// \~english a container that will grow and abandon this buffer
    /// \~spanish un contenedor que se hara mayor y abandonara este buffer  \~
    Growing = VestaShape_Growing
};

/**
 * @brief
 * \~english How much of the block the caller is going to touch.
 * \~spanish Cuanto del bloque va a tocar el llamante.
 * \~
 *
 * \~english
 * A THIRD AXIS, AND APART FROM THE OTHER TWO ON PURPOSE.  It does not go into
 * @c AllocTag: those two are packed into four bits so the byte can index the
 * counter table directly, and a third axis would take that table from 16 slots
 * to 64 -- 384 KiB more of static memory across every cache, and a change to
 * the layout of the structure the fast path lives in, which has already cost a
 * measurement once.  It is counted on its own instead.
 *
 * WHAT IT DECIDES, and it is the one thing the allocator cannot work out for
 * itself.  Handing back a big zeroed block has two ways to be right and they
 * are opposites: keep the block and CLEAR it, whose cost is flat in what gets
 * read because the whole thing is written; or ask the system for a fresh one,
 * which arrives already zero and costs a page fault for every page the caller
 * actually touches.  Measured across sizes and fractions, they cross at a
 * FRACTION -- somewhere between 1/16 and 1/4 -- and NOT at a size: the same
 * 8 MiB request wins one way read sparsely and the other way read whole.  So no
 * threshold on size can choose, and the only one who knows is the caller.
 *
 * Zero is "nobody said", and it keeps today's behaviour: a default that looks
 * like knowledge does not fail, it lies.
 *
 * \~spanish
 * UN TERCER EJE, Y APARTE DE LOS OTROS DOS A PROPOSITO.  No entra en
 * @c AllocTag: esos dos van empaquetados en cuatro bits para que el byte indexe
 * directamente la tabla de contadores, y un tercer eje la llevaria de 16 a 64
 * ranuras -- 384 KiB mas de memoria estatica entre todos los caches, y un
 * cambio en el layout de la estructura donde vive el camino rapido, que ya
 * costo una medicion una vez --.  Se cuenta por separado.
 *
 * QUE DECIDE, que es lo unico que el asignador no puede averiguar solo.
 * Entregar un bloque grande a cero tiene dos formas de estar bien y son
 * opuestas: quedarse el bloque y LIMPIARLO, cuyo coste es plano en lo que se
 * lea porque se escribe entero; o pedirle uno fresco al sistema, que llega ya a
 * cero y cuesta un fallo de pagina por cada pagina que el llamante toque de
 * verdad.  Medido a lo largo de tamanos y fracciones, se cruzan en una
 * FRACCION -- entre 1/16 y 1/4 -- y NO en un tamano: la misma peticion de
 * 8 MiB gana de una forma leida a trozos y de la otra leida entera.  Asi que
 * ningun umbral por tamano puede elegir, y el unico que lo sabe es quien llama.
 *
 * El cero es "nadie lo dijo", y deja el comportamiento de hoy: un valor por
 * defecto que parece saber no falla, miente.
 *
 * \~
 */
enum class AllocFill : uint8_t {
    /// \~english nobody said  \~spanish nadie lo dijo  \~
    Unknown = VestaFill_Unknown,
    /// \~english a small part of it gets touched
    /// \~spanish se toca una parte pequena  \~
    Sparse = VestaFill_Sparse,
    /// \~english a good part of it  \~spanish buena parte de el  \~
    Dense = VestaFill_Dense,
    /// \~english all of it  \~spanish entero  \~
    All = VestaFill_All
};

/// \~english How many values @c AllocFill takes.  The size of its counters.
/// \~spanish Cuantos valores toma @c AllocFill.  Tamano de sus contadores.
/// \~
inline constexpr uint32_t kAllocFillSlots = VESTA_ALLOC_FILL_SLOTS;

/**
 * @brief
 * \~english The two axes in one byte.
 * \~spanish Los dos ejes en un byte.
 * \~
 *
 * \~english
 * Packed into four bits so that it can serve DIRECTLY as an index into the
 * counter table: counting by tag is then one indexed increment, with nothing to
 * take apart on the hot path.  Four of the sixteen combinations go unused; they
 * are cheaper than a multiplication per allocation.
 *
 * \~spanish
 * Empaquetado en cuatro bits para que sirva DIRECTAMENTE de indice en la tabla
 * de contadores: asi contar por etiqueta es un incremento indexado y no hay que
 * descomponer nada en el camino caliente.  Sobran cuatro combinaciones de las
 * dieciseis; salen mas baratas que una multiplicacion por reserva.
 *
 * \~
 * @code
 * util::AllocTag t{util::AllocUse::Instant, util::AllocShape::Fixed};
 * counters[t.raw()]++;
 * @endcode
 */
class AllocTag {
  public:
    constexpr AllocTag() noexcept = default;
    constexpr AllocTag(AllocUse u, AllocShape s) noexcept
        : raw_(static_cast<uint8_t>((static_cast<uint8_t>(u) << 2) |
                                    static_cast<uint8_t>(s))) {}

    /**
     * @brief
     * \~english Rebuilds a tag from its packed form.
     * \~spanish Reconstruye una etiqueta desde su forma empaquetada.
     * \~
     * @param r
     * \~english the packed byte, as @c raw returns it.
     * \~spanish el byte empaquetado, tal como lo devuelve @c raw.
     * \~
     * @return
     * \~english the tag it stands for.
     * \~spanish la etiqueta que representa.
     * \~
     */
    static constexpr AllocTag from_raw(uint8_t r) noexcept {
        return AllocTag(r, 0);
    }

    constexpr AllocUse use() const noexcept {
        return static_cast<AllocUse>((raw_ >> 2) & 0x3);
    }
    constexpr AllocShape shape() const noexcept {
        return static_cast<AllocShape>(raw_ & 0x3);
    }
    /**
     * @brief
     * \~english The packed form: what gets stored, and what indexes the tables.
     * \~spanish La forma empaquetada, que es lo que se guarda y lo que indexa.
     * \~
     * @return
     * \~english the byte, in [0, @c kSlots).
     * \~spanish el byte, en [0, @c kSlots).
     * \~
     */
    constexpr uint8_t raw() const noexcept { return raw_; }

    /**
     * @brief
     * \~english Whether the tag says nothing at all.
     * \~spanish Si la etiqueta no dice nada.
     * \~
     * @return
     * \~english true for the default tag, which is what is still unclassified.
     * \~spanish true para la etiqueta por defecto, que es lo que queda sin
     *           clasificar.
     * \~
     */
    constexpr bool unknown() const noexcept { return raw_ == 0; }

    /// \~english How many distinct values @c raw can take.  The table size.
    /// \~spanish Cuantos valores distintos puede tomar @c raw.  Tamano de las
    /// tablas.
    /// \~
    static constexpr uint32_t kSlots = 16;

  private:
    /// \~english The second parameter only disambiguates from the two-axis
    ///           constructor.
    /// \~spanish El segundo parametro solo desambigua del constructor de dos
    ///           ejes.
    /// \~
    constexpr AllocTag(uint8_t r, int) noexcept : raw_(r) {}

    /// \~english 0 == Unknown/Unknown, which is what a zeroed POD gives
    /// \~spanish 0 == Unknown/Unknown, lo que da un POD a cero  \~
    uint8_t raw_ = 0;
};

static_assert(sizeof(AllocTag) == 1, "la etiqueta tiene que caber en un byte");

/**
 * @brief
 * \~english A readable name for a tag, for reports.
 * \~spanish Nombre legible de una etiqueta, para los informes.
 * \~
 *
 * \~english Never on a hot path.  \~spanish Nunca en camino caliente.  \~
 *
 * @param t
 * \~english the tag to name.
 * \~spanish la etiqueta a nombrar.
 * \~
 * @return
 * \~english a static string, never null; "unknown" for the default tag.
 * \~spanish una cadena estatica, nunca nula; "unknown" para la etiqueta por
 *           defecto.
 * \~
 *
 * @code
 * std::printf("%s\n", util::alloc_tag_name(site.tag));
 * @endcode
 */
const char *alloc_tag_name(AllocTag t) noexcept;

} // namespace util

#endif // VESTA_UTIL_ALLOC_TAG_H
