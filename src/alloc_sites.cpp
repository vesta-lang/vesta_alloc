/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/alloc_sites.cpp
 * @brief Implementacion de la tabla de sitios.  El contrato y los motivos, en
 *        la cabecera.
 */
#include "util/alloc_sites.h"

#include "util/host_allocator.h"
#include "util/host_allocator_layout.h"
#include "util/os_memory.h"

#include <atomic>
#include <cstdio>

namespace util {

namespace {

/**
 * @brief Cuantos sitios distintos cabe apuntar POR HILO.
 *
 * Potencia de dos: el indice sale de enmascarar.
 *
 * EMPEZO EN 512 Y LA MEDIDA LO CORRIGIO.  Compilando 144k lineas, con 512 se
 * quedaban fuera 24.128.919 reservas de 62.653.790 -- el 38,5% --, y eso no es
 * "faltan algunas": significa que la lista era "los sitios que llegaron antes
 * de llenarse", no "los que mas reservan".  Un sitio que apareciera tarde y
 * pidiera diez millones de bloques no habria salido.  En un `hola mundo` ya se
 * desbordaba.
 *
 * Con 2048 son 48 KiB por hilo y 3 MiB en total, y SOLO cuando se ha pedido
 * medir.  Es memoria de una herramienta de diagnostico, no del asignador.
 */
/**
 * @brief Cuantas casillas tiene la fila de un hilo.  POTENCIA DE DOS.
 *
 * Se declara por sus BITS y el tamano sale de ahi, no al reves, y eso no es
 * cosmetica: el tamano de la tabla estaba escrito DOS VECES -- aqui y, sin
 * decirlo, en el desplazamiento del reparto (`slot_of`) --.  Al subir de 512 a
 * 2048 solo cambio uno, el reparto siguio dando nueve bits y **las casillas de
 * la 512 a la 2047 no se usaron jamas**.
 *
 * Lo peor es como se escondio: `& (kSlots - 1)` parece la red que valida el
 * indice, y lo es, pero una mascara solo puede ESTRECHAR -- nunca puede revelar
 * que el desplazamiento se paso --.  O sea que el codigo parecia defensivo
 * justo donde estaba mal.  Con los bits como unica fuente, el `static_assert`
 * de abajo hace imposible que vuelvan a separarse.
 */
constexpr uint32_t kSlotBits = 11;
constexpr uint32_t kSlots = 1u << kSlotBits;

/// Cuantas casillas se miran antes de desalojar.  Con la tabla medio vacia la
/// primera acierta casi siempre; el limite existe para que una tabla llena no
/// convierta cada reserva en un recorrido completo.
constexpr uint32_t kMaxProbe = 8;

struct Entry {
    const void *pc;
    uint64_t count;
    uint64_t bytes;
    uint64_t class_mask; ///< un bit por clase; ver `AllocSite`
    uint64_t large;
    uint64_t over;       ///< cuenta HEREDADA al desalojar; ver el desalojo
    uint64_t over_bytes; ///< bytes heredados, por el mismo motivo
    uint8_t tag;         ///< el proposito; forma PARTE de la clave
    uint8_t _pad[7];
};

static_assert(sizeof(Entry) == 64,
              "una entrada, una linea de cache: el camino que las recorre es"
              " justamente el que se paga en cada reserva sin etiquetar");

static_assert(kClasses <= 64,
              "el mapa de clases es una palabra: con mas clases que bits habria"
              " que ensancharlo, no truncarlo en silencio");

static_assert(sizeof(Entry) * kSlots % 64 == 0,
              "la fila de un hilo tiene que medir lineas de cache enteras, o"
              " dos hilos se pelean por la ultima -- ver g_remote");

/**
 * @brief La tabla entera, de todos los hilos, en un solo bloque.
 *
 * UNA SOLA RESERVA Y NO UNA POR HILO: son 12 KiB por hilo y 768 KiB en total,
 * asi que pedirlo de golpe cuesta una llamada al sistema en vez de sesenta y
 * cuatro, y ademas deja las filas seguidas y alineadas a linea de cache -- que
 * es lo que evita que el hilo 3 y el 4 se peleen por la misma linea al apuntar
 * cosas que no tienen nada que ver.
 *
 * SE PIDE AL SISTEMA, NO AL ASIGNADOR.  Ni `new` ni `malloc` ni las clases de
 * aqui al lado: esto se llama DESDE el camino de reservar, asi que pedir por
 * ahi seria llamarse a si mismo.
 */
std::atomic<Entry *> g_table{nullptr};
std::atomic<uint64_t> g_overflow{0};

/**
 * @brief Peticiones de apuntar que no llegaron a la tabla.
 *
 * POR QUE ESTE Y NO UN CONTADOR DE LLAMADAS.  Porque el de llamadas iria en el
 * camino de CADA reserva, y un atomico global ahi es una linea de cache que se
 * pelean todos los hilos -- exactamente lo que este asignador evita en todo lo
 * demas --: mediria mas despacio de lo que hay, que es la peor forma de medir.
 *
 * Y no hace falta, porque hay una identidad exacta: toda llamada o se va por
 * una salida temprana o sube en UNO alguna entrada.  Asi que
 *
 *     pedidas == suma de la tabla + descartadas
 *
 * y las salidas tempranas son FRIAS -- un hilo sin cache propio --, asi que el
 * contador cae fuera del camino caliente.  Con esa identidad se puede decir de
 * que lado esta un descuadre: si la suma de la tabla mas esto cuadra con las
 * reservas del asignador, la tabla es fiel y el que cuenta mal es el contador.
 */
std::atomic<uint64_t> g_skipped{0};

/// Apunta una peticion que no se pudo registrar.  Fria a proposito: solo la
/// alcanza un hilo que no consiguio cache propio.
[[gnu::cold]] void skipped() noexcept {
    g_skipped.fetch_add(1, std::memory_order_relaxed);
}

/// Bytes del bloque entero.
constexpr size_t kTableBytes = size_t(kMaxThreads) * kSlots * sizeof(Entry);

/// Reserva el bloque la primera vez.  Devuelve nulo si el sistema no da; en ese
/// caso no se apunta nada, que es peor que apuntarlo pero mejor que mentir.
Entry *table() noexcept {
    Entry *t = g_table.load(std::memory_order_acquire);
    if (t != nullptr) return t;

    void *raw = os_reserve(kTableBytes);
    if (raw == nullptr) return nullptr;
    if (!os_commit(raw, kTableBytes)) {
        os_release(raw, kTableBytes);
        return nullptr;
    }
    Entry *mine = static_cast<Entry *>(raw);

    Entry *expected = nullptr;
    if (g_table.compare_exchange_strong(expected, mine,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire))
        return mine;
    /* Gano otro: se devuelve el de mas.  Pasa como mucho una vez en la vida
     * del proceso. */
    os_release(raw, kTableBytes);
    return expected;
}

/// Reparte una direccion por toda la tabla.  Los punteros de retorno de un
/// mismo modulo comparten los bits altos y se diferencian en los bajos, asi que
/// tomar un trozo tal cual amontonaria; multiplicar y quedarse con los altos
/// reparte cualquier paso regular.  Es el mismo motivo que en `thread_slot`.
/// La clave es el PAR (sitio, proposito), asi que las dos partes entran en el
/// reparto: si no, el mismo sitio con dos propositos caeria siempre en la misma
/// casilla y uno de los dos desalojaria al otro sin parar.
inline uint32_t slot_of(const void *pc, uint8_t tag) noexcept {
    const uint64_t x = uint64_t(reinterpret_cast<uintptr_t>(pc)) ^
                       (uint64_t(tag) * 0xC2B2AE3D27D4EB4Full);
    /* El desplazamiento SALE del tamano: quedarse con los `kSlotBits` de mas
     * peso del producto ya da un indice del rango exacto, sin mascara.  Ver
     * `kSlotBits`: escribir el numero a mano es lo que dejo tres cuartas partes
     * de la tabla muertas durante toda su vida. */
    return uint32_t((x * 0x9E3779B97F4A7C15ull) >> (64 - kSlotBits));
}

static_assert(kSlots == (1u << kSlotBits),
              "el tamano de la tabla y los bits del reparto son EL MISMO dato:"
              " si se separan, parte de la tabla deja de existir en silencio");

} // namespace

void record_alloc_site(const void *pc, size_t n, uint8_t tag) noexcept {
    if (pc == nullptr) return;
    const detail::ThreadCache *c = detail::current_cache();
    if (c == nullptr) return skipped(); // sin cache no hay fila donde apuntar

    Entry *t = table();
    if (t == nullptr) return skipped();
    Entry *row = t + size_t(c->id) * kSlots;

    uint32_t i = slot_of(pc, tag);
    Entry *weakest = &row[i];
    for (uint32_t probe = 0; probe < kMaxProbe; ++probe) {
        Entry &e = row[i];
        /* LA CLAVE ES EL PAR.  El mismo sitio llamado desde dos fases con
         * propositos distintos son dos entradas, no una: mezclarlas daria la
         * media de dos poblaciones que no tienen nada que ver. */
        if ((e.pc == pc && e.tag == tag) || e.pc == nullptr) {
            /* La fila es de ESTE hilo: nadie mas la escribe, asi que no hay
             * nada que sincronizar.  Es la misma propiedad que hace barato
             * todo lo demas del asignador. */
            e.pc = pc;
            e.tag = tag;
            e.count += 1;
            e.bytes += n;
            if (n > kMaxSmall)
                e.large += 1;
            else
                e.class_mask |= uint64_t(1) << class_of(n);
            return;
        }
        if (e.count < weakest->count) weakest = &e;
        i = (i + 1) & (kSlots - 1);
    }

    /* LA VENTANA ESTA LLENA: SE DESALOJA LA MENOR Y SE HEREDA SU CUENTA.
     *
     * Rendirse aqui -- que es lo que se hacia -- convierte la lista en "los
     * sitios que llegaron primero": uno que aparezca tarde y reserve millones
     * de veces no entra nunca, porque su primera visita ya no cabe.  Medido
     * sobre 144k lineas, con eso se perdia el 38,5% de las reservas.
     *
     * Heredando la cuenta, un sitio de verdad frecuente sube en pocas visitas
     * y uno raro vuelve a salir enseguida, que es como se encuentran los mas
     * pesados con memoria acotada.
     *
     * Y SE GUARDA LO HEREDADO, que es la parte que faltaba.  Contar los
     * desalojos AVISA de que las cuentas son cotas superiores, pero no permite
     * corregir ninguna: sin esto, sesenta y cuatro sitios llegaron a sumar el
     * 192,5% de las reservas reales, y veinte filas seguidas ensenaban
     * exactamente 330 -- que no eran veinte sitios con 330 reservas, era UNA
     * cuenta copiandose hacia delante y presentandose como ganada --.
     *
     * Con `over` la cuenta de verdad esta en `[count - over, count]`: el suelo
     * es lo que ESTE sitio se gano desde que entro, y la suma de los suelos
     * cabe en el total por construccion.  Es el termino de error que el
     * algoritmo de los pesados lleva de serie y que aqui se estaba tirando. */
    /* El MAPA DE CLASES si se reinicia: heredar la cuenta es lo que hace subir
     * a un sitio frecuente, pero heredar las clases de OTRO seria inventarse la
     * forma del que entra -- y la forma es justo lo que se mira para decidir si
     * puede ir a una arena. */
    weakest->pc = pc;
    weakest->tag = tag;
    weakest->class_mask = 0;
    weakest->over = weakest->count;       // lo que trae prestado del anterior
    weakest->over_bytes = weakest->bytes; // y los bytes, por lo mismo
    weakest->count += 1;
    weakest->bytes += n;
    if (n > kMaxSmall)
        weakest->large += 1;
    else
        weakest->class_mask = uint64_t(1) << class_of(n);
    g_overflow.fetch_add(1, std::memory_order_relaxed);
}

uint64_t alloc_sites_overflow() noexcept {
    return g_overflow.load(std::memory_order_relaxed);
}

uint64_t alloc_sites_skipped() noexcept {
    return g_skipped.load(std::memory_order_relaxed);
}

/**
 * @brief Suma los hilos y deja los @p max mayores en @p out, ordenados.
 *
 * SIN RESERVAR NADA.  Se llama al final del proceso y desde herramientas que
 * corren dentro del propio asignador; pedir memoria en cualquiera de los dos
 * sitios es como se llega a un cuelgue al salir.  Por eso el array lo pone
 * quien llama.
 */
unsigned merge_sites(AllocSite *out, unsigned max) noexcept {
    const Entry *t = g_table.load(std::memory_order_acquire);
    if (t == nullptr || out == nullptr || max == 0) return 0;

    unsigned kept = 0;
    for (uint32_t th = 0; th < kMaxThreads; ++th) {
        const Entry *row = t + size_t(th) * kSlots;
        for (uint32_t s = 0; s < kSlots; ++s) {
            if (row[s].pc == nullptr || row[s].count == 0) continue;
            /* Si ya estaba (otro hilo) se suma; si no, entra, y cuando esta
             * lleno desplaza al menor.  Es cuadratico sobre unas decenas de
             * elementos y corre una vez. */
            unsigned j = 0;
            for (; j < kept; ++j)
                if (out[j].pc == row[s].pc && out[j].tag == row[s].tag) break;
            if (j < kept) {
                out[j].count += row[s].count;
                out[j].bytes += row[s].bytes;
                out[j].large += row[s].large;
                /* Lo heredado se suma como todo lo demas: juntar dos filas es
                 * juntar tambien su incertidumbre, no perderla por el camino. */
                out[j].over += row[s].over;
                out[j].over_bytes += row[s].over_bytes;
                /* Las clases son las de TODOS los hilos juntos: el mismo sitio
                 * puede pedir tamanos distintos segun quien lo llame. */
                out[j].class_mask |= row[s].class_mask;
                continue;
            }
            const AllocSite fresh{row[s].pc,    row[s].count,
                                  row[s].bytes, row[s].class_mask,
                                  row[s].large, row[s].over,
                                  row[s].over_bytes, row[s].tag};
            if (kept < max) {
                out[kept++] = fresh;
                continue;
            }
            unsigned worst = 0;
            for (unsigned k = 1; k < kept; ++k)
                if (out[k].count < out[worst].count) worst = k;
            if (row[s].count > out[worst].count) out[worst] = fresh;
        }
    }

    /* Ordenar por lo que cada sitio SE GANO, no por su cota superior.
     *
     * Y son criterios distintos a proposito: dentro de la tabla, el desalojo
     * mira la cuenta con lo heredado incluido -- eso es lo que protege al
     * recien entrado y lo que le permite subir si de verdad es frecuente --,
     * mientras que la LISTA que se ensena ordena por el suelo, porque uno que
     * acaba de heredar 330 no ha reservado mas que otro que lleva 300 hechas.
     * Mezclar los dos criterios es lo que ponia veinte filas identicas arriba
     * del todo. */
    for (unsigned a = 1; a < kept; ++a) {
        const AllocSite v = out[a];
        const uint64_t vf = v.count - v.over;
        unsigned b = a;
        while (b > 0 && (out[b - 1].count - out[b - 1].over) < vf) {
            out[b] = out[b - 1];
            --b;
        }
        out[b] = v;
    }
    return kept;
}

unsigned alloc_sites_snapshot(AllocSite *out, unsigned max) noexcept {
    return merge_sites(out, max);
}

void dump_alloc_sites(unsigned top) noexcept {
    /* Un array en la PILA, por lo mismo que `merge_sites` no reserva. */
    constexpr unsigned kKeep = 64;
    AllocSite best[kKeep];
    const unsigned kept = merge_sites(best, kKeep);
    if (kept == 0) return;

    /* EL DESPLAZAMIENTO RESPECTO A DONDE ESTA CARGADO, y nada mas.  La
     * direccion tal cual no se puede leer con el binario en otra corrida --
     * cambia con la disposicion aleatoria --, y la base para la que se ENLAZO
     * no se puede averiguar desde dentro: en Windows el cargador PARCHEA ese
     * campo de la cabecera al reubicar, asi que leerlo devuelve donde esta
     * cargado y no donde se enlazo.  Se intento y devolvia la absoluta.
     *
     * Asi que esto imprime lo unico que aqui se sabe de verdad, y el consumidor
     * que tenga simbolos pone los nombres: ver `alloc_sites_snapshot`. */
    const uintptr_t loaded = uintptr_t(os_module_base());
    std::fprintf(stderr,
                 "[allocator] top %u allocation sites with NO declared "
                 "purpose, as offsets from the module base (%p)\n",
                 top, (const void *)loaded);
    const unsigned show = kept < top ? kept : top;
    for (unsigned a = 0; a < show; ++a) {
        /* Lo que ese sitio SE GANO y, si heredo algo, hasta donde podria
         * llegar.  Una sola cifra tendria que ser la cota superior, y esa
         * miente hacia arriba sin decirlo. */
        const uint64_t floor = best[a].count - best[a].over;
        std::fprintf(stderr, "    +0x%-12llx %12llu allocs",
                     (unsigned long long)(uintptr_t(best[a].pc) - loaded),
                     (unsigned long long)floor);
        if (best[a].over != 0)
            std::fprintf(stderr, " (up to %llu)",
                         (unsigned long long)best[a].count);
        std::fprintf(stderr, "  %10.1f MiB\n",
                     double(best[a].bytes - best[a].over_bytes) /
                         (1024.0 * 1024.0));
    }
    const uint64_t evicted = alloc_sites_overflow();
    if (evicted != 0)
        std::fprintf(stderr,
                     "    %llu allocations found a full probe window and took "
                     "over the weakest entry: for those sites the true count is "
                     "between the two figures above\n",
                     (unsigned long long)evicted);
}

} // namespace util
