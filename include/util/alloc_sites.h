/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/alloc_sites.h
 * @brief De DONDE sale cada reserva que llega sin declarar su proposito.
 *
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
 */
#ifndef VESTA_UTIL_ALLOC_SITES_H
#define VESTA_UTIL_ALLOC_SITES_H

#include <cstddef>
#include <cstdint>

namespace util {

/**
 * @brief La identidad de un tipo, sin registro y sin contadores.
 *
 * Su DIRECCION es el identificador -- unica por `T`, sin carreras y sin nada
 * que inicializar -- y su CONTENIDO es el nombre legible.  Las dos cosas de una
 * sola pieza, que es lo que evita tener que mantener una tabla de nombres.
 *
 * `__PRETTY_FUNCTION__` trae el tipo dentro del texto de la firma, asi que el
 * volcado sale legible sin resolver ningun simbolo.
 */
template <class T> struct TypeName {
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
 * @brief Apunta una reserva hecha desde @p pc con el proposito @p tag.
 *
 * @param pc   Direccion de retorno del que reservo, o la direccion de un
 *             @c TypeName<T>::get para una reserva de contenedor tipado.
 * @param n    Bytes pedidos.
 * @param tag  El proposito que corria en ese momento, empaquetado.  Cero es
 *             "no se", que es lo que queda por declarar.
 *
 * SOLO cuenta cuando el reparto de tamanos esta encendido
 * (`VESTA_HOST_ALLOC_STATS=1`).  La CAPTURA de la direccion es gratis y puede
 * quedarse siempre -- esta medido --, pero apuntarla en una tabla es una
 * medida, y una medida que no se ha pedido no se paga.
 *
 * NO APUNTA NADA si el hilo todavia no tiene cache propio, porque la tabla se
 * lleva por hilo y cuelga de el.  Desde `operator new` eso no puede pasar --
 * se llama justo DESPUES de reservar, asi que el cache acaba de nacer --, pero
 * quien la llame a mano tiene que haber reservado antes.
 */
void record_alloc_site(const void *pc, size_t n, uint8_t tag) noexcept;

/**
 * @brief Cuantas veces una reserva encontro su ventana llena y desalojo a la
 *        entrada mas floja.
 *
 * NO ES MEMORIA PERDIDA: nada se deja de contar.  Lo que dice es que a partir
 * de ahi las cuentas son COTAS SUPERIORES, porque un sitio nuevo hereda la
 * cuenta del que echa.  Se lleva y se dice porque una lista que finge ser
 * exacta cuando no lo es se usa para decidir, y decide mal.
 */
uint64_t alloc_sites_overflow() noexcept;

/**
 * @brief Peticiones de apuntar que no llegaron a la tabla.
 *
 * Solo ocurre con un hilo sin cache propio -- por encima del tope de duenos --,
 * asi que normalmente es cero.  Es la tercera cifra que hace falta para leer el
 * informe sin adivinar: toda llamada o se descarta aqui o sube en uno alguna
 * entrada, de modo que `suma de la tabla + esto` son las reservas que pasaron
 * por el sitio de captura.  Si eso no cuadra con lo que cuenta el asignador, el
 * descuadre esta en el CONTADOR y no en la tabla, que es una pregunta distinta
 * y se arregla en otro sitio.
 *
 * No hay contador de llamadas totales a proposito: iria en el camino de cada
 * reserva y un atomico compartido ahi mide mas despacio de lo que hay.
 */
uint64_t alloc_sites_skipped() noexcept;

/**
 * @brief Un sitio ya sumado entre todos los hilos.
 *
 * EL MAPA DE CLASES ES EL EJE **FORMA** DE LA ETIQUETA, no un adorno.  El plan
 * lo dice asi: un sitio que siempre pide lo mismo es `Fixed`, y uno repartido
 * por seis clases esta CRECIENDO -- y esos dos casos quieren cosas opuestas de
 * una arena, porque un contenedor que crece ABANDONA su buffer anterior y en
 * una arena eso no se recupera nunca (2.414 -> 5.455 MB, medido).
 *
 * Se lleva como MAPA DE BITS y no como minimo y maximo: "de 16 a 8192" no
 * distingue un sitio que pide esos dos tamanos de uno que recorre las doce
 * clases de por medio, y esa es justo la diferencia.  Contar los bits da la
 * dispersion exacta en una instruccion.
 *
 * FALTA EL OTRO EJE, y no se puede deducir de aqui: el **USO** -- cuanto vive
 * lo que reserva este sitio -- sale del par reserva/liberacion (D8), que es la
 * fase 4 del plan.  Mientras no este, este informe contesta la mitad.
 */
struct AllocSite {
    const void *pc;      ///< la direccion tal como esta CARGADA
    uint64_t count;      ///< reservas: COTA SUPERIOR.  Ver @c over
    uint64_t bytes;      ///< bytes pedidos: cota superior.  Ver @c over_bytes
    uint64_t class_mask; ///< un bit por clase de tamano tocada
    uint64_t large;      ///< cuantas pasaban del tope de clases
    /**
     * @brief Lo que esta entrada HEREDO de la que desalojo, y no se gano.
     *
     * La tabla tiene sitio acotado, asi que cuando se llena una ventana se
     * echa a la mas debil y la que entra hereda su cuenta.  Sin eso, la lista
     * seria "los sitios que llegaron primero" y un sitio frecuente que aparece
     * tarde no entraria nunca -- medido, se perdia el 38,5% de las reservas.
     *
     * El precio es que la cuenta deja de ser una cuenta, y ESO HAY QUE PODER
     * DESHACERLO: lo cierto esta en `[count - over, count]`.  El suelo es lo
     * que este sitio se gano desde que entro y la suma de los suelos cabe en
     * el total; la cota superior, sola, hacia que los sitios sumaran el 192,5%
     * de las reservas reales sin que nada lo delatara.
     *
     * Cero significa que la entrada nunca desalojo a nadie: ahi la cuenta es
     * exacta, no estimada.
     */
    uint64_t over;
    uint64_t over_bytes; ///< lo mismo para @c bytes
    /**
     * @brief El PROPOSITO con el que reservo, empaquetado (@c AllocTag).
     *
     * LA CLAVE ES EL PAR (sitio, proposito) Y NO EL SITIO, y esto es lo que
     * separa a este asignador de uno normal.  Una misma funcion llamada desde
     * dos fases distintas reserva para dos cosas distintas, y mezclarlas da la
     * media de dos poblaciones que no tienen nada que ver -- que es el mismo
     * error que confundir dos clases de nucleo en una medida.
     *
     * Ademas es lo que permite CONTRASTAR: el proposito que alguien declaro se
     * pone al lado de la forma y la vida MEDIDAS de ese mismo sitio, y si no
     * cuadran, la declaracion esta mal.  Sin esta columna la etiqueta seria
     * incontestable, que es justo lo que el plan no quiere (D9).
     */
    uint8_t tag;
};

/**
 * @brief Copia los sitios que mas reservan, ordenados de mayor a menor.
 *
 * @param out  Array del que llama.  Esta libreria NO reserva aqui: se la llama
 *             desde el camino de reservar y desde el final del proceso, y en
 *             los dos sitios pedir memoria es como se llega a un cuelgue.
 * @param max  Cuantos caben en @p out.
 * @return Cuantos se escribieron.
 *
 * PARA QUIEN.  Para un consumidor que sepa poner NOMBRES a esas direcciones.
 * Esta libreria no puede: se distribuye aparte y no sabe leer simbolos de nadie
 * -- ni debe --.  El compilador si sabe, porque ya lee tablas de simbolos para
 * enlazar, asi que la division es: aqui se mide, alli se lee.
 *
 * Se suman los hilos antes de ordenar: un mismo sitio visto desde ocho hilos es
 * UN sitio, no ocho.
 */
unsigned alloc_sites_snapshot(AllocSite *out, unsigned max) noexcept;

/**
 * @brief Vuelca los @p top sitios que mas reservan, en crudo.
 *
 * Es el respaldo para quien use la libreria SUELTA y no tenga con que resolver:
 * imprime las direcciones tal como estan cargadas, y la base del modulo, que es
 * lo unico que permite leerlas con el binario en otra corrida.  Quien pueda
 * resolver deberia usar @c alloc_sites_snapshot y ensenar nombres.
 */
void dump_alloc_sites(unsigned top) noexcept;

} // namespace util

#endif // VESTA_UTIL_ALLOC_SITES_H
