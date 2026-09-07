/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file tests/util/test_alloc_sites.cpp
 * @brief De DONDE viene lo que no dice para que es.
 *
 * El asignador ya sabe CUANTO llega sin declarar su proposito -- hoy el 100% --,
 * y eso dice cuanto falta pero no por donde empezar.  La tabla de sitios
 * contesta la otra pregunta.  Lo que se comprueba aqui es lo que la haria
 * inutil sin que nadie lo notara:
 *
 *  1. Que APUNTA: un sitio que reserva aparece, con su cuenta y sus bytes.
 *  2. Que DISTINGUE: dos sitios distintos no se confunden en uno.
 *  3. Que ACUMULA: el mismo sitio dos veces es una entrada con dos.
 *  4. Que **se calla cuando ya se sabe**: con un `AllocScope` abierto NO se
 *     apunta nada.  Es lo que hace que el coste sea proporcional a lo que
 *     falta y que esto se apague solo segun se avanza -- sin eso, seguiria
 *     costando lo mismo cuando ya no hiciera falta.
 *  5. Que **el desbordamiento se CUENTA**.  Una tabla que se llena y pierde
 *     sitios en silencio da una lista que parece completa y no lo es, que es
 *     peor que no tener lista.
 *  6. Que la identidad por tipo es UNICA por tipo y legible.
 *  7. Que la base del modulo permite escribir un desplazamiento resoluble.
 */

#include "util/alloc_sites.h"
#include "util/host_allocator.h"
#include "util/host_allocator_layout.h"
#include "util/os_memory.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int g_failures = 0;

void check(bool ok, const char *what) {
    std::printf("  [%s] %s\n", ok ? "OK  " : "FALLO", what);
    if (!ok) ++g_failures;
}

/// Para lo que se comprueba MILES de veces: solo habla cuando falla.  Una
/// linea por entrada de la tabla enterraria el resto del informe, y un informe
/// que no se puede leer es un informe que no se lee.
void check_quiet(bool ok, const char *what) {
    if (ok) return;
    std::printf("  [FALLO] %s\n", what);
    ++g_failures;
}

/// Direcciones falsas de "sitio".  No se desreferencian nunca: la tabla las
/// trata como identidades opacas, que es justo lo que son.
const void *fake_site(uintptr_t n) {
    return reinterpret_cast<const void *>(0x140000000ull + n * 64u);
}

/// Cuantas reservas cuenta la etiqueta "no se" ahora mismo.
uint64_t untagged_now() {
    return util::host_alloc_stats().by_tag[util::AllocTag{}.raw()];
}

/**
 * @brief Corre el volcado y devuelve lo que escribio.
 *
 * El volcado escribe en el canal de errores porque es un diagnostico, asi que
 * para mirarlo hay que desviarlo.  Es lo unico que un consumidor ve de esta
 * tabla, asi que es lo que hay que comprobar: mirar por dentro probaria una
 * estructura, no un resultado.
 */
std::string dump_to_string(unsigned top) {
    const char *path = "alloc_sites_dump.tmp";
    std::fflush(stderr);
    FILE *saved = freopen(path, "w", stderr);
    if (saved == nullptr) return std::string();
    util::dump_alloc_sites(top);
    std::fflush(stderr);

    std::string out;
    if (FILE *f = std::fopen(path, "rb")) {
        char buf[4096];
        size_t got;
        while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0)
            out.append(buf, got);
        std::fclose(f);
    }
    /* Devolver el canal a la consola.  Si no se puede, el test sigue: sus
     * resultados van por la salida normal, no por esta. */
#if defined(_WIN32)
    freopen("CON", "w", stderr);
#else
    freopen("/dev/tty", "w", stderr);
#endif
    std::remove(path);
    return out;
}

/// Cuantos bits puestos: cuantas clases de tamano toco un sitio.
int bits(uint64_t v) {
    int n = 0;
    while (v != 0) {
        v &= v - 1;
        ++n;
    }
    return n;
}

/// Cuantas lineas del volcado contienen @p needle.
size_t count_lines_with(const std::string &s, const char *needle) {
    size_t n = 0, pos = 0;
    while ((pos = s.find(needle, pos)) != std::string::npos) {
        ++n;
        ++pos;
    }
    return n;
}

} // namespace

int main() {
    std::printf("== de donde viene lo que no dice para que es ==\n");
    if (!util::host_alloc_active()) {
        std::printf("  asignador apagado (VESTA_NO_HOST_SLAB): nada que "
                    "comprobar\n");
        return 0;
    }

    /* La tabla vive detras de la misma bandera que el reparto de tamanos: una
     * medida que nadie pidio no se paga.  Aqui se pide a mano. */
    util::detail::g_measure = true;

    /* Una reserva de verdad ANTES de nada.  La tabla se lleva por hilo y cuelga
     * del cache del hilo, que no existe hasta su primera reserva -- en
     * produccion eso no se nota porque quien apunta es `operator new`, o sea
     * justo despues de reservar, pero aqui se llama a mano. */
    util::host_free(util::host_alloc(32));

    /* 1, 2 y 3.  Apunta, distingue y acumula -- y se comprueba por el VOLCADO,
     * que es lo unico que un consumidor ve.  No hay forma publica de leer una
     * entrada suelta, y no debe haberla: seria API existiendo solo para un
     * test, y entonces el test no probaria lo que se usa. */
    {
        util::record_alloc_site(fake_site(1), 100, 0);
        util::record_alloc_site(fake_site(1), 200, 0);
        util::record_alloc_site(fake_site(2), 50, 0);

        const std::string out = dump_to_string(10);
        check(out.find(" 2 allocs") != std::string::npos,
              "un sitio visto dos veces sale con DOS, no con una ni con tres");
        check(count_lines_with(out, " allocs") >= 2,
              "dos sitios distintos salen como dos entradas, no fundidos");
        check(out.find("module base") != std::string::npos &&
                  out.find("+0x") != std::string::npos,
              "y se vuelca desplazamiento + base, no la direccion tal cual");
    }

    /* 3b.  LA CLAVE ES EL PAR (sitio, proposito), que es lo que separa a este
     * asignador de uno normal.  El MISMO sitio llamado con dos propositos son
     * dos entradas: mezclarlas daria la media de dos poblaciones distintas, y
     * ademas haria imposible contrastar lo declarado con lo medido -- que es
     * para lo que existe la etiqueta (D9 del plan). */
    {
        const util::AllocTag a{util::AllocUse::Instant, util::AllocShape::Fixed};
        const util::AllocTag b{util::AllocUse::Long, util::AllocShape::Growing};
        const void *same = fake_site(7);
        for (int i = 0; i < 40; ++i)
            util::record_alloc_site(same, 24, a.raw());
        for (int i = 0; i < 25; ++i)
            util::record_alloc_site(same, 24, b.raw());

        util::AllocSite snap[64];
        const unsigned n = util::alloc_sites_snapshot(snap, 64);
        unsigned con_a = 0, con_b = 0;
        for (unsigned i = 0; i < n; ++i) {
            if (snap[i].pc != same) continue;
            if (snap[i].tag == a.raw() && snap[i].count == 40) ++con_a;
            if (snap[i].tag == b.raw() && snap[i].count == 25) ++con_b;
        }
        check(con_a == 1 && con_b == 1,
              "el mismo sitio con dos propositos son DOS entradas, cada una "
              "con su cuenta");
    }

    /* 3c.  LA FORMA sale de las clases que toca, que es el otro medio eje.  Un
     * sitio que pide siempre lo mismo cae en UNA clase; uno que crece, en
     * varias.  Sin esto la etiqueta que alguien declare no se puede contrastar
     * con nada. */
    {
        const void *fijo = fake_site(11);
        const void *crece = fake_site(12);
        for (int i = 0; i < 30; ++i)
            util::record_alloc_site(fijo, 48, 0);
        for (size_t s : {size_t(16), size_t(64), size_t(256), size_t(1024)})
            for (int i = 0; i < 10; ++i)
                util::record_alloc_site(crece, s, 0);

        util::AllocSite snap[64];
        const unsigned n = util::alloc_sites_snapshot(snap, 64);
        int clases_fijo = -1, clases_crece = -1;
        for (unsigned i = 0; i < n; ++i) {
            if (snap[i].pc == fijo) clases_fijo = bits(snap[i].class_mask);
            if (snap[i].pc == crece) clases_crece = bits(snap[i].class_mask);
        }
        check(clases_fijo == 1, "un sitio de tamano fijo toca UNA clase");
        check(clases_crece >= 4,
              "y uno que crece toca varias, que es como se ve que crece");
    }

    /* 4.  CON etiqueta no se apunta.  Es la propiedad que hace que esto se
     * apague solo, y la unica que se puede comprobar de punta a punta: se mira
     * que la reserva se cuente bajo la etiqueta y no bajo "no se". */
    {
        const util::AllocTag t{util::AllocUse::Instant, util::AllocShape::Fixed};
        const uint64_t untagged_before = untagged_now();
        const util::HostAllocStats before = util::host_alloc_stats();
        {
            const util::AllocScope scope(t);
            for (int i = 0; i < 1000; ++i)
                util::host_free(util::host_alloc(48));
        }
        const util::HostAllocStats after = util::host_alloc_stats();
        check(after.by_tag[t.raw()] - before.by_tag[t.raw()] >= 1000,
              "con ambito abierto, la reserva se cuenta bajo SU etiqueta");
        check(untagged_now() - untagged_before < 1000,
              "y no engorda el monton de \"no se\"");
    }

    /* 5.  LA PROPIEDAD QUE JUSTIFICA EL DESALOJO, y la razon de que este test
     * exista.
     *
     * Con la tabla llena de sitios raros, uno FRECUENTE que aparezca DESPUES
     * tiene que acabar en la lista.  Rendirse al llenarse -- que es lo que se
     * hacia -- convertia la lista en "los que llegaron primero": medido sobre
     * 144k lineas, se quedaba fuera el 38,5% de las reservas, y un sitio que
     * pidiera millones de bloques podia no salir por haber aparecido tarde.
     *
     * Se comprueba lo contrario de lo que uno miraria por instinto: no que los
     * raros sobrevivan -- no deben --, sino que el frecuente ENTRA. */
    {
        const uint64_t before = util::alloc_sites_overflow();
        for (uintptr_t i = 0; i < 20000; ++i)
            util::record_alloc_site(fake_site(1000 + i), 16, 0);
        const uint64_t evicted = util::alloc_sites_overflow() - before;
        check(evicted > 0, "con la ventana llena hay desalojos, y se CUENTAN");

        const void *late = fake_site(999999);
        for (int i = 0; i < 50000; ++i)
            util::record_alloc_site(late, 32, 0);

        const std::string out = dump_to_string(30);
        check(out.find("took over the weakest entry") != std::string::npos,
              "y el volcado dice que hubo desalojos y que ya van descontados");

        /* El desplazamiento del sitio tardio, con la misma cuenta que hace el
         * volcado -- no una parecida. */
        char want[32];
        std::snprintf(want, sizeof(want), "+0x%llx",
                      (unsigned long long)(uintptr_t(late) -
                                           uintptr_t(util::os_module_base())));
        check(out.find(want) != std::string::npos,
              "un sitio FRECUENTE que llega con la tabla llena entra igual");
    }

    // 6.  Identidad por tipo: una por tipo, y legible.
    {
        struct Alfa {};
        struct Beta {};
        const char *a = util::TypeName<Alfa>::get();
        const char *b = util::TypeName<Beta>::get();
        const char *a2 = util::TypeName<Alfa>::get();
        check(a != b, "dos tipos dan identidades distintas");
        check(a == a2, "y el mismo tipo da SIEMPRE la misma");
        check(std::strstr(a, "Alfa") != nullptr,
              "el nombre del tipo se lee en la identidad, sin resolver nada");
    }

    // 7.  La base del modulo, para poder volcar desplazamientos resolubles.
    {
        const void *base = util::os_module_base();
        check(base != nullptr, "el sistema dice donde esta cargado el modulo");
        if (base != nullptr) {
            /* Una funcion de este mismo binario tiene que caer por encima de la
             * base y a una distancia razonable: si el desplazamiento saliera
             * enorme o negativo, `addr2line` daria otra funcion o ninguna, y el
             * volcado pareceria correcto. */
            const uintptr_t here = reinterpret_cast<uintptr_t>(&check);
            const uintptr_t off = here - reinterpret_cast<uintptr_t>(base);
            check(here > reinterpret_cast<uintptr_t>(base) &&
                      off < (uintptr_t(1) << 31),
                  "y el desplazamiento de una funcion de aqui es plausible");
        }
    }

    /* 8.  LA TABLA ENTERA SE USA, Y LAS CUENTAS SUMAN LO QUE DEBEN.
     *
     * Los dos fallos que este bloque fija estuvieron vivos a la vez y se
     * tapaban el uno al otro:
     *
     *  - El reparto se quedaba con NUEVE bits cuando la tabla pide once, asi
     *    que las casillas de la 512 a la 2047 no se usaron nunca.  Y la
     *    mascara `& (kSlots - 1)` parecia la red de seguridad, pero una
     *    mascara solo estrecha: no puede revelar que el desplazamiento se
     *    paso.  Por eso se comprueba de fuera y por su EFECTO -- cuantas
     *    entradas distintas sobreviven --, que es lo unico que no se puede
     *    escribir mal en dos sitios a la vez.
     *  - Al desalojar, el que entra hereda la cuenta del que echa, y eso no se
     *    descontaba: sesenta y cuatro sitios llegaron a sumar el 192,5% de las
     *    reservas reales, con veinte filas seguidas ensenando la misma cifra.
     */
    {
        /* Cuantas casillas tiene una fila.  Vive en el `.cpp` del asignador, no
         * en la cabecera, asi que aqui se escribe aparte -- y si algun dia
         * dejan de coincidir, el que falla es este test, que es el sitio
         * correcto para enterarse. */
        constexpr unsigned kSlotsExpected = 2048;

        /* Estatico y no en la pila: son 128 KiB, y una pila reventada aqui se
         * leeria como un fallo del asignador. */
        static util::AllocSite snap[kSlotsExpected];

        const void *known = fake_site(4242424);
        const unsigned kCalls = 12345;
        for (unsigned i = 0; i < kCalls; ++i)
            util::record_alloc_site(known, 24, 0);

        const unsigned n = util::alloc_sites_snapshot(snap, 2048);
        /* EL UMBRAL NO ES 512, y la diferencia importa.  Con nueve bits el
         * reparto solo alcanzaba la casilla 511, pero el SONDEO avanza hasta
         * ocho mas, asi que el techo real eran 519 -- medido -- y una
         * comprobacion contra 512 pasaba con el fallo dentro.  La mitad de la
         * tabla es inalcanzable si el reparto se queda corto y trivial si
         * llega: medido, 519 antes y 2048 ahora. */
        check(n > kSlotsExpected / 2,
              "se usa la tabla ENTERA y no solo su primer cuarto");

        bool found = false;
        for (unsigned i = 0; i < n; ++i) {
            check_quiet(snap[i].over <= snap[i].count,
                        "lo heredado nunca puede pasar de la cuenta");
            check_quiet(snap[i].over_bytes <= snap[i].bytes,
                        "ni los bytes heredados de los bytes");
            if (snap[i].pc != known) continue;
            found = true;
            const uint64_t hechas = snap[i].count - snap[i].over;
            check(hechas <= kCalls,
                  "un sitio no puede haberse GANADO mas reservas de las que hizo");
            check(hechas >= kCalls / 2,
                  "y el suelo no se queda corto por descontar de mas");
        }
        check(found, "el sitio con cuenta conocida esta en la tabla");
    }

    util::detail::g_measure = false;
    std::printf(g_failures == 0 ? "TODO OK\n" : "%d FALLOS\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
