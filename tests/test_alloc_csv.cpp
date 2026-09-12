/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file tests/util/test_alloc_csv.cpp
 * @brief That the export is READABLE BY A TOOL, not just written.
 *
 * WHAT MUST NOT BREAK, and why these checks and not others.  A CSV that opens
 * is not the same as a CSV that is correct, and the ways this one can go wrong
 * are specific:
 *
 *   - A C++ name is full of commas.  Written raw it splits into columns and the
 *     tool reads the tail of a template argument as if it were the next field
 *     -- without any error, with a plausible-looking result.  So a name with
 *     commas AND quotes goes in on purpose, and it is checked that it comes
 *     back out whole.
 *   - The two tables join on `site_id`.  If a frame carries an id that is not
 *     in `sites.csv`, the tool hangs branches off nothing.
 *   - The directory is created.  Requiring one to exist buys a single failure
 *     mode, and a bad one: the run ends and the report is gone.
 *   - Without a resolver installed the files still come out, and `summary.csv`
 *     SAYS there are no names.  An empty table means "nothing is known"; a
 *     missing file means "something broke", and they must not look alike.
 *
 * The parser here is deliberately naive except for quoting: it is a test, and
 * what it has to prove is that a naive reader -- like the Python tool -- gets
 * the fields back the way they went in.
 */

#include "util/report/alloc_csv.h"
#include "util/report/alloc_sites.h" // para sembrar sitios a mano; ver `main`
#include "util/alloc/host_allocator.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const char *what) {
    std::printf("  [%s] %s\n", ok ? "OK  " : "FAIL", what);
    if (!ok) ++g_failures;
}

/// A name with everything that breaks a CSV: commas, quotes and angle
/// brackets.  It is not far-fetched -- half the report looks like this.
const char kNastyName[] =
    "std::pair<std::string const, std::vector<Foo, \"bar\">>::pair(int, char)";

unsigned fake_resolver(const void *pc, util::AllocFrame *out, unsigned max) {
    (void)pc;
    if (max < 2) return 0;
    out[0].function = kNastyName;
    out[0].file = "src/some,file.cpp"; // una coma en la RUTA, que tambien pasa
    out[0].line = 42;
    out[0].inlined = true;
    out[0].module = "mod,ulo"; // con coma: el modulo tambien se entrecomilla
    out[1].function = "outer_function()";
    out[1].file = "src/other.cpp";
    out[1].line = 7;
    out[1].inlined = false;
    /* `module` de este marco se deja SIN TOCAR a proposito: un resolutor
     * rellena lo que sabe, y lo que no toca tiene que salir vacio.  Si el
     * escritor no limpiara el array entre sitio y sitio, aqui saldria el
     * modulo del sitio ANTERIOR -- un valor plausible y falso, que es la peor
     * clase de dato equivocado porque no se distingue mirando. */
    return 2;
}

/// Turns every name upper-case.  No pretende ser un desmanglador: lo que se
/// comprueba es que el gancho SE LLAMA y que lo que devuelve es lo que sale.
/* UN BUFFER, NO UN `std::string`, y la razon la encontro el nivel de guarda.
 *
 * Este gancho lo llama tambien el informe del comprobador, que corre al SALIR.
 * Con un `static std::string` dentro, para entonces su destructor ya paso y
 * escribir en el es un uso despues de destruir: en los niveles de abajo el
 * bloque sigue mapeado y no se nota, y en el de guarda sus paginas ya no estan y
 * el proceso muere ahi -- que es el nivel haciendo exactamente su trabajo.
 *
 * Un array de caracteres no tiene destructor que pueda haber corrido ni reserva
 * que pueda haberse soltado.  Lo que no cabe se recorta, que es lo que un
 * formateador de nombres puede permitirse. */
const char *shouty_formatter(const char *raw) {
    static char held[512];
    size_t i = 0;
    for (; raw[i] != '\0' && i + 1 < sizeof held; ++i)
        held[i] = (raw[i] >= 'a' && raw[i] <= 'z')
                      ? char(raw[i] - 'a' + 'A')
                      : raw[i];
    held[i] = '\0';
    return held;
}

/// Un lector de CSV que respeta las comillas, que es lo unico que el escritor
/// promete.  Devuelve las filas ya partidas en campos.
std::vector<std::vector<std::string>> read_csv(const std::string &path) {
    std::vector<std::vector<std::string>> rows;
    FILE *f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return rows;

    std::vector<std::string> row;
    std::string field;
    bool quoted = false;
    int c;
    while ((c = std::fgetc(f)) != EOF) {
        if (quoted) {
            if (c != '"') {
                field.push_back(char(c));
                continue;
            }
            const int next = std::fgetc(f);
            if (next == '"') {
                field.push_back('"'); // comilla doblada: una sola de verdad
            } else {
                quoted = false;
                if (next != EOF) std::ungetc(next, f);
            }
            continue;
        }
        if (c == '"' && field.empty()) {
            quoted = true;
        } else if (c == ',') {
            row.push_back(field);
            field.clear();
        } else if (c == '\n') {
            row.push_back(field);
            field.clear();
            rows.push_back(row);
            row.clear();
        } else if (c != '\r') {
            field.push_back(char(c));
        }
    }
    if (!field.empty() || !row.empty()) {
        row.push_back(field);
        rows.push_back(row);
    }
    std::fclose(f);
    return rows;
}

/// El valor de una clave en `summary.csv`.
std::string summary_value(const std::string &dir, const char *key) {
    for (const auto &row : read_csv(dir + "/summary.csv"))
        if (row.size() >= 2 && row[0] == key) return row[1];
    return std::string();
}

/// Una carpeta temporal que NO existe todavia: es parte de lo que se prueba.
std::string temp_dir() {
#if defined(_WIN32)
    const char *base = std::getenv("TEMP");
    if (base == nullptr) base = ".";
#else
    const char *base = "/tmp";
#endif
    return std::string(base) + "/vesta_alloc_csv_test/nested";
}

} // namespace

int main() {
    std::printf("== the allocation report as CSV ==\n");

    if (!util::host_alloc_active()) {
        std::printf("  el asignador no esta en vigor: nada que exportar\n");
        return 0;
    }

    /* QUE HAYA ALGO QUE EXPORTAR, y puesto a mano.
     *
     * Apuntar de donde viene cada reserva vive detras de una bandera que aqui
     * nadie ha puesto, asi que reservar por las buenas no deja ni un sitio en
     * la tabla -- y el test acabaria comprobando que un fichero vacio esta
     * vacio, que pasa siempre.  Se enciende la medida y se apuntan sitios con
     * la API publica, que ademas los hace DETERMINISTAS: con reservas de
     * verdad, cuantos sitios salgan depende de que inline el compilador. */
    util::detail::g_measure = true;
    /* Una reserva de verdad antes de nada: la tabla cuelga del cache del hilo,
     * que no existe hasta su primera reserva. */
    util::host_free(util::host_alloc(32));

    for (int i = 0; i < 5; ++i) {
        const void *pc =
            reinterpret_cast<const void *>(uintptr_t(&main) + uintptr_t(i * 64));
        util::record_alloc_site(pc, 100 + size_t(i), 0);
        util::record_alloc_site(pc, 200 + size_t(i), 0);
    }

    const std::string dir = temp_dir();

    // -- sin gancho de nombres ----------------------------------------------
    util::alloc_set_symbol_resolver(nullptr);
    util::alloc_set_name_formatter(nullptr);
    check(util::write_alloc_csv(dir.c_str()),
          "se exporta aunque la carpeta no exista (se crea, con sus niveles)");
    check(!read_csv(dir + "/sites.csv").empty(), "sites.csv tiene contenido");
    check(read_csv(dir + "/frames.csv").size() == 1,
          "sin resolutor, frames.csv es SOLO la cabecera -- no falta, esta "
          "vacio, que no es lo mismo");
    check(summary_value(dir, "has_symbols") == "0",
          "y summary.csv dice que no hay nombres");

    // -- con gancho ---------------------------------------------------------
    util::alloc_set_symbol_resolver(&fake_resolver);
    util::alloc_set_name_formatter(&shouty_formatter);
    check(util::write_alloc_csv(dir.c_str()), "se exporta con los ganchos");

    const auto sites = read_csv(dir + "/sites.csv");
    const auto frames = read_csv(dir + "/frames.csv");
    check(sites.size() > 1 && frames.size() > 1, "las dos tablas traen filas");
    check(summary_value(dir, "has_symbols") == "1",
          "summary.csv dice que si hay nombres");
    check(summary_value(dir, "sites_without_frames") == "0",
          "y que no quedo ningun sitio sin resolver");

    /* LA COMPROBACION QUE IMPORTA: el nombre feo vuelve ENTERO.  Con las
     * comillas mal puestas saldria partido en varias columnas y el numero de
     * campos delataria justo eso. */
    /* LAS COLUMNAS, POR NOMBRE.  Estaban por posicion y la fila se validaba
     * contando campos, asi que anadir una columna al fichero ponia CUATRO
     * comprobaciones en rojo -- ninguna por su propio motivo --, y la que decia
     * "tiene sus siete campos" es la que mas despista: lo que fallaba no era el
     * entrecomillado, era el numero siete escrito aqui. */
    const size_t n_cols = frames[0].size();
    size_t c_site = 0, c_depth = 0, c_fn = 0, c_file = 0, c_mod = 0;
    for (size_t c = 0; c < n_cols; ++c) {
        if (frames[0][c] == "site_id") c_site = c;
        if (frames[0][c] == "depth") c_depth = c;
        if (frames[0][c] == "function") c_fn = c;
        if (frames[0][c] == "file") c_file = c;
        if (frames[0][c] == "module") c_mod = c;
    }
    check(c_fn != 0 && c_file != 0 && c_mod != 0,
          "frames.csv dice en su cabecera donde esta cada columna");
    bool whole = false, columns_ok = true, joins = true;
    std::string ids;
    for (size_t i = 1; i < sites.size(); ++i)
        if (!sites[i].empty()) ids += "," + sites[i][0] + ",";
    for (size_t i = 1; i < frames.size(); ++i) {
        const auto &row = frames[i];
        if (row.size() != n_cols) {
            columns_ok = false;
            continue;
        }
        if (ids.find("," + row[c_site] + ",") == std::string::npos)
            joins = false;
        if (row[c_fn] == shouty_formatter(kNastyName)) whole = true;
    }
    check(columns_ok,
          "toda fila de frames.csv tiene tantos campos como su cabecera");
    check(whole,
          "un nombre con comas y comillas vuelve entero por el formateador");
    check(joins, "todo site_id de frames.csv existe en sites.csv");

    bool comma_path = false, module_ok = false, module_clean = true;
    for (size_t i = 1; i < frames.size(); ++i) {
        if (frames[i].size() != n_cols) continue;
        if (frames[i][c_file] == "src/some,file.cpp") comma_path = true;
        if (frames[i][c_depth] == "0" && frames[i][c_mod] == "mod,ulo")
            module_ok = true;
        /* El marco que el resolutor NO rellena tiene que salir vacio, corrida
         * tras corrida.  Con basura heredada saldria el modulo del sitio
         * anterior y el arbol agruparia por algo que nadie midio. */
        if (frames[i][c_depth] == "1" && !frames[i][c_mod].empty())
            module_clean = false;
    }
    check(comma_path, "y una RUTA con coma tambien");

    /* La cuenta de clases y la mascara tienen que decir lo MISMO.  Van las dos
     * porque la cuenta no se puede sumar entre sitios -- dos sitios con la
     * misma clase darian dos --, asi que quien agrupe necesita los bits; y si
     * alguna vez dejan de cuadrar, lo que falla es silencioso: un numero
     * plausible en una columna que nadie vuelve a comprobar. */
    bool mask_agrees = true;
    for (size_t i = 1; i < sites.size(); ++i) {
        if (sites[i].size() < 8) continue;
        unsigned bits = 0;
        for (unsigned long long m =
                 std::strtoull(sites[i][7].c_str(), nullptr, 10);
             m != 0; m &= m - 1)
            ++bits;
        if (bits != unsigned(std::atoi(sites[i][6].c_str())))
            mask_agrees = false;
    }
    check(mask_agrees,
          "la mascara de clases y su cuenta dicen lo mismo en cada sitio");

    /* EL REPARTO POR SITIO TIENE QUE SUMAR EL SITIO.
     *
     * Es el invariante que ata las dos tablas: si alguna vez dejan de cuadrar,
     * lo que falla es mudo -- un histograma plausible al lado de un total que
     * ya no es el suyo --.  Y ata tambien el desalojo: al echar una entrada su
     * reparto se reinicia igual que su mapa de clases, asi que la suma sigue
     * siendo lo que ESE sitio se gano (`allocs`), no lo que heredo. */
    const auto site_sizes = read_csv(dir + "/site_sizes.csv");
    check(site_sizes.size() > 1, "site_sizes.csv trae filas");
    /* LAS COLUMNAS, POR NOMBRE.  Estaban por posicion y ademas filtrando por
     * "la fila tiene exactamente cuatro campos", asi que anadir una columna al
     * fichero no rompia el test: lo dejaba sin encontrar NI UNA fila, sumando
     * cero, y fallando por algo que no tenia nada que ver con el invariante que
     * mira.  La cabecera esta ahi; preguntarle cuesta lo mismo. */
    size_t col_site = 0, col_allocs = 0;
    for (size_t c = 0; c < site_sizes[0].size(); ++c) {
        if (site_sizes[0][c] == "site_id") col_site = c;
        if (site_sizes[0][c] == "allocs") col_allocs = c;
    }
    check(col_allocs != 0, "y site_sizes.csv dice cual es su columna `allocs`");
    bool hist_adds_up = true;
    for (size_t i = 1; i < sites.size(); ++i) {
        if (sites[i].size() < 2) continue;
        long long sum = 0;
        for (size_t k = 1; k < site_sizes.size(); ++k)
            if (site_sizes[k].size() > col_allocs &&
                site_sizes[k][col_site] == sites[i][0])
                sum += std::atoll(site_sizes[k][col_allocs].c_str());
        if (sum != std::atoll(sites[i][1].c_str())) hist_adds_up = false;
    }
    check(hist_adds_up,
          "y el reparto de tamanos de cada sitio SUMA sus reservas");
    check(module_ok, "el modulo viaja en su columna, entrecomillado si toca");
    check(module_clean,
          "y lo que el resolutor no rellena sale VACIO, no con lo del sitio "
          "anterior");

    // -- lo que no se puede escribir se DICE --------------------------------
    check(!util::write_alloc_csv(nullptr), "sin carpeta no se exporta");
    check(!util::write_alloc_csv(""), "y una vacia tampoco");

    std::printf("%s\n", g_failures == 0 ? "TODO OK" : "HAY FALLOS");
    return g_failures == 0 ? 0 : 1;
}
