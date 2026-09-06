/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file util/alloc_tag.h
 * @brief Que se sabe de una reserva: cuanto va a vivir y si va a crecer.
 *
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
 * contrastar con lo que de verdad pasa.  El plan entero esta en
 * `doc/PLAN_RESERVAS.md`.
 */
#ifndef VESTA_UTIL_ALLOC_TAG_H
#define VESTA_UTIL_ALLOC_TAG_H

#include <cstdint>

namespace util {

/// Cuanto vive lo reservado.  El orden importa: va de menos a mas.
enum class AllocUse : uint8_t {
    Unknown = 0, ///< nadie lo dijo
    Instant,     ///< muere dentro de la operacion que lo pidio
    Medium,      ///< vive lo que dure una fase
    Long         ///< vive lo que dure la compilacion
};

/// Si lo reservado cambia de tamano a lo largo de su vida.
enum class AllocShape : uint8_t {
    Unknown = 0, ///< nadie lo dijo
    Fixed,       ///< se pide una vez y ya
    Growing      ///< un contenedor que se hara mayor y abandonara este buffer
};

/**
 * @brief Los dos ejes en un byte.
 *
 * Empaquetado en cuatro bits para que sirva DIRECTAMENTE de indice en la tabla
 * de contadores: asi contar por etiqueta es un incremento indexado y no hay que
 * descomponer nada en el camino caliente.  Sobran cuatro combinaciones de las
 * dieciseis; salen mas baratas que una multiplicacion por reserva.
 */
class AllocTag {
  public:
    constexpr AllocTag() noexcept = default;
    constexpr AllocTag(AllocUse u, AllocShape s) noexcept
        : raw_(static_cast<uint8_t>((static_cast<uint8_t>(u) << 2) |
                                    static_cast<uint8_t>(s))) {}

    /// Reconstruye una etiqueta desde su forma empaquetada.
    static constexpr AllocTag from_raw(uint8_t r) noexcept {
        return AllocTag(r, 0);
    }

    constexpr AllocUse use() const noexcept {
        return static_cast<AllocUse>((raw_ >> 2) & 0x3);
    }
    constexpr AllocShape shape() const noexcept {
        return static_cast<AllocShape>(raw_ & 0x3);
    }
    /// La forma empaquetada, que es lo que se guarda y lo que indexa.
    constexpr uint8_t raw() const noexcept { return raw_; }
    /// true si no dice nada; es lo que queda por migrar.
    constexpr bool unknown() const noexcept { return raw_ == 0; }

    /// Cuantos valores distintos puede tomar @c raw.  Tamano de las tablas.
    static constexpr uint32_t kSlots = 16;

  private:
    /// El segundo parametro solo desambigua del constructor de dos ejes.
    constexpr AllocTag(uint8_t r, int) noexcept : raw_(r) {}

    uint8_t raw_ = 0; ///< 0 == Unknown/Unknown, lo que da un POD a cero
};

static_assert(sizeof(AllocTag) == 1, "la etiqueta tiene que caber en un byte");

/// Nombre legible de una etiqueta, para los informes.  Nunca en camino caliente.
const char *alloc_tag_name(AllocTag t) noexcept;

} // namespace util

#endif // VESTA_UTIL_ALLOC_TAG_H
