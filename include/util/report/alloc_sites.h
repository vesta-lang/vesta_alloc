/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/report/alloc_sites.h
 * @brief
 * \~english WHERE each allocation that arrives without declaring its purpose
 *          comes from.
 * \~spanish De DONDE sale cada reserva que llega sin declarar su proposito.
 * \~
 *
 * \~english
 * WHAT FOR.  The allocator already counts by purpose, but today 100% arrives as
 * "unknown": nobody declares anything yet.  A percentage does not say where to
 * start; what is needed is the LIST of the sites that allocate most, ordered by
 * volume, and that list is what says which code is worth tagging.
 *
 * The cost is proportional to what is MISSING: only what arrives untagged gets
 * noted down, so as sites get declared, this switches itself off.
 *
 * TWO WAYS TO IDENTIFY A SITE, and both are needed:
 *
 *  - **By TYPE** (@c TypeName).  A container allocation identifies itself --
 *    `std::vector<IrInstr>` -- with no symbols to resolve and no dependence on
 *    where the module was loaded.
 *  - **By RETURN ADDRESS**, for the remainder: what does not go through a typed
 *    container.  It is captured in `operator new` and NOT in `host_alloc`: from
 *    `host_alloc` the caller is always `operator new` and the datum is good for
 *    nothing.
 *
 * WHAT GETS DUMPED.  The module base and the OFFSET, never the absolute
 * address: with ASLR one run's dump cannot be read with another run's binary.
 * The symbols are resolved outside, with `addr2line` over the Profile binary --
 * the Release one does not unwind, it carries `-fomit-frame-pointer`.
 *
 * \~spanish
 * PARA QUE.  El asignador ya cuenta por proposito, pero hoy el 100% llega como
 * "no se": nadie declara nada todavia.  Un porcentaje no dice por donde
 * empezar; hace falta la LISTA de los sitios que mas reservan, ordenada por
 * volumen, y esa lista es lo que dice a que codigo merece la pena ponerle una
 * etiqueta.
 *
 * El coste es proporcional a lo que FALTA: solo se apunta lo que llega sin
 * etiqueta, asi que segun se van declarando sitios, esto se va apagando solo.
 *
 * DOS FORMAS DE IDENTIFICAR UN SITIO, y las dos hacen falta:
 *
 *  - **Por TIPO** (@c TypeName).  Una reserva de contenedor se identifica sola
 *    -- `std::vector<IrInstr>` --, sin resolver simbolos y sin depender de
 *    donde se cargo el modulo.
 *  - **Por DIRECCION de retorno**, para el residuo: lo que no pasa por un
 *    contenedor tipado.  Se captura en `operator new` y NO en `host_alloc`:
 *    desde `host_alloc` el llamante es siempre `operator new` y el dato no
 *    vale para nada.
 *
 * QUE SE VUELCA.  La base del modulo y el DESPLAZAMIENTO, nunca la direccion
 * absoluta: con ASLR el volcado de una corrida no se puede leer con el binario
 * de otra.  Los simbolos se resuelven fuera, con `addr2line` sobre el binario
 * de Profile -- el de Release no desenrolla, lleva `-fomit-frame-pointer`.
 *
 * \~
 */
#ifndef VESTA_UTIL_ALLOC_SITES_H
#define VESTA_UTIL_ALLOC_SITES_H

/* La ESTRUCTURA de un sitio se declara en la cabecera de C y aqui solo se
 * nombra otra vez: declararla dos veces, una por lenguaje, es la duplicacion
 * que nadie ve romperse.  Ver los `static_assert` de mas abajo. */
#include "util/report/alloc_sites_c.h"
#include "util/alloc/alloc_tag.h"
#include "util/alloc/size_buckets.h"

#include <cstddef>
#include <cstdint>

namespace util {

/**
 * @brief
 * \~english A type's identity, with no registry and no counters.
 * \~spanish La identidad de un tipo, sin registro y sin contadores.
 * \~
 *
 * \~english
 * Its ADDRESS is the identifier -- unique per `T`, with no races and nothing to
 * initialise -- and its CONTENT is the readable name.  Both things in one
 * piece, which is what avoids having to maintain a table of names.
 *
 * `__PRETTY_FUNCTION__` carries the type inside the signature's text, so the
 * dump comes out readable without resolving any symbol.
 *
 * \~spanish
 * Su DIRECCION es el identificador -- unica por `T`, sin carreras y sin nada
 * que inicializar -- y su CONTENIDO es el nombre legible.  Las dos cosas de una
 * sola pieza, que es lo que evita tener que mantener una tabla de nombres.
 *
 * `__PRETTY_FUNCTION__` trae el tipo dentro del texto de la firma, asi que el
 * volcado sale legible sin resolver ningun simbolo.
 *
 * \~
 * @tparam T
 * \~english the type being identified.
 * \~spanish el tipo que se identifica.
 * \~
 *
 * \~english
 * @code
 *   util::record_alloc_site(
 *       (const void *)&util::TypeName<std::vector<int>>::get, n, tag);
 * @endcode
 *
 * \~spanish
 * @code
 *   util::record_alloc_site(
 *       (const void *)&util::TypeName<std::vector<int>>::get, n, tag);
 * @endcode
 *
 * \~
 */
template <class T> struct TypeName {
    /**
     * @brief
     * \~english The readable name, and an address unique to @c T.
     * \~spanish El nombre legible, y una direccion unica de @c T.
     * \~
     * @return
     * \~english a static string with the type inside it.
     * \~spanish una cadena estatica con el tipo dentro.
     * \~
     */
    static const char *get() noexcept {
#if defined(__GNUC__) || defined(__clang__)
        return __PRETTY_FUNCTION__;
#elif defined(_MSC_VER)
        return __FUNCSIG__;
#else
        return "?";
#endif
    }
};

/**
 * @brief
 * \~english Notes down an allocation made from @p pc with purpose @p tag.
 * \~spanish Apunta una reserva hecha desde @p pc con el proposito @p tag.
 * \~
 *
 * \~english
 * It ONLY counts when the size split is on (`VESTA_HOST_ALLOC_STATS=1`).
 * CAPTURING the address is free and can stay in forever -- it is measured --
 * but noting it down in a table is a measurement, and a measurement nobody
 * asked for is not paid for.
 *
 * IT NOTES NOTHING DOWN if the thread does not have a cache of its own yet,
 * because the table is kept per thread and hangs off it.  From `operator new`
 * that cannot happen -- it is called right AFTER allocating, so the cache has
 * just been born -- but whoever calls it by hand must have allocated first.
 *
 * \~spanish
 * SOLO cuenta cuando el reparto de tamanos esta encendido
 * (`VESTA_HOST_ALLOC_STATS=1`).  La CAPTURA de la direccion es gratis y puede
 * quedarse siempre -- esta medido --, pero apuntarla en una tabla es una
 * medida, y una medida que no se ha pedido no se paga.
 *
 * NO APUNTA NADA si el hilo todavia no tiene cache propio, porque la tabla se
 * lleva por hilo y cuelga de el.  Desde `operator new` eso no puede pasar --
 * se llama justo DESPUES de reservar, asi que el cache acaba de nacer --, pero
 * quien la llame a mano tiene que haber reservado antes.
 *
 * \~
 * @param pc
 * \~english the return address of whoever allocated, or the address of a
 *           @c TypeName<T>::get for a typed container allocation.
 * \~spanish direccion de retorno del que reservo, o la direccion de un
 *           @c TypeName<T>::get para una reserva de contenedor tipado.
 * \~
 * @param n
 * \~english the bytes asked for.
 * \~spanish bytes pedidos.
 * \~
 * @param tag
 * \~english the purpose running at that moment, packed.  Zero is "unknown",
 *           which is what is left to declare.
 * \~spanish el proposito que corria en ese momento, empaquetado.  Cero es "no
 *           se", que es lo que queda por declarar.
 * \~
 */
void record_alloc_site(const void *pc, size_t n, uint8_t tag) noexcept;

/**
 * @brief
 * \~english How many times an allocation found its window full and evicted the
 *          weakest entry.
 * \~spanish Cuantas veces una reserva encontro su ventana llena y desalojo a la
 *          entrada mas floja.
 * \~
 *
 * \~english
 * IT IS NOT LOST MEMORY: nothing stops being counted.  What it says is that
 * from there on the counts are UPPER BOUNDS, because a new site inherits the
 * count of the one it throws out.  It is kept and it is said because a list
 * pretending to be exact when it is not gets used to decide, and decides badly.
 *
 * \~spanish
 * NO ES MEMORIA PERDIDA: nada se deja de contar.  Lo que dice es que a partir
 * de ahi las cuentas son COTAS SUPERIORES, porque un sitio nuevo hereda la
 * cuenta del que echa.  Se lleva y se dice porque una lista que finge ser
 * exacta cuando no lo es se usa para decidir, y decide mal.
 *
 * \~
 * @return
 * \~english the number of evictions since the process started.
 * \~spanish cuantos desalojos van desde que arranco el proceso.
 * \~
 */
uint64_t alloc_sites_overflow() noexcept;

/**
 * @brief
 * \~english Requests to note something down that never reached the table.
 * \~spanish Peticiones de apuntar que no llegaron a la tabla.
 * \~
 *
 * \~english
 * It only happens with a thread that has no cache of its own -- above the owner
 * cap -- so it is normally zero.  It is the third figure needed to read the
 * report without guessing: every call is either dropped here or adds one to
 * some entry, so that `the table's sum + this` is the number of allocations
 * that went past the capture point.  If that does not match what the allocator
 * counts, the mismatch is in the COUNTER and not in the table, which is a
 * different question and is fixed somewhere else.
 *
 * There is no counter of total calls on purpose: it would sit on the path of
 * every allocation and a shared atomic there measures slower than what is
 * there.
 *
 * \~spanish
 * Solo ocurre con un hilo sin cache propio -- por encima del tope de dueños --,
 * asi que normalmente es cero.  Es la tercera cifra que hace falta para leer el
 * informe sin adivinar: toda llamada o se descarta aqui o sube en uno alguna
 * entrada, de modo que `suma de la tabla + esto` son las reservas que pasaron
 * por el sitio de captura.  Si eso no cuadra con lo que cuenta el asignador, el
 * descuadre esta en el CONTADOR y no en la tabla, que es una pregunta distinta
 * y se arregla en otro sitio.
 *
 * No hay contador de llamadas totales a proposito: iria en el camino de cada
 * reserva y un atomico compartido ahi mide mas despacio de lo que hay.
 *
 * \~
 * @return
 * \~english how many were dropped since the process started.
 * \~spanish cuantas se descartaron desde que arranco el proceso.
 * \~
 */
uint64_t alloc_sites_skipped() noexcept;

/**
 * @brief
 * \~english One site, already added up across every thread.
 * \~spanish Un sitio ya sumado entre todos los hilos.
 * \~
 *
 * \~english
 * THE CLASS MAP IS THE TAG'S **SHAPE** AXIS, not decoration.  A site that
 * always asks for the same thing is `Fixed`, and one spread over six classes is
 * GROWING -- and those two cases want opposite things from an arena, because a
 * container that grows ABANDONS its previous buffer and in an arena that never
 * comes back (2,414 -> 5,455 MB, measured).
 *
 * It is kept as a BITMAP and not as a minimum and a maximum: "from 16 to 8192"
 * does not tell a site that asks for those two sizes apart from one that walks
 * the twelve classes in between, and that is exactly the difference.  Counting
 * the bits gives the exact spread in one instruction.
 *
 * THE OTHER AXIS IS MISSING, and cannot be deduced from here: the **USE** --
 * how long what this site allocates lives -- comes out of the
 * allocation/release pair.  While that is not there, this report answers half.
 *
 * \~spanish
 * EL MAPA DE CLASES ES EL EJE **FORMA** DE LA ETIQUETA, no un adorno.  Un sitio
 * que siempre pide lo mismo es `Fixed`, y uno repartido por seis clases esta
 * CRECIENDO -- y esos dos casos quieren cosas opuestas de una arena, porque un
 * contenedor que crece ABANDONA su buffer anterior y en una arena eso no se
 * recupera nunca (2.414 -> 5.455 MB, medido).
 *
 * Se lleva como MAPA DE BITS y no como minimo y maximo: "de 16 a 8192" no
 * distingue un sitio que pide esos dos tamanos de uno que recorre las doce
 * clases de por medio, y esa es justo la diferencia.  Contar los bits da la
 * dispersion exacta en una instruccion.
 *
 * FALTA EL OTRO EJE, y no se puede deducir de aqui: el **USO** -- cuanto vive
 * lo que reserva este sitio -- sale del par reserva/liberacion.  Mientras no
 * este, este informe contesta la mitad.
 *
 * \~
 */
using AllocSite = ::VestaAllocSite;

/* UNA SOLA DEFINICION, y estas dos comprobaciones son las que lo sostienen.
 *
 * La estructura se declara en `util/report/alloc_sites_c.h`, que C puede leer, y aqui
 * solo se nombra otra vez.  Los dos tamanos que lleva dentro no pueden salir
 * de un `constexpr` -- C no lo entiende --, asi que estan escritos alli como
 * macros y se comprueban aqui: si alguien cambia uno de los dos numeros y no
 * el otro, esto no compila.  Sin esto serian dos fuentes que se separan en
 * silencio y las dos mitades leerian estructuras distintas. */
static_assert(kSizeBuckets == VESTA_ALLOC_SIZE_BUCKETS,
              "el reparto de tamanos de C y el de C++ tienen que ser el mismo");
static_assert(AllocTag::kSlots == VESTA_ALLOC_TAG_SLOTS,
              "las etiquetas de C y las de C++ tienen que ser las mismas");

/**
 * @brief
 * \~english Copies out the sites that allocate most, ordered from most to
 *          least.
 * \~spanish Copia los sitios que mas reservan, ordenados de mayor a menor.
 * \~
 *
 * \~english
 * FOR WHOM.  For a consumer that knows how to put NAMES to those addresses.
 * This library cannot: it is distributed separately and does not know how to
 * read anybody's symbols -- nor should it.  The compiler does know, because it
 * already reads symbol tables in order to link, so the division is: here it is
 * measured, there it is read.
 *
 * The threads are added up before sorting: the same site seen from eight
 * threads is ONE site, not eight.
 *
 * \~spanish
 * PARA QUIEN.  Para un consumidor que sepa poner NOMBRES a esas direcciones.
 * Esta libreria no puede: se distribuye aparte y no sabe leer simbolos de nadie
 * -- ni debe --.  El compilador si sabe, porque ya lee tablas de simbolos para
 * enlazar, asi que la division es: aqui se mide, alli se lee.
 *
 * Se suman los hilos antes de ordenar: un mismo sitio visto desde ocho hilos es
 * UN sitio, no ocho.
 *
 * \~
 * @param out
 * \~english the caller's array.  This library does NOT allocate here: it is
 *           called from the allocation path and from the end of the process,
 *           and in both places asking for memory is how a hang is reached.
 * \~spanish array del que llama.  Esta libreria NO reserva aqui: se la llama
 *           desde el camino de reservar y desde el final del proceso, y en los
 *           dos sitios pedir memoria es como se llega a un cuelgue.
 * \~
 * @param max
 * \~english how many fit in @p out.
 * \~spanish cuantos caben en @p out.
 * \~
 * @return
 * \~english how many were written.
 * \~spanish cuantos se escribieron.
 * \~
 *
 * \~english
 * @code
 *   util::AllocSite top[32];
 *   const unsigned n = util::alloc_sites_snapshot(top, 32);
 *   for (unsigned i = 0; i < n; ++i) name_and_print(top[i]);
 * @endcode
 *
 * \~spanish
 * @code
 *   util::AllocSite top[32];
 *   const unsigned n = util::alloc_sites_snapshot(top, 32);
 *   for (unsigned i = 0; i < n; ++i) nombrar_e_imprimir(top[i]);
 * @endcode
 *
 * \~
 */
unsigned alloc_sites_snapshot(AllocSite *out, unsigned max) noexcept;

/**
 * @brief
 * \~english Dumps the @p top sites that allocate most, raw.
 * \~spanish Vuelca los @p top sitios que mas reservan, en crudo.
 * \~
 *
 * \~english
 * It is the fallback for whoever uses the library ON ITS OWN and has nothing to
 * resolve with: it prints the addresses as they are loaded, and the module
 * base, which is the only thing that allows reading them with the binary in
 * another run.  Whoever can resolve should use @c alloc_sites_snapshot and show
 * names.
 *
 * \~spanish
 * Es el respaldo para quien use la libreria SUELTA y no tenga con que resolver:
 * imprime las direcciones tal como estan cargadas, y la base del modulo, que es
 * lo unico que permite leerlas con el binario en otra corrida.  Quien pueda
 * resolver deberia usar @c alloc_sites_snapshot y ensenar nombres.
 *
 * \~
 * @param top
 * \~english how many to show, from the one that allocates most downwards.
 * \~spanish cuantos ensenar, del que mas reserva hacia abajo.
 * \~
 */
void dump_alloc_sites(unsigned top) noexcept;

} // namespace util

#endif // VESTA_UTIL_ALLOC_SITES_H
