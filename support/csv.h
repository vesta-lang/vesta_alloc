/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file support/csv.h
 * @brief Vuelca lo medido a CSV, aparte de imprimirlo.
 *
 * PARA QUE, teniendo ya la tabla y los graficos.  Porque los dos responden a
 * "que pasa AHORA", y la pregunta que no saben contestar es "que ha cambiado
 * desde la vez anterior".  Eso no se puede leer: hay que restar, y para restar
 * hacen falta los numeros en un fichero.
 *
 * Y hay una razon concreta detras: este banco existe porque dos corridas
 * seguidas daban ordenaciones distintas.  Sin las corridas guardadas, cada
 * discusion sobre si algo mejoro o si es ruido empieza de cero y se resuelve de
 * memoria; con ellas, se resuelve mirando.
 *
 * SON TRES FICHEROS, NO UNO, y la razon es que hay tres cosas que cambian a
 * ritmos distintos:
 *
 *   `_cpu`   la maquina.  UNA fila.  No cambia en toda la tanda.
 *   `_runs`  las pasadas.  Una fila por pasada -- en que nucleos corrio, que
 *            suelo de margen se midio en ella.
 *   `_data`  las medidas.  Una fila por fila de tabla, y nada mas.
 *
 * Meterlo todo en uno parecia mas comodo y no lo era: la descripcion de la CPU
 * son quince columnas que se repetian identicas en ciento cincuenta filas,
 * asi que al abrir el fichero lo primero que se veia era justo lo que no habia
 * que mirar, y las medidas quedaban empujadas fuera de la pantalla.  Un fichero
 * de medidas tiene que poder leerse; si hay que recortar columnas para verlo,
 * las columnas sobraban.
 *
 * SE UNEN POR `run_id`, que es el momento de la tanda y esta en los tres.  Asi
 * se pueden concatenar los de muchas tandas -- `cat *_data.csv` -- sin que se
 * mezclen, y se sigue pudiendo recuperar de que maquina salio cada una.
 *
 * EL NOMBRE lleva el banco, el sistema, la microarquitectura y el momento --
 * `memcpy_windows_raptorlake-avx2-erms_...` -- porque un fichero de medidas sin
 * saber DONDE ni CUANDO se tomo no sirve para comparar: los numeros de dos
 * maquinas distintas puestos uno al lado del otro no dicen nada, y peor aun,
 * parecen decir algo.  El momento lleva la HORA y no solo el dia porque dos
 * tandas del mismo dia son justo el caso que se quiere comparar -- antes y
 * despues de un cambio -- y con el dia solo, la segunda pisaria a la primera.
 */
#ifndef VESTA_ALLOC_SUPPORT_CSV_H
#define VESTA_ALLOC_SUPPORT_CSV_H

#include "isa.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

namespace csv {

/// En que sistema se tomo la medida.  Va en el nombre del fichero y tambien en
/// una columna: el nombre se pierde en cuanto alguien renombra o concatena, y
/// entonces el dato tiene que seguir dentro.
inline const char *platform() {
#if defined(_WIN32)
    return "windows";
#elif defined(__linux__)
    return "linux";
#elif defined(__APPLE__)
    return "macos";
#else
    return "posix";
#endif
}

/// Con que compilador y version, que cambia los resultados mas que casi
/// cualquier otra cosa de las que se apuntan aqui.
inline std::string toolchain() {
    char b[64];
#if defined(__clang__)
    std::snprintf(b, sizeof(b), "clang-%d.%d", __clang_major__,
                  __clang_minor__);
#elif defined(__GNUC__)
    std::snprintf(b, sizeof(b), "gcc-%d.%d", __GNUC__, __GNUC_MINOR__);
#elif defined(_MSC_VER)
    std::snprintf(b, sizeof(b), "msvc-%d", _MSC_VER);
#else
    std::snprintf(b, sizeof(b), "unknown");
#endif
    return std::string(b);
}

/// Momento de la corrida, en un formato que ORDENA bien alfabeticamente: puesto
/// en el nombre, un `ls` deja las tandas en orden cronologico sin que nadie
/// tenga que pedirlo.
inline std::string stamp(const char *fmt) {
    std::time_t now = std::time(nullptr);
    std::tm tm_buf;
#if defined(_WIN32)
    localtime_s(&tm_buf, &now);
#else
    localtime_r(&now, &tm_buf);
#endif
    char b[64];
    std::strftime(b, sizeof(b), fmt, &tm_buf);
    return std::string(b);
}

/**
 * @brief Los tres ficheros de una tanda.
 *
 * Se abren en el constructor y se cierran en el destructor: si el banco se
 * interrumpe a media tabla, lo escrito hasta ahi sigue siendo un CSV valido con
 * menos filas, que es mucho mas util que un fichero a medio formar.
 */
class Report {
  public:
    /// Las columnas de `_data` de los bancos de memoria, que fueron los
    /// primeros en escribir CSV y de los que sale el formato.
    static const char *default_columns() {
        return "run_id,pass,section,label,bytes,"
               "c_ns,cpp_ns,libc_ns,gain,vs_libc,"
               "ratio_spread,time_spread,gain_verdict,libc_verdict";
    }

    /**
     * @param bench Nombre corto de la tanda, que encabeza los ficheros.
     */
    explicit Report(const char *bench) : Report(bench, default_columns()) {}

    /**
     * @brief Como el anterior, pero eligiendo las columnas de `_data`.
     *
     * POR QUE HACE FALTA.  Las columnas de arriba describen una tabla de TRES
     * implementaciones de la misma operacion, que es la forma de los bancos de
     * copiar y rellenar.  El del asignador compara DOS cosas y tiene columnas
     * que aquellos no tienen, asi que reutilizar su cabecera obligaria a
     * escribir un tiempo del asignador bajo un titulo que dice `libc_ns`.  Un
     * CSV cuyas columnas mienten es peor que no tenerlo: el numero se lee bien
     * y se atribuye mal.
     *
     * Las dos primeras columnas las pone esta clase (`run_id` y `pass`), asi
     * que la cadena tiene que empezar por ellas; es lo que permite concatenar
     * tandas y que sigan distinguiendose.
     *
     * @param bench   Nombre corto de la tanda.
     * @param columns Cabecera COMPLETA de `_data`, empezando por
     *                `run_id,pass`.
     */
    Report(const char *bench, const char *columns) : bench_(bench) {
        /* Se puede desviar a otro directorio sin tocar el codigo: en una
         * maquina de integracion los resultados no van al directorio de
         * trabajo. */
        const char *dir = std::getenv("VESTA_BENCH_CSV_DIR");

        run_id_ = stamp("%Y%m%d-%H%M%S");
        taken_ = stamp("%Y-%m-%dT%H:%M:%S");
        prefix_ = (dir != nullptr && dir[0] != '\0') ? std::string(dir) + "/"
                                                     : "";
        /* Banco, sistema, compilador, microarquitectura y momento, en ese
         * orden.  EL COMPILADOR VA EN EL NOMBRE porque es, junto con la
         * maquina, lo que mas mueve estos numeros -- la misma fuente da tablas
         * distintas con GCC y con Clang --, y el nombre es lo que se mira al
         * elegir que dos tandas comparar.  Estaba dentro, en una columna, y ahi
         * llegaba tarde: para cuando se abre el fichero, ya se ha decidido
         * abrirlo. */
        prefix_ += std::string(bench) + "_" + platform() + "_" + toolchain() +
                   "_" + isa::tag() + "_" + run_id_;

        write_cpu();

        runs_ = std::fopen((prefix_ + "_runs.csv").c_str(), "w");
        if (runs_ != nullptr)
            std::fprintf(runs_, "run_id,pass,core_seen,margin_floor,"
                                "started_at\n");

        data_ = std::fopen((prefix_ + "_data.csv").c_str(), "w");
        if (data_ != nullptr) std::fprintf(data_, "%s\n", columns);
    }

    ~Report() {
        if (runs_ != nullptr) std::fclose(runs_);
        if (data_ != nullptr) std::fclose(data_);
    }

    Report(const Report &) = delete;
    Report &operator=(const Report &) = delete;

    bool open() const { return data_ != nullptr; }
    const std::string &prefix() const { return prefix_; }

    /**
     * @brief Empieza una pasada y deja su fila en `_runs`.
     *
     * @param name      Como se llama la pasada ("P-cores", "all cores"...).
     * @param core_seen En que clase de nucleo se COMPROBO que estaba, que no es
     *                  lo mismo que lo que se pidio.
     * @param floor     El suelo del margen medido en esta pasada.  Cambia entre
     *                  clases de nucleo, asi que va aqui y no en la ficha de la
     *                  maquina.
     */
    void begin_pass(const char *name, const char *core_seen, double floor) {
        pass_ = name;
        if (runs_ == nullptr) return;
        std::fprintf(runs_, "\"%s\",\"%s\",\"%s\",%.5f,\"%s\"\n",
                     run_id_.c_str(), name, core_seen, floor,
                     stamp("%Y-%m-%dT%H:%M:%S").c_str());
        std::fflush(runs_);
    }

    /**
     * @brief Una fila de medida, con lo justo para rehacer su veredicto.
     *
     * Los campos de texto van entre comillas porque alguno lleva espacios
     * (`256 @32`) o parentesis (`natural (control)`), y porque una etiqueta
     * futura podria llevar una coma sin que nadie se acuerde de esto.
     */
    void row(const char *section, const char *label, double bytes, double c_ns,
             double cpp_ns, double libc_ns, double gain, double vs_libc,
             double ratio_spread, double time_spread, const char *gain_verdict,
             const char *libc_verdict) {
        if (data_ == nullptr) return;
        std::fprintf(data_,
                     "\"%s\",\"%s\",\"%s\",\"%s\",%.0f,"
                     "%.4f,%.4f,%.4f,%.5f,%.5f,"
                     "%.5f,%.5f,\"%s\",\"%s\"\n",
                     run_id_.c_str(), pass_.c_str(), section, label, bytes,
                     c_ns, cpp_ns, libc_ns, gain, vs_libc, ratio_spread,
                     time_spread, gain_verdict, libc_verdict);
    }

    /**
     * @brief Una fila cuyas columnas las eligio quien construyo el informe.
     *
     * Pone las dos que son de esta clase -- la tanda y la pasada -- y deja el
     * resto tal cual llega.  Va con el constructor de dos argumentos: quien
     * elige la cabecera es quien tiene que formar la fila que la cumple.
     *
     * @param rest Los campos que siguen a `run_id,pass`, ya formados y
     *             separados por comas.  El texto va entrecomillado por el que
     *             llama, que es quien sabe cual lleva espacios.
     *
     * @code
     * csv::Report out("alloc", "run_id,pass,size,ours_ns,system_ns");
     * char line[128];
     * std::snprintf(line, sizeof(line), "%zu,%.4f,%.4f", n, ours, sys);
     * out.raw(line);
     * @endcode
     */
    void raw(const char *rest) {
        if (data_ == nullptr) return;
        std::fprintf(data_, "\"%s\",\"%s\",%s\n", run_id_.c_str(),
                     pass_.c_str(), rest);
    }

  private:
    /// La ficha de la maquina: UNA fila, con cabecera, para que se puedan
    /// concatenar las de muchas tandas y salga una tabla de maquinas.
    void write_cpu() {
        std::FILE *f = std::fopen((prefix_ + "_cpu.csv").c_str(), "w");
        if (f == nullptr) return;
        std::fprintf(f, "run_id,bench,platform,uarch,isa,cpu_signature,"
                        "cpu_brand,threads,l1d_bytes,l2_bytes,l3_bytes,"
                        "cache_line,cpu_flags,toolchain,taken_at\n");
        std::fprintf(f,
                     "\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\","
                     "\"%s\",%u,%llu,%llu,%llu,"
                     "%u,\"%s\",\"%s\",\"%s\"\n",
                     run_id_.c_str(), bench_.c_str(), platform(),
                     isa::uarch().c_str(), isa::tag().c_str(),
                     isa::signature().c_str(), isa::brand().c_str(),
                     isa::threads(), isa::l1d(), isa::l2(), isa::l3(),
                     isa::cache_line(), isa::flags().c_str(),
                     toolchain().c_str(), taken_.c_str());
        std::fclose(f);
    }

    std::FILE *runs_ = nullptr;
    std::FILE *data_ = nullptr;
    std::string bench_, prefix_, run_id_, taken_, pass_;
};

} // namespace csv

#endif // VESTA_ALLOC_SUPPORT_CSV_H
