/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file util/alloc/size_buckets.h
 * @brief
 * \~english How requested sizes are split into buckets.  ONE definition.
 * \~spanish Como se reparten en tramos los tamanos pedidos.  UNA definicion.
 * \~
 *
 * \~english
 * WHY IT HAS ITS OWN HEADER.  Because it was written down twice -- once where
 * the global histogram is filled and once where it is exported -- and the two
 * copies had ALREADY drifted: one ended in `~size_t(0)` and the other in `0`.
 * That particular pair happened to be harmless because the exporter means "no
 * bound" by zero, but nothing said so, and the next copy is the one that
 * shifts a boundary and makes two histograms of the same run disagree without
 * either of them looking wrong.
 *
 * WHY THESE BOUNDS.  They answer one question: does what is being asked for
 * fall outside the size classes the allocator serves?  If it does, raising the
 * ceiling gives back more than it costs; if it does not, it does not.  The
 * upper half is deliberately spread out instead of leaving every large
 * allocation in one bin: with a single bin there is no way to decide HOW to
 * serve them -- tens of KiB call for classes by page, megabytes call for
 * mapping each block on its own, and those are different designs.
 *
 * \~spanish
 * POR QUE TIENE CABECERA PROPIA.  Porque estaba escrito dos veces -- una donde
 * se rellena el histograma global y otra donde se exporta -- y las dos copias
 * YA habian divergido: una acababa en `~size_t(0)` y la otra en `0`.  Esa
 * pareja en concreto resulto ser inofensiva porque el exportador entiende el
 * cero como "sin tope", pero eso no lo decia nadie, y la copia siguiente es la
 * que mueve una frontera y hace que dos histogramas de la misma ejecucion no
 * coincidan sin que ninguno parezca equivocado.
 *
 * POR QUE ESTAS FRONTERAS.  Contestan a una pregunta: se sale de las clases de
 * tamano que sirve el asignador lo que se esta pidiendo?  Si se sale, subir
 * el techo devuelve mas de lo que cuesta; si no, no.  La mitad de arriba esta
 * repartida a proposito en vez de dejar todas las reservas grandes en un solo
 * tramo: con uno solo no hay forma de decidir COMO servirlas -- decenas de KiB
 * piden clases por pagina, los megabytes piden mapear cada bloque por su
 * cuenta, y esos son disenos distintos.
 *
 * \~
 */
#ifndef VESTA_UTIL_SIZE_BUCKETS_H
#define VESTA_UTIL_SIZE_BUCKETS_H

#include <cstddef>
#include <cstdint>

namespace util {

/// \~english The upper bound of each bucket, in bytes.  The last one is
///           unbounded.
/// \~spanish El tope de cada tramo, en bytes.  El ultimo no tiene tope.
/// \~
inline constexpr size_t kBucketLimit[] = {
    64,     256,     1024,      2048,     // lo que servimos por clases
    4096,   8192,    16384,     65536,    // paginas y trozos
    262144, 1048576, 16777216,  ~size_t(0)};

/// \~english How many buckets there are; the size of every histogram.
/// \~spanish Cuantos tramos hay; el tamano de todos los histogramas.
/// \~
inline constexpr uint32_t kSizeBuckets =
    sizeof(kBucketLimit) / sizeof(kBucketLimit[0]);

/**
 * @brief
 * \~english The bucket @p n falls in.
 * \~spanish El tramo en el que cae @p n.
 * \~
 *
 * \~english
 * A linear walk over twelve constants, which the compiler unrolls into a
 * handful of compares.  It is not worth being cleverer: this runs only when
 * measuring, and a table lookup would need a shift-and-index that gets the
 * boundaries wrong the moment somebody changes one of them.
 *
 * \~spanish
 * Un recorrido lineal sobre doce constantes, que el compilador desenrolla en
 * un punado de comparaciones.  No merece la pena ser mas listo: esto solo
 * corre al medir, y una tabla exigiria un desplazamiento e indice que se
 * equivoca de frontera en cuanto alguien cambia una.
 *
 * \~
 * @param n
 * \~english the size asked for, in bytes.
 * \~spanish el tamano pedido, en bytes.
 * \~
 * @return
 * \~english the bucket index, in [0, @c kSizeBuckets).
 * \~spanish el indice del tramo, en [0, @c kSizeBuckets).
 * \~
 *
 * \~english
 * @code
 *   hist[util::bucket_of(n)]++;      // counting by size costs one increment
 * @endcode
 *
 * \~spanish
 * @code
 *   hist[util::bucket_of(n)]++;      // contar por tamano cuesta un incremento
 * @endcode
 *
 * \~
 */
[[gnu::always_inline]] inline uint32_t bucket_of(size_t n) noexcept {
    uint32_t b = 0;
    while (b + 1 < kSizeBuckets && n > kBucketLimit[b]) ++b;
    return b;
}

/**
 * @brief
 * \~english The bound as an EXPORT writes it: zero for the unbounded one.
 * \~spanish El tope tal como lo escribe una EXPORTACION: cero para el que no
 *          tiene.
 * \~
 *
 * \~english
 * A tool reading `0` can tell it apart from a real limit; writing
 * `18446744073709551615` would look like a boundary somebody chose.
 *
 * \~spanish
 * Una herramienta que lee `0` lo distingue de un limite de verdad; escribir
 * `18446744073709551615` pareceria una frontera que alguien eligio.
 *
 * \~
 * @param b
 * \~english the bucket index, in [0, @c kSizeBuckets).
 * \~spanish el indice del tramo, en [0, @c kSizeBuckets).
 * \~
 * @return
 * \~english its upper bound in bytes, or zero for the last one.
 * \~spanish su tope en bytes, o cero para el ultimo.
 * \~
 *
 * \~english
 * @code
 *   for (uint32_t b = 0; b < util::kSizeBuckets; ++b)
 *       fprintf(f, "%llu,%llu\n", util::bucket_limit_out(b), hist[b]);
 * @endcode
 *
 * \~spanish
 * @code
 *   for (uint32_t b = 0; b < util::kSizeBuckets; ++b)
 *       fprintf(f, "%llu,%llu\n", util::bucket_limit_out(b), hist[b]);
 * @endcode
 *
 * \~
 */
inline constexpr unsigned long long bucket_limit_out(uint32_t b) noexcept {
    return kBucketLimit[b] == ~size_t(0)
               ? 0ull
               : (unsigned long long)kBucketLimit[b];
}

} // namespace util

#endif // VESTA_UTIL_SIZE_BUCKETS_H
