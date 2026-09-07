/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file bench/bench_memset.cpp
 * @brief Three ways to fill, side by side, on every single row.
 *
 * THE THREE COLUMNS, and why they are all here rather than in an appendix:
 *
 *   C      `vesta_memset(d, v, N)` -- the byte-counting interface.  Knows the
 *          length, knows nothing about the alignment of the destination, so it
 *          emits the prologue that aligns it at run time.
 *   C++    `util::vesta_memfill(dst, v)` -- the typed one.  Gets both the
 *          length and the alignment from the type, so where the type is
 *          over-aligned the prologue is not emitted at all.
 *   libc   the system's `memset`, as the reference.
 *
 * WHY THIS IS A SEPARATE BENCHMARK from the copy, and not one table with both:
 * they do not share a bottleneck.  A copy reads and writes, so it is limited by
 * the load unit and by two streams competing for cache.  A fill only WRITES,
 * which on a modern core is a different limit entirely -- and at large sizes it
 * runs into write-allocate: the CPU reads a line it is about to overwrite whole.
 *
 * Everything else -- why the size is a template parameter, what each row
 * covers, why the sizes include non-multiples, and the A-B-C-C-B-A discipline
 * -- is the same as in `bench_memcpy.cpp`, which explains it at length.
 *
 * ZEROING IS KEPT APART from filling with another byte: zeroing is the
 * overwhelmingly common case -- it is what `calloc` does -- and it is the one a
 * C library is most likely to have a special path for.
 */

#include "util/mem/vesta_memset.h"

#include "affinity.h"
#include "chart.h"
#include "csv.h"
#include "report.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

/// Hace opaco un puntero para el optimizador, sin emitir ni una instruccion.
/// Sin esto, el compilador VE que el relleno no se usa y lo borra.
template <class T> [[gnu::always_inline]] inline void escape(T &p) noexcept {
    asm volatile("" : "+r"(p) : : "memory");
}

/// Cual de las tres se esta midiendo.  Llega como parametro de plantilla para
/// que la rama desaparezca al compilar y no se mida el `if`.
enum Impl { kC = 0, kCpp = 1, kLibc = 2 };

/**
 * @brief Un bloque de @p N bytes con la alineacion @p A declarada.
 *
 * @p A es lo que la interfaz con tipo lee con @c alignof, asi que declarar 32
 * cuando la direccion NO lo esta seria mentirle al codigo y romperlo.  Por eso
 * los bufers se colocan a mano en una direccion que cumpla lo prometido.
 */
template <size_t N, size_t A> struct alignas(A) Block {
    uint8_t b[N];
};

/// Los tres numeros de una fila, en nanosegundos por operacion.
/**
 * @brief Los tres numeros de una fila, y lo que se mueven.
 *
 * @c noise es la dispersion RELATIVA de las muestras de esa fila -- el rango
 * entre cuartiles dividido por la mediana, tomando el peor de los tres --.  Es
 * lo que decide si la fila puede declarar un ganador; ver @c row.
 */
struct Trio {
    double c_api, cpp_api, libc, noise;

    /**
     * Las dos proporciones, y NO son el cociente de los tres numeros de arriba.
     *
     * Se calculan VUELTA A VUELTA y se resumen despues, que es lo que
     * corresponde a como esta medido esto: las tres implementaciones corren
     * intercaladas dentro de la misma vuelta, sobre las mismas direcciones y a
     * los mismos segundos de reloj.  Si la maquina se frena en la vuelta siete,
     * las tres medidas de esa vuelta se frenan JUNTAS y su cociente no se
     * entera; en cambio el cociente de dos resumenes si arrastra ese frenazo,
     * porque cada columna se lo lleva repartido de otra forma.
     *
     * Es la diferencia entre comparar dos cosas y comparar dos medidas de dos
     * cosas.  Y se nota justo donde importa: las filas que quedan al borde del
     * margen, que eran las que cambiaban de veredicto entre dos corridas.
     *
     * El precio es que la tabla no cuadra a mano -- 2,30 entre 1,69 no da
     * exactamente el 1,40x de al lado --, y es el precio correcto: la columna
     * de la proporcion es una medida por derecho propio, no una division.
     */
    double gain, vs_libc;
    /// Dispersion de las PROPORCIONES, que es la que decide el veredicto.  La
    /// de los tiempos (`noise`) dice si la maquina estuvo tranquila; esta dice
    /// si la comparacion se sostiene, que no es lo mismo.
    double ratio_noise;
};

/**
 * @brief Devuelve el mismo puntero, pero el compilador no puede seguirle la
 *        pista.
 *
 * SIN ESTO EL BANCO NO MIDE LO QUE DICE MEDIR, y costo encontrarlo.  El
 * puntero sale de un `(p + 31) & ~31` que esta a la vista, asi que el
 * compilador DEMUESTRA que esta alineado a 32 y se lo regala tambien a la
 * version de C: su prologo de alineacion se pliega y desaparece.  Resultado:
 * las dos columnas salian iguales y parecia que el tipo no aportaba nada.
 *
 * Medido con el puntero opaco -- que es como llega en la vida real, desde un
 * asignador o desde un campo de una estructura --, un relleno de 250 bytes
 * cuesta 7,2 ns por la interfaz de C y 1,7 sabiendo la alineacion.  Ese 4x es
 * lo que el banco estaba escondiendo.
 *
 * La version con tipo sigue sabiendolo aunque el puntero sea opaco, porque no
 * lo deduce del calculo: se lo dice `alignof(T)`.  Esa asimetria es justo lo
 * que se quiere medir.
 */
[[gnu::noinline]] uint8_t *opaque(uint8_t *p) { return p; }

/// @brief Reserva un bufer y devuelve un puntero alineado a @p A mas @p off.
uint8_t *aligned_at(std::vector<uint8_t> &store, size_t bytes, size_t align,
                    size_t off) {
    store.assign(bytes + align + off + 64, 0);
    uintptr_t p = reinterpret_cast<uintptr_t>(store.data());
    p = (p + align - 1) & ~uintptr_t(align - 1);
    /* Opaco a proposito: ver `opaque`.  Si no, el compilador deduce de este
     * mismo calculo que el puntero esta alineado y el banco deja de medir lo
     * que aporta el tipo. */
    return opaque(reinterpret_cast<uint8_t *>(p) + off);
}

/**
 * @brief El relleno del SISTEMA, alcanzado por un puntero que el compilador no
 *        puede seguir.
 *
 * Este es el UNICO sitio de todo el arbol donde se llama al de la libreria de
 * C, y esta aqui porque es la referencia contra la que se mide: sin ella la
 * tabla compararia nuestras dos interfaces entre si y no diria si alguna de las
 * dos vale la pena.
 *
 * Y VA POR UN PUNTERO OPACO POR NECESIDAD, no por gusto.  Escrita a las claras
 * y con la longitud constante, esa llamada no mide la libreria del sistema: el
 * compilador RECONOCE `memset` como funcion suya y la sustituye por codigo
 * puesto en el sitio, asi que la columna "libc" acababa midiendo al compilador
 * -- y encima con una ventaja que ninguna de las otras dos tiene, porque a
 * ellas no las reconoce --.  Escondido el destino detras de la misma barrera
 * vacia que se usa para los punteros de los bufers, no le queda mas remedio que
 * llamar, que es lo que hace un programa de verdad.
 */
using FillFn = void *(*)(void *, int, size_t);
FillFn system_fill() noexcept {
    static FillFn f = [] {
        FillFn g = std::memset;
        escape(g);
        return g;
    }();
    return f;
}

/// @brief Ejecuta la implementacion @p W sobre @p N bytes constantes.
template <size_t N, size_t A, int W>
[[gnu::always_inline]] inline void one(uint8_t *d, uint8_t v) noexcept {
    if (W == kC) {
        vesta_memset(d, v, N);
    } else if (W == kCpp) {
        util::vesta_memfill(reinterpret_cast<Block<N, A> *>(d), v);
    } else {
        system_fill()(d, v, N);
    }
}

// ---------------------------------------------------------------------------
//  Patrones
// ---------------------------------------------------------------------------

/**
 * @brief Rellena bloques que ROTAN, todos residentes en cache.
 *
 * POR QUE ROTAN Y NO ES SIEMPRE EL MISMO.  Con un solo destino, el puntero es
 * invariante del bucle, y entonces el compilador saca la decision de
 * alineacion FUERA y especializa el cuerpo: la version de C deja de pagar el
 * prologo y el banco no puede medir lo que aporta el tipo.  Costo encontrarlo
 * -- ver `opaque`, que era el primer intento y no bastaba, porque el problema
 * no es solo que el puntero se pueda seguir sino que no cambia.
 *
 * Rotando entre varios, el compilador no puede sacar nada fuera, y ademas es
 * la forma que tiene el caso real: un asignador no rellena mil veces el mismo
 * bloque, rellena el que le acaba de tocar.
 *
 * Los bufers caben de sobra en cache, asi que lo que se mide sigue siendo la
 * rutina y no la memoria; para eso esta `scattered`.
 */
template <size_t N, size_t A, size_t Off, int W>
double hot(uint8_t v, int rounds) {
    /* Tantos como quepan comodos en unos 256 KiB, y al menos dos. */
    constexpr size_t kWant = (256u << 10) / (N + A + Off + 64);
    /* Potencia de dos, para que elegir uno sea un `and` y no una division. */
    constexpr size_t kBufs = kWant >= 8 ? 8 : (kWant >= 4 ? 4 : 2);

    std::vector<uint8_t> store[kBufs];
    uint8_t *d[kBufs];
    for (size_t k = 0; k < kBufs; ++k) {
        d[k] = aligned_at(store[k], N, A, Off);
        one<N, A, W>(d[k], v); // calienta y decide el despacho
    }

    uint64_t sink = 0;
    const auto t0 = Clock::now();
    for (int i = 0; i < rounds; ++i) {
        uint8_t *dd = d[size_t(i) & (kBufs - 1)];
        escape(dd);
        one<N, A, W>(dd, v);
        sink += dd[0]; // leer el destino: el relleno NO es codigo muerto
    }
    const auto dt = Clock::now() - t0;

    escape(sink);
    return double(std::chrono::duration_cast<std::chrono::nanoseconds>(dt)
                      .count()) /
           double(rounds);
}

/**
 * @brief Rellenos repartidos por una region de 16 MiB.
 *
 * El caso realista para un asignador: el bloque que hay que zerificar NO esta
 * en cache, porque acaba de salir de una lista de libres donde llevaba un rato.
 * Es exactamente lo que hace `host_alloc_zeroed` al reciclar.
 */
template <size_t N, size_t A, size_t Off, int W>
double scattered(uint8_t v, int rounds) {
    constexpr size_t kSpan = 16u << 20;
    std::vector<uint8_t> store;
    uint8_t *base = aligned_at(store, kSpan, A, Off);

    uint64_t seed = 0x9E3779B97F4A7C15ull;
    uint64_t sink = 0;
    const size_t reach = kSpan - N - 64;

    const auto t0 = Clock::now();
    for (int i = 0; i < rounds; ++i) {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        // Redondeada a la alineacion que el tipo promete; las tres columnas
        // usan las MISMAS direcciones.
        uint8_t *d = base + ((size_t(seed >> 33) % reach) & ~(A - 1));
        escape(d);
        one<N, A, W>(d, v);
        sink += d[0];
    }
    const auto dt = Clock::now() - t0;

    escape(sink);
    return double(std::chrono::duration_cast<std::chrono::nanoseconds>(dt)
                      .count()) /
           double(rounds);
}

// ---------------------------------------------------------------------------
//  Medida y presentacion
// ---------------------------------------------------------------------------

/**
 * @brief Cuantas veces se mide cada punto.
 *
 * Veinticinco, no cinco.  Con pocas, el veredicto se lo juega al ruido y dos
 * corridas del banco dan ordenaciones distintas -- que es exactamente el
 * sintoma de estar midiendo la maquina en vez del codigo.
 *
 * Y es la MITAD del remedio, no todo.  De aqui salen doce muestras limpias que
 * `summarize` promedia; la otra mitad es no creerselas a ciegas, midiendo
 * ademas cuanto se movieron (el ruido) y exigiendo esa distancia como margen
 * antes de declarar un ganador.  Repetir mas sin lo segundo solo da un numero
 * mas fino de una maquina que sigue haciendo otras cosas.
 */
constexpr int kReps = 25;

/// Ordena por insercion.  Son unas pocas decenas de elementos.
void sort_samples(double *v, int n) {
    for (int i = 1; i < n; ++i) {
        const double key = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > key) {
            v[j + 1] = v[j];
            --j;
        }
        v[j + 1] = key;
    }
}

/**
 * @brief Resume las muestras de una implementacion: MEDIA DE LA MITAD LIMPIA,
 *        y el ruido aparte.
 *
 * Las dos cosas a la vez, porque cada una arregla lo que la otra no.
 *
 * DE DONDE SALE LO DE "LA MITAD LIMPIA".  El ruido de una medida de tiempo solo
 * puede SUMAR -- un cambio de contexto, una interrupcion, una bajada de
 * frecuencia --, nunca restar.  Asi que al ordenar, las muestras BAJAS son las
 * limpias y las altas son ellas mismas mas contaminacion.  Quedarse con la
 * mitad de abajo es tirar la contaminacion sin tener que decidir un umbral.
 *
 * Y POR QUE LA MEDIA DE ESA MITAD Y NO EL MINIMO.  Porque el minimo, aun siendo
 * la muestra menos contaminada, es UNA SOLA: tiene poco sesgo pero mucha
 * varianza de muestreo, y por eso dos corridas seguidas pueden ordenar
 * distinto.  Promediar las que quedan reduce esa varianza sin volver a meter lo
 * que ya se ha tirado.
 *
 * EL RUIDO se mide aparte, con lo que se ha descartado: cuanto se separa la
 * mediana del minimo.  Si son casi iguales, la maquina estuvo tranquila y el
 * veredicto de esa fila se puede creer; si se separan, no.  Con eso se hace el
 * margen, en `row`.
 *
 * @param v         Muestras; se ORDENAN en el sitio.
 * @param n         Cuantas.
 * @param out_noise Donde dejar cuanto se aparta la mediana del minimo.
 * @return La media de la mitad baja.
 */
double summarize(double *v, int n, double *out_noise) {
    sort_samples(v, n);

    const double best = v[0];
    const double med = v[n / 2];
    *out_noise = best > 0.0 ? (med - best) / best : 0.0;

    const int keep = n / 2 > 0 ? n / 2 : 1;
    double sum = 0.0;
    for (int i = 0; i < keep; ++i)
        sum += v[i];
    return sum / double(keep);
}

/// Resume las tres y se queda con la PEOR dispersion de las tres: si cualquiera
/// se movio, la fila no esta en condiciones de declarar nada -- aunque las otras
/// dos hayan salido limpias, porque el veredicto las COMPARA.
/**
 * @brief Resume una serie de PROPORCIONES, que no se resume igual que un tiempo.
 *
 * POR QUE NO VALE `summarize` AQUI.  Aquella se queda con la mitad BAJA, y
 * puede hacerlo porque el ruido de un tiempo solo suma: las muestras bajas son
 * las limpias.  Una proporcion no tiene esa propiedad -- el ruido puede caer en
 * el numerador, y sube, o en el denominador, y baja --, asi que quedarse con la
 * mitad baja no seria limpiar, seria SESGAR hacia el lado que nos perjudica o
 * nos favorece segun la fila.
 *
 * Lo que si vale es recortar por LOS DOS EXTREMOS: se tira el cuarto de arriba
 * y el cuarto de abajo y se promedia el medio.  Simetrico, robusto, y sigue
 * usando la mitad de las muestras en vez de una sola como haria la mediana.
 *
 * @param v         Muestras; se ORDENAN en el sitio.
 * @param n         Cuantas.
 * @param out_noise Donde dejar la dispersion, medida como lo que separa a los
 *                  dos cuartiles dividido por el centro.
 * @return La media del medio.
 */
double summarize_ratio(double *v, int n, double *out_noise) {
    sort_samples(v, n);

    const int q1 = n / 4;
    const int q3 = n - 1 - n / 4;
    const double mid = v[n / 2];
    *out_noise = mid > 0.0 ? (v[q3] - v[q1]) / mid : 0.0;

    double sum = 0.0;
    int cnt = 0;
    for (int i = q1; i <= q3; ++i) {
        sum += v[i];
        ++cnt;
    }
    return cnt > 0 ? sum / double(cnt) : mid;
}

Trio summarize_all(double *c, double *p, double *l) {
    Trio t;

    /* Las proporciones PAREADAS, calculadas antes de que `summarize` ordene los
     * arrays y pierda la correspondencia entre vueltas.  Ver `Trio::gain`. */
    double g[kReps], v[kReps];
    for (int r = 0; r < kReps; ++r) {
        g[r] = p[r] > 0.0 ? c[r] / p[r] : 0.0;
        const double best = (p[r] > 0.0 && p[r] < c[r]) ? p[r] : c[r];
        v[r] = best > 0.0 ? l[r] / best : 0.0;
    }
    double ng, nv;
    t.gain = summarize_ratio(g, kReps, &ng);
    t.vs_libc = summarize_ratio(v, kReps, &nv);
    t.ratio_noise = ng > nv ? ng : nv;

    double nc, np, nl;
    t.c_api = summarize(c, kReps, &nc);
    t.cpp_api = summarize(p, kReps, &np);
    t.libc = summarize(l, kReps, &nl);
    t.noise = nc > np ? nc : np;
    if (nl > t.noise) t.noise = nl;
    return t;
}

/**
 * @brief Mide las tres @c kReps veces, alternando el orden, y las resume.
 *
 * El orden se invierte en las vueltas impares para que ninguna implementacion
 * quede siempre la primera ni siempre la ultima: si la maquina se calienta o se
 * frena durante la medida, el efecto se reparte entre las tres.
 */
template <size_t N, size_t A, size_t Off>
Trio hot_trio(uint8_t v, int rounds) {
    double c[kReps], p[kReps], l[kReps];
    for (int r = 0; r < kReps; ++r) {
        if (r & 1) {
            l[r] = hot<N, A, Off, kLibc>(v, rounds);
            p[r] = hot<N, A, Off, kCpp>(v, rounds);
            c[r] = hot<N, A, Off, kC>(v, rounds);
        } else {
            c[r] = hot<N, A, Off, kC>(v, rounds);
            p[r] = hot<N, A, Off, kCpp>(v, rounds);
            l[r] = hot<N, A, Off, kLibc>(v, rounds);
        }
    }
    return summarize_all(c, p, l);
}

/// @copydoc hot_trio
template <size_t N, size_t A, size_t Off>
Trio scattered_trio(uint8_t v, int rounds) {
    double c[kReps], p[kReps], l[kReps];
    for (int r = 0; r < kReps; ++r) {
        if (r & 1) {
            l[r] = scattered<N, A, Off, kLibc>(v, rounds);
            p[r] = scattered<N, A, Off, kCpp>(v, rounds);
            c[r] = scattered<N, A, Off, kC>(v, rounds);
        } else {
            c[r] = scattered<N, A, Off, kC>(v, rounds);
            p[r] = scattered<N, A, Off, kCpp>(v, rounds);
            l[r] = scattered<N, A, Off, kLibc>(v, rounds);
        }
    }
    return summarize_all(c, p, l);
}

/**
 * @brief Lo que hace falta para dibujar una seccion, guardado mientras se mide.
 *
 * Las proporciones y los tiempos se apuntan segun salen, pero los graficos se
 * dibujan TODOS AL FINAL y no debajo de su tabla.  La razon es que asi se
 * pueden comparar entre si: la pregunta interesante casi nunca es como se
 * comporta una seccion, es en que se diferencia de la de al lado -- que le pasa
 * a lo mismo cuando la alineacion es 64 en vez de 32, o cuando el bloque no
 * esta en cache --.  Con cada dibujo pegado a su tabla, esa comparacion exige
 * recordar la silueta anterior mientras se leen cuarenta numeros por medio.
 */
struct Plot {
    const char *name = "";
    /// En que pasada se midio.  En un procesador hibrido la misma seccion se
    /// mide una vez por clase de nucleo, y sin esto los dibujos se
    /// multiplicarian sin decir cual es cual.
    const char *pass = "";
    std::vector<double> gain;    ///< que aporta el tipo
    std::vector<double> vs_libc; ///< donde estamos frente al sistema
    /* Y los tiempos crudos, que responden a otra pregunta: no quien gana, sino
     * COMO ESCALA cada uno.  Una proporcion de 1,00x no distingue entre las
     * tres yendo rapido y las tres yendo mal. */
    std::vector<double> t_c, t_cpp, t_libc; ///< ns por operacion
    std::vector<double> bytes;              ///< cuantos bytes lleno cada fila
    std::string first, last;                ///< extremos del eje horizontal
};

/// Las secciones ya medidas, en el orden en que se imprimieron.
std::vector<Plot> g_plots;

/**
 * @brief Suelo del margen del veredicto.  Se MIDE al arrancar; ver `calibrate`.
 *
 * Arranca en el 2% para que siga valiendo algo si la calibracion no corre.
 */
double g_floor = 0.02;

/// El CSV de la tanda, si se pudo abrir.  Lo alimenta `row`, que es el unico
/// sitio donde ya estan calculadas las dos proporciones y los dos veredictos:
/// escribirlo desde otro lado obligaria a repetir ese calculo, y entonces el
/// fichero y la tabla podrian llegar a decir cosas distintas.
csv::Report *g_csv = nullptr;

/// La pasada que se esta midiendo.  Ver `Plot::pass`.
const char *g_pass = "";

/// Como le fue a cada pasada.  Se guarda para poder elegir DESPUES cual de
/// ellas manda -- ver `pass_summary` --, que no se puede saber hasta haberlas
/// medido todas.
struct PassInfo {
    const char *name;
    const char *core;
    double floor;
};
std::vector<PassInfo> g_passes;

void size_name(char *out, size_t cap, size_t n) {
    if (n >= (1u << 20))
        std::snprintf(out, cap, "%zuM", n >> 20);
    else if (n >= 1024 && (n % 1024) == 0)
        std::snprintf(out, cap, "%zuK", n >> 10);
    else
        std::snprintf(out, cap, "%zu", n);
}

/**
 * @brief Imprime una fila.
 *
 * @param name Que varia en esta tabla.  Casi siempre el tamano, pero las tablas
 *             de barrido varian la alineacion o el desplazamiento y el tamano
 *             se queda fijo, asi que la etiqueta se pasa aparte.
 * @param n    Bytes escritos, que NO se deduce de la etiqueta y hace falta para
 *             el rendimiento del grafico.
 * @param t    Las tres medidas.
 */
void row(const char *name, size_t n, Trio t) {
    /* Dos proporciones, porque son dos preguntas distintas: que aporta el tipo
     * (C contra C++) y donde estamos frente al sistema.  Vienen MEDIDAS vuelta
     * a vuelta, no divididas aqui; el por que esta en `Trio::gain`. */
    const double gain = t.gain;
    const double vs_libc = t.vs_libc;

    /* Se dice CUAL gana y no solo la proporcion, porque una proporcion de 0,97x
     * se lee como "casi igual" cuando lo que dice es que se PIERDE.  Un banco
     * que solo publica proporciones deja al lector la parte incomoda.
     *
     * Y hay TRES veredictos, no dos, en las dos comparaciones.  Sin el empate,
     * un 1,01x se lee como victoria y un 0,99x como derrota cuando ninguno de
     * los dos dice nada.
     *
     * EL UMBRAL SALE DE LA PROPIA FILA, no de un numero fijo.  Un 5% escrito a
     * mano vale para unas filas y miente en otras: en los tamanos pequenos la
     * dispersion medida pasa del 6%, asi que ahi un "gana por un 5%" es ruido
     * con nombre.  Aqui el margen es la dispersion observada de esa fila, con
     * un suelo del 2%, que es lo mejor que este reloj distingue.  Una fila que
     * se mueve mucho NO puede declarar ganador.
     *
     * Y la dispersion que se usa es la de LAS PROPORCIONES, no la de los
     * tiempos.  Son cosas distintas: los tiempos se mueven en cuanto la maquina
     * hace otra cosa, mientras que la proporcion entre dos medidas tomadas en
     * la misma vuelta apenas se entera de eso.  Con la de los tiempos, una fila
     * perfectamente decidida se quedaba sin veredicto solo porque el momento de
     * medirla fue movido.
     *
     * Y el SUELO tampoco es un numero elegido: sale medido del caso de control
     * al arrancar.  Ver `calibrate`. */
    const double margin = t.ratio_noise > g_floor ? t.ratio_noise : g_floor;

    /* El color no es adorno: una tabla de cuarenta filas se recorre con la
     * vista buscando lo que se sale, y en gris se lee entera o no se lee. */
    const char *verdict = "tie";
    const char *vc = report::dim();
    if (gain > 1.0 + margin) {
        verdict = "C++ wins";
        vc = report::green();
    } else if (gain > 0.0 && gain < 1.0 - margin) {
        verdict = "C++ LOSES";
        vc = report::red();
    }

    const char *against = "tie";
    const char *ac = report::dim();
    if (vs_libc > 1.0 + margin) {
        against = "we win";
        ac = report::green();
    } else if (vs_libc > 0.0 && vs_libc < 1.0 - margin) {
        against = "libc wins";
        ac = report::red();
    }

    /* Se publica la dispersion de LA COMPARACION, que es la que acaba de
     * decidir el veredicto; publicar la de los tiempos daria un numero que no
     * explica lo que se ve al lado.  En ambar por encima del 5%: ahi la fila no
     * puede decidir nada y conviene que se vea antes de leerle el veredicto. */
    const char *nc = t.ratio_noise > 0.05 ? report::amber() : report::dim();

    std::printf("  %-8s %10.2f %10.2f %10.2f %s%5.1f%%%s  %7.2fx %s%-10s%s "
                "%7.2fx %s%s%s\n",
                name, t.c_api, t.cpp_api, t.libc, nc, t.ratio_noise * 100.0,
                report::reset(), gain, vc, verdict, report::reset(), vs_libc,
                ac, against, report::reset());

    /* Las mismas proporciones y los mismos tiempos que acaban de decidir el
     * veredicto, guardados para el dibujo del final.  Se copian de aqui a
     * proposito: si el grafico los recalculara por su cuenta, podria acabar
     * diciendo algo distinto de la tabla que tiene encima. */
    if (g_csv != nullptr)
        g_csv->row(g_plots.empty() ? "" : g_plots.back().name, name,
                   double(n), t.c_api, t.cpp_api, t.libc, gain, vs_libc,
                   t.ratio_noise, t.noise, verdict, against);

    if (!g_plots.empty()) {
        Plot &p = g_plots.back();
        p.gain.push_back(gain);
        p.vs_libc.push_back(vs_libc);
        p.t_c.push_back(t.c_api);
        p.t_cpp.push_back(t.cpp_api);
        p.t_libc.push_back(t.libc);
        p.bytes.push_back(double(n));
        if (p.first.empty()) p.first = name;
        p.last = name;
    }
}

/// La forma corriente: lo que varia es el tamano, asi que la etiqueta ES el
/// tamano.
void row(size_t n, Trio t) {
    char name[32];
    size_name(name, sizeof(name), n);
    row(name, n, t);
}

void header(const char *name, const char *title) {
    g_plots.emplace_back();
    g_plots.back().name = name;
    g_plots.back().pass = g_pass;
    report::section(name, title);
    std::printf("  %s%-8s %10s %10s %10s %6s  %8s %-10s %8s %s%s\n",
                report::bold(), "size", "C", "C++", "libc", "noise", "C/C++",
                "verdict", "libc/us", "verdict", report::reset());
    std::printf("  %s-------- ---------- ---------- ---------- ------  -------- "
                "---------- -------- ---------%s\n",
                report::dim(), report::reset());
}

/// Bytes por nanosegundo de una serie de tiempos.  Es el mismo dato del reves,
/// pero responde a otra pregunta: el tiempo dice cuanto TARDA -- y siempre
/// tarda mas cuanto mas grande --, mientras el rendimiento dice a que VELOCIDAD
/// va, y ahi si se ve donde cada implementacion toca techo y donde se cae.
std::vector<double> throughput(const std::vector<double> &ns,
                               const std::vector<double> &bytes) {
    std::vector<double> out(ns.size(), 0.0);
    for (size_t i = 0; i < ns.size(); ++i)
        if (ns[i] > 0.0) out[i] = bytes[i] / ns[i];
    return out;
}

/// Alto y ancho de cada panel, en lineas y columnas de la terminal.
///
/// Diez lineas son CUARENTA filas de puntos, y hacen falta: con seis, las tres
/// curvas caian en las mismas celdas casi todo el recorrido -- van dentro de un
/// factor de dos, sobre un eje que cubre tres ordenes de magnitud -- y se veia
/// una sola linea.  El ancho, setenta y dos columnas, son ciento cuarenta y
/// cuatro puntos: con treinta y seis filas de tabla eso son cuatro puntos por
/// medida, suficiente para que un trazo discontinuo se distinga del continuo.
constexpr int kRows = 10;
constexpr int kCols = 72;

/**
 * @brief Un panel por seccion comparando las PASADAS entre si.
 *
 * Es la unica pregunta que los dibujos de cada seccion no pueden responder:
 * ellos comparan las tres implementaciones dentro de una pasada, y aqui se
 * compara la MISMA implementacion entre clases de nucleo.  Se dibuja la nuestra
 * con tipo, que es la que se esta afinando.
 *
 * En bytes por nanosegundo y no en tiempo, porque asi se ve de un vistazo
 * cuanto mas lento es un nucleo pequeno: dos curvas paralelas separadas por una
 * distancia constante son un factor constante.
 */
void core_plots() {
    /* Con una sola pasada esto compararia una cosa consigo misma. */
    std::vector<const char *> names;
    for (const Plot &p : g_plots) {
        bool seen = false;
        for (const char *n : names)
            if (std::strcmp(n, p.pass) == 0) seen = true;
        if (!seen) names.push_back(p.pass);
    }
    if (names.size() < 2) return;

    report::section("cores", "the same measurement on each kind of core");

    for (const Plot &first : g_plots) {
        if (std::strcmp(first.pass, names[0]) != 0 || first.gain.empty())
            continue;

        /* Los datos tienen que sobrevivir al dibujo, asi que se guardan aqui y
         * no dentro del bucle que construye las series. */
        std::vector<std::vector<double>> tp;
        std::vector<chart::Series> series;
        const char *kColors[3] = {report::cyan(), report::green(),
                                  report::amber()};

        for (size_t i = 0; i < names.size(); ++i) {
            for (const Plot &q : g_plots) {
                if (std::strcmp(q.pass, names[i]) != 0) continue;
                if (std::strcmp(q.name, first.name) != 0) continue;
                if (q.t_cpp.size() != first.t_cpp.size()) continue;
                tp.push_back(throughput(q.t_cpp, q.bytes));
                series.push_back({names[i], nullptr, kColors[i % 3]});
                break;
            }
        }
        /* Los punteros se ponen DESPUES de llenar `tp`: al crecer, el vector
         * puede mover sus elementos y dejar apuntando a memoria vieja. */
        if (series.size() != tp.size()) continue;
        for (size_t i = 0; i < series.size(); ++i)
            series[i].v = tp[i].data();

        std::printf("\n  %s%s%s %s-- C++ column, bytes per ns%s\n",
                    report::bold(), first.name, report::reset(), report::dim(),
                    report::reset());
        chart::plot("by core kind", series.data(), int(series.size()),
                    int(first.t_cpp.size()), kRows, kCols, 0.0,
                    first.first.c_str(), first.last.c_str());
    }
}

/**
 * @brief Compara las pasadas y dice de cual hay que leer el veredicto.
 *
 * QUE PASADA MANDA, Y POR QUE NO ES LA GENERAL.  La tentacion es quedarse con
 * la que no ata nada, por parecerse a lo que hace un programa de verdad.  Para
 * decir CUANTO TARDA algo, es la buena.  Para decir CUAL DE DOS
 * IMPLEMENTACIONES es mejor, es la peor de las tres: sin atar, el sistema mueve
 * el proceso entre nucleos de las dos clases a mitad de la medida, asi que cada
 * columna acaba siendo una mezcla de dos poblaciones muy distintas en una
 * proporcion que decide el planificador y que cambia de una corrida a la
 * siguiente.  Eso no es ruido que se pueda promediar: es medir dos cosas y
 * llamarlas una.
 *
 * Asi que manda la pasada con el SUELO mas bajo, que es la que esta maquina
 * demostro poder resolver mejor -- y el suelo esta medido, no supuesto: sale
 * del caso de control, donde las dos columnas son el mismo codigo.  Es el mismo
 * criterio que gobierna los veredictos, aplicado un nivel mas arriba.
 *
 * @return El nombre de la pasada que manda.
 */
const char *pass_summary() {
    if (g_passes.size() < 2) return g_passes.empty() ? "" : g_passes[0].name;

    report::section("passes", "the same tables, once per kind of core");

    std::printf("  %s%-12s %-10s %s%s\n", report::bold(), "pass", "core seen",
                "margin floor", report::reset());
    std::printf("  %s------------ ---------- ------------%s\n", report::dim(),
                report::reset());

    /* Menor suelo gana, y en caso de EMPATE gana la primera -- que son las
     * atadas, porque la general va la ultima --.  No es un desempate al azar:
     * si dos pasadas resuelven igual de bien, la que no mezcla clases de nucleo
     * es preferible por construccion, no por lo que haya salido hoy. */
    const PassInfo *best = &g_passes[0];
    for (const PassInfo &p : g_passes)
        if (p.floor < best->floor) best = &p;

    for (const PassInfo &p : g_passes) {
        const char *tint =
            (&p == best) ? report::green()
                         : (p.floor > 0.05 ? report::amber() : report::dim());
        std::printf("  %-12s %-10s %s%11.1f%%%s\n", p.name, p.core, tint,
                    p.floor * 100.0, report::reset());
    }

    std::printf("\n%sVerdicts and charts are read from %s%s%s%s, the pass this "
                "machine\nresolves best.  A pass that is not pinned mixes two "
                "kinds of core in\nwhatever proportion the scheduler chose, "
                "and that is not noise to be\naveraged away -- it is two "
                "different things measured as one.%s\n",
                report::dim(), report::reset(), report::bold(), best->name,
                report::dim(), report::reset());
    return best->name;
}

/**
 * @brief Todos los graficos, juntos y al final.
 *
 * Ver `Plot` para por que aqui y no debajo de cada tabla, y `pass_summary` para
 * por que se dibuja UNA pasada y cual.  Lo que si cambia entre pasadas tiene su
 * propio bloque, `core_plots`.
 */
void plots(const char *which) {
    report::section("charts", "the same numbers as above, as shapes");
    chart::legend();

    for (const Plot &p : g_plots) {
        if (p.gain.empty() || std::strcmp(p.pass, which) != 0) continue;
        const int n = int(p.gain.size());

        std::printf("\n  %s%s%s %s-- %d rows%s\n", report::bold(), p.name,
                    report::reset(), report::dim(), n, report::reset());

        /* COMO ESCALA cada una.  Es la pregunta que una proporcion no puede
         * responder: si las tres se degradan igual, la proporcion sigue plana
         * en 1,00x y no se entera nadie. */
        const chart::Series ts[3] = {
            {"C", p.t_c.data(), report::cyan()},
            {"C++", p.t_cpp.data(), report::green()},
            {"libc", p.t_libc.data(), report::amber()},
        };
        chart::plot("ns per fill", ts, 3, n, kRows, kCols, 0.0, p.first.c_str(),
                    p.last.c_str());

        /* Y a que VELOCIDAD, que es donde se ve el techo de cada camino y el
         * escalon de cada nivel de cache.  En un relleno importa mas que en una
         * copia: a partir de cierto tamano el limite deja de ser la rutina y
         * pasa a ser que la CPU LEE la linea que va a sobrescribir entera. */
        const std::vector<double> bc = throughput(p.t_c, p.bytes);
        const std::vector<double> bp = throughput(p.t_cpp, p.bytes);
        const std::vector<double> bl = throughput(p.t_libc, p.bytes);
        const chart::Series bs[3] = {
            {"C", bc.data(), report::cyan()},
            {"C++", bp.data(), report::green()},
            {"libc", bl.data(), report::amber()},
        };
        chart::plot("bytes per ns", bs, 3, n, kRows, kCols, 0.0,
                    p.first.c_str(), p.last.c_str());

        /* Y QUIEN gana, con la linea del empate dibujada: lo que queda por
         * encima se gana y lo que queda por debajo se pierde. */
        const chart::Series rs[2] = {
            {"C/C++", p.gain.data(), report::cyan()},
            {"libc/us", p.vs_libc.data(), report::amber()},
        };
        chart::plot("ratio, 1.00x dashed", rs, 2, n, kRows, kCols, 1.0,
                    p.first.c_str(), p.last.c_str());
    }
}

/// Menos vueltas segun crece el bloque, o los casos grandes se comen el tiempo
/// de pared sin anadir informacion.
constexpr int rounds_for(size_t n) {
    return n <= 256 ? 2000000
                    : (n <= 4096 ? 400000 : (n <= 65536 ? 40000 : 3000));
}

/**
 * @brief Averigua cuanto es capaz de resolver este banco, EN ESTA MAQUINA.
 *
 * DE DONDE SALE EL PROBLEMA.  El margen de cada fila salia de la dispersion de
 * sus propias muestras, y eso se queda corto por una razon de fondo: mide lo
 * que varia DENTRO de una corrida, cuando lo que hace flaquear un veredicto es
 * lo que varia ENTRE corridas -- otra disposicion de la memoria, otro estado de
 * la maquina, otro reparto de la cache --.  Con un margen sacado de la
 * dispersion interna, un puñado de filas cruzaba el umbral en una corrida y no
 * en la siguiente, que es exactamente el sintoma que habia que quitar.
 *
 * LA SOLUCION ES MEDIRLO, no elegir un porcentaje a ojo.  Y se puede, porque
 * este banco tiene un caso cuya respuesta VERDADERA se conoce de antemano: con
 * alineacion natural, la interfaz con tipo no promete nada, asi que las dos
 * columnas ejecutan el MISMO codigo y su proporcion real es exactamente 1,00x.
 * Todo lo que se aparte de ahi no es una diferencia, es lo que este montaje no
 * sabe distinguir.  Eso es el suelo, y sale medido en la maquina y el dia en
 * que se esta corriendo en vez de escrito en una constante que envejece.
 *
 * Se prueban varios tamanos porque el suelo no es el mismo en todos: en los
 * bloques pequenos el relleno dura pocos nanosegundos y el reloj pesa mas.  Se
 * toma el PEOR, que es la unica eleccion segura -- con el promedio, las filas
 * pequenas seguirian decidiendo por encima de sus posibilidades.
 */
void calibrate() {
    /* Se vuelve a empezar en cada pasada: el suelo es una propiedad de las
     * condiciones de medida, y un nucleo pequeno no resuelve lo mismo que uno
     * grande.  Sin reiniciar, el suelo solo podria crecer y la segunda pasada
     * heredaria el de la primera. */
    g_floor = 0.02;
    double worst = 0.0;

#define VESTA_CALIB(N)                                                         \
    {                                                                          \
        const Trio t = hot_trio<N, 1, 3>(0, rounds_for(N) / 4);                \
        if (t.gain > 0.0) {                                                    \
            const double d = t.gain > 1.0 ? t.gain - 1.0 : 1.0 - t.gain;       \
            if (d > worst) worst = d;                                          \
        }                                                                      \
    }
    VESTA_CALIB(33)
    VESTA_CALIB(100)
    VESTA_CALIB(250)
    VESTA_CALIB(1000)
    VESTA_CALIB(4095)
#undef VESTA_CALIB

    if (worst > g_floor) g_floor = worst;
    std::printf("\n%sVerdict margin floor, measured on the control case (where "
                "both sides\nrun the same code, so the true ratio is 1.00x): "
                "%s%.1f%%%s.  A row needs to\nbeat that, or its own spread if "
                "it is worse, before it names a winner.%s\n",
                report::dim(), report::bold(), g_floor * 100.0, report::dim(),
                report::reset());
}

/* El tamano tiene que ser una CONSTANTE porque la interfaz con tipo necesita un
 * tipo, asi que las listas se expanden con un macro en vez de recorrerse.
 *
 * SON DOS LISTAS, Y NO ES UN CAPRICHO.  Un tipo sobrealineado NO PUEDE tener un
 * tamano que no sea multiplo de su alineacion: `sizeof` se redondea hacia
 * arriba, asi que un `alignas(32)` de 50 bytes mide 64.  Meter tamanos no
 * multiplos en la seccion alineada hacia que las dos columnas rellenaran
 * cantidades DISTINTAS -- 50 bytes por C contra 64 por C++ -- y la comparacion
 * no media nada.  Costo verlo, y invalido una tabla entera.
 *
 * Asi que la seccion alineada lleva solo multiplos de 32, que es lo unico que
 * un tipo alineado a 32 puede medir, y los tamanos raros van a la seccion sin
 * alinear, donde el tipo mide exactamente lo que dice. */
/* Multiplos de 32, que es lo unico que puede medir un tipo alineado a 32.  Van
 * SEGUIDOS y no a saltos de potencia de dos: entre 128 y 256 hay tres cambios
 * de camino y mirar solo los extremos deja los bordes sin probar. */
#define VESTA_ALIGNED32_SIZES(X)                                               \
    X(32) X(64) X(96) X(128) X(160) X(192) X(224) X(256) X(288) X(320)         \
        X(384) X(448) X(512) X(576) X(768) X(1024) X(1536) X(2048) X(3072)     \
            X(4096) X(8192) X(16384) X(65536)

/* Multiplos de 64: lineas de cache ENTERAS, ninguna escritura puede partir una
 * linea.  Es la cota superior de lo que la alineacion puede dar. */
#define VESTA_ALIGNED64_SIZES(X)                                               \
    X(64) X(128) X(192) X(256) X(384) X(512) X(1024) X(2048) X(4096) X(16384)  \
        X(65536)

/* Multiplos de 16 que NO lo son de 32: caen justo entre dos vueltas del bucle
 * ancho, y con la lista de 32 no se prueba ninguno. */
#define VESTA_ALIGNED16_SIZES(X)                                               \
    X(16) X(48) X(80) X(112) X(144) X(176) X(208) X(240) X(272) X(496)         \
        X(1008) X(2032) X(4080)

/* Multiplos de 8 que no lo son de 16.  Importa por lo contrario que los otros:
 * la alineacion se SABE pero NO BASTA, asi que el prologo se sigue emitiendo. */
#define VESTA_ALIGNED8_SIZES(X)                                                \
    X(8) X(24) X(40) X(56) X(88) X(120) X(152) X(248) X(504) X(1016) X(4088)

/* Cualquier tamano, alineacion natural.  Incluye los de una unidad mas y una
 * menos que cada frontera (15/16/17, 31/32/33, 127/128/129, ...): ahi es donde
 * un `>=` mal puesto cambia de camino y donde la cola se come un byte. */
#define VESTA_ANY_SIZES(X)                                                     \
    X(1) X(3) X(7) X(8) X(9) X(15) X(16) X(17) X(24) X(31) X(32) X(33) X(47)   \
        X(50) X(63) X(64) X(65) X(100) X(127) X(128) X(129) X(200) X(250)      \
            X(255) X(256) X(257) X(500) X(1000) X(1023) X(1024) X(1025)        \
                X(2047) X(4095) X(4096) X(4097)

/// Todas las tablas, en orden.  Esta aparte de `main` porque en un procesador
/// hibrido se recorre VARIAS veces, una por clase de nucleo; ver `main`.
void run_sections() {
    header("zeroing, aligned 32",
           "zeroing, aligned to 32 (what calloc does) -- sizes must be a "
           "multiple of 32, see the note on the size lists");
#define ROW_A32(N) row(N, hot_trio<N, 32, 0>(0, rounds_for(N)));
    VESTA_ALIGNED32_SIZES(ROW_A32)
#undef ROW_A32

    header("aligned 64",
           "aligned to 64 -- whole cache lines, so no store can split one "
           "even with the worst luck; the ceiling of what alignment can give");
#define ROW_A64(N) row(N, hot_trio<N, 64, 0>(0, rounds_for(N)));
    VESTA_ALIGNED64_SIZES(ROW_A64)
#undef ROW_A64

    header("aligned 16",
           "aligned to 16 -- the sizes that fall between two turns of the "
           "wide loop, which the multiples of 32 never reach");
#define ROW_A16(N) row(N, hot_trio<N, 16, 0>(0, rounds_for(N)));
    VESTA_ALIGNED16_SIZES(ROW_A16)
#undef ROW_A16

    header("aligned 8",
           "aligned to 8 -- the alignment is KNOWN but NOT ENOUGH (8 < the "
           "16 of the vector), so the prologue is still emitted: this checks "
           "that knowing too little breaks nothing");
#define ROW_A8(N) row(N, hot_trio<N, 8, 0>(0, rounds_for(N)));
    VESTA_ALIGNED8_SIZES(ROW_A8)
#undef ROW_A8

    header("non-zero byte",
           "filling with a non-zero byte (no special path can apply)");
#define ROW_FILL(N) row(N, hot_trio<N, 32, 0>(0x5C, rounds_for(N)));
    VESTA_ALIGNED32_SIZES(ROW_FILL)
#undef ROW_FILL

    header("natural (control)",
           "natural alignment -- the type promises nothing, so the typed one "
           "has nothing to drop and MUST come out level; this section is the "
           "CONTROL, any deviation here is the noise floor");
#define ROW_ANY(N) row(N, hot_trio<N, 1, 3>(0, rounds_for(N)));
    VESTA_ANY_SIZES(ROW_ANY)
#undef ROW_ANY

    header("scattered",
           "scattered over 16 MiB, aligned -- recycled blocks, not in cache");
#define ROW_COLD(N) row(N, scattered_trio<N, 32, 0>(0, rounds_for(N) / 8));
    VESTA_ALIGNED32_SIZES(ROW_COLD)
#undef ROW_COLD

    /* Los dos barridos que siguen mueven la alineacion en vez del tamano, que
     * es la otra variable de la que depende esta rutina.  En las tablas de
     * arriba la alineacion esta fija dentro de cada seccion y el tamano recorre
     * la lista, asi que se ve como escala con el tamano pero no que le pasa a
     * UN tamano cuando la direccion se mueve.  Son dos ejes y hacen falta los
     * dos. */
    header("by declared alignment",
           "same size, varying what the TYPE promises (32/16/8/1) with the "
           "address always aligned -- this isolates what the promise buys, "
           "because the memory does not change at all between rows");
#define ROW_BY_ALIGN(N, A)                                                     \
    row(#N " @" #A, N, hot_trio<N, A, 0>(0, rounds_for(N)));
    ROW_BY_ALIGN(256, 32)
    ROW_BY_ALIGN(256, 16)
    ROW_BY_ALIGN(256, 8)
    ROW_BY_ALIGN(256, 1)
    ROW_BY_ALIGN(1024, 32)
    ROW_BY_ALIGN(1024, 16)
    ROW_BY_ALIGN(1024, 8)
    ROW_BY_ALIGN(1024, 1)
    ROW_BY_ALIGN(4096, 32)
    ROW_BY_ALIGN(4096, 16)
    ROW_BY_ALIGN(4096, 8)
    ROW_BY_ALIGN(4096, 1)
#undef ROW_BY_ALIGN

    header("by address offset",
           "same size, the type promising NOTHING, and the address pushed off "
           "a cache line by 0..31 bytes -- neither side can drop the prologue "
           "here, so what this measures is how the hardware degrades");
#define ROW_BY_OFF(N, O)                                                       \
    row(#N " +" #O, N, hot_trio<N, 1, O>(0, rounds_for(N)));
    ROW_BY_OFF(256, 0)
    ROW_BY_OFF(256, 1)
    ROW_BY_OFF(256, 2)
    ROW_BY_OFF(256, 4)
    ROW_BY_OFF(256, 8)
    ROW_BY_OFF(256, 15)
    ROW_BY_OFF(256, 16)
    ROW_BY_OFF(256, 31)
    ROW_BY_OFF(4096, 0)
    ROW_BY_OFF(4096, 1)
    ROW_BY_OFF(4096, 2)
    ROW_BY_OFF(4096, 4)
    ROW_BY_OFF(4096, 8)
    ROW_BY_OFF(4096, 15)
    ROW_BY_OFF(4096, 16)
    ROW_BY_OFF(4096, 31)
#undef ROW_BY_OFF
}

} // namespace

int main() {
    std::printf("== vesta_memset: the C interface, the typed C++ one, and "
                "libc ==\n\n");
    std::printf("All three are called in this process on the same addresses,\n"
                "interleaved A-B-C-C-B-A, in nanoseconds per fill.  `C/C++` is\n"
                "what the type buys; `libc/us` compares the better of ours\n"
                "against the system.\n");

    /* Los ficheros se abren ANTES de medir nada, no al final: si la tanda se
     * interrumpe, lo escrito hasta ese punto sigue siendo un CSV valido con
     * menos filas.  Guardarlo todo al terminar significa perderlo todo cuando
     * no se termina, que es justo cuando mas se agradece tenerlo. */
    csv::Report out("memset");
    g_csv = out.open() ? &out : nullptr;
    if (!out.open())
        std::printf("\n%sCould not write the CSVs (%s_*.csv); the tables below "
                    "are still complete.%s\n",
                    report::amber(), out.prefix().c_str(), report::reset());

    /* En un procesador hibrido la tabla entera se mide TRES veces: atada a los
     * nucleos grandes, atada a los pequenos, y sin atar.  No es por gusto -- es
     * que "el rendimiento de esta maquina" no existe ahi: hay dos, y muy
     * distintos justo en lo que este banco mide.  Sin separarlos, la tanda sin
     * atar mezcla los dos en una proporcion que decide el sistema y que cambia
     * de una corrida a la siguiente, que es una de las sospechas que quedaban
     * sobre por que dos corridas no coincidian.
     *
     * En un procesador normal la lista trae UNA pasada y todo esto no cuesta
     * nada. */
    const std::vector<affinity::Pass> passes = affinity::passes();
    /* Y si NO se pudo separar habiendo motivo para esperarlo, se dice por que.
     * Saltarse la comparacion en silencio la haria parecer hecha y con
     * resultado "no hay diferencia". */
    if (affinity::topology().why[0] != 0)
        std::printf("\n%sOnly one pass: %s.%s\n", report::amber(),
                    affinity::topology().why, report::reset());
    if (passes.size() > 1) {
        std::printf("\n%sThis CPU has two kinds of core, so everything below "
                    "is measured more\nthan once -- on a hybrid part there is "
                    "no single \"speed of this machine\"\nto report.  Passes:",
                    report::dim());
        for (const affinity::Pass &p : passes)
            std::printf(" %s%s%s", report::reset(), p.name, report::dim());
        std::printf(".\nThe small cores are left out by default: they decide "
                    "no verdict and cost\nmore than half the clock.  Ask for "
                    "them with VESTA_BENCH_PASSES=p,e,all.%s\n",
                    report::reset());
    }

    for (const affinity::Pass &p : passes) {
        const bool ok = affinity::pin(p.cpus);
        /* Se comprueba DONDE quedo, no se da por hecho: una atadura que falla
         * en silencio convertiria las tres pasadas en la misma medida repetida
         * y el informe diria que no hay diferencia entre clases de nucleo. */
        const char *seen = isa::current_core_kind();

        g_pass = p.name;
        std::printf("\n%s%s#### %s ####%s\n", report::bold(), report::cyan(),
                    p.name, report::reset());
        if (!p.cpus.empty() && !ok)
            std::printf("%scould not pin to those cores; this pass measures "
                        "whatever the scheduler gave it%s\n",
                        report::amber(), report::reset());
        else if (seen[0] != '\0')
            std::printf("%srunning on a %s%s\n", report::dim(), seen,
                        report::reset());

        calibrate();
        out.begin_pass(p.name, seen, g_floor);
        g_passes.push_back({p.name, seen, g_floor});
        run_sections();
    }
    affinity::unpin();

    plots(pass_summary());
    core_plots();

    std::printf("\nWhat the type buys shows up where the size is NOT a\n"
                "multiple of the store width: there the C interface pays the\n"
                "run-time alignment prologue in full, and paying it turns a\n"
                "constant length into a variable one, which is what stops the\n"
                "loop from being unrolled.  On round sizes the cascade already\n"
                "folded on its own and the two come out level -- which is the\n"
                "honest result, not a disappointing one.\n");

    if (out.open())
        std::printf("\n%sAlso written, three files sharing the prefix%s "
                    "%s%s%s%s:\n  `_cpu`   the machine, one row\n  `_runs`  one "
                    "row per pass\n  `_data`  one row per row of table above\n"
                    "They join on `run_id`, so batches can be concatenated and "
                    "still\ntell each other apart.%s\n",
                    report::dim(), report::reset(), report::bold(),
                    out.prefix().c_str(), report::reset(), report::dim(),
                    report::reset());
    return 0;
}
