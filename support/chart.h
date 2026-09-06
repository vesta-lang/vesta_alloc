/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file support/chart.h
 * @brief Graficos de linea en la terminal, para ver la FORMA de lo que mide un
 *        banco.
 *
 * POR QUE HACE FALTA, teniendo ya la tabla.  Una tabla de cuarenta filas se lee
 * fila a fila, y lo que interesa de un banco casi nunca es una fila: es DONDE
 * cambia el comportamiento -- a partir de que tamano el tipo deja de aportar, en
 * que tramo se pierde contra el sistema, si un veredicto raro esta rodeado de
 * iguales o es el unico de su vecindario --.  Eso es una forma, y una forma se
 * ve; leyendo cuarenta numeros hay que reconstruirla en la cabeza, y cada quien
 * la reconstruye distinta.
 *
 * POR QUE BRAILLE Y NO BLOQUES.  El primer intento fue una tira de una linea de
 * alto con los bloques de octavo (U+2581..U+2588), un caracter por medida.  No
 * servia, y el motivo es geometrico: en una sola linea de texto la diferencia
 * entre un octavo y siete octavos son unos pocos pixeles, asi que lo unico que
 * se distinguia era el COLOR y el dibujo no anadia nada a la columna de
 * veredictos que ya estaba al lado.  Los caracteres braille (U+2800..U+28FF)
 * llevan una rejilla de 2x4 puntos independientes CADA UNO, asi que un panel de
 * 60x6 caracteres son 120x24 puntos: eso ya es un grafico, con pendiente,
 * codos y cruces entre curvas.
 *
 * ESCALA LOGARITMICA en el eje vertical, que es lo unico honesto con estos
 * datos.  Entre el bloque mas pequeno y el mas grande de una tabla hay tres
 * ordenes de magnitud: en lineal, las quince primeras filas se aplastarian
 * contra el suelo y el dibujo solo diria "los grandes tardan mas", que ya se
 * sabia.  En logaritmica lo que se ve es la PENDIENTE, y ahi si hay respuesta a
 * como escala cada uno: pendiente constante es coste proporcional al tamano, y
 * un codo es un cambio de camino o un nivel de cache que se acaba.
 *
 * Y con proporciones pasa lo mismo por otra razon: 2x y 0,5x son la misma
 * distancia -- una es exactamente lo contrario de la otra -- y solo el
 * logaritmo las coloca simetricas respecto de la linea del empate.
 *
 * SOBRE EL UNICODE.  Los caracteres van compuestos byte a byte y no escritos
 * literales, para que este fichero siga siendo ASCII como el resto del arbol;
 * lo que sale por la salida estandar si es UTF-8.  Con @c NO_UNICODE se cae a
 * un dibujo ASCII, por la misma razon que existe @c NO_COLOR: una terminal
 * vieja, una tuberia hacia otro programa, o una consola con otra pagina de
 * codigos.  En Windows se pide la pagina UTF-8 al arrancar, porque la de por
 * defecto convertiria los puntos en basura.
 */
#ifndef VESTA_ALLOC_SUPPORT_CHART_H
#define VESTA_ALLOC_SUPPORT_CHART_H

#include "report.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace chart {

/**
 * @brief Si se dibuja con braille o con una rejilla ASCII.
 *
 * De paso deja la consola en condiciones: en Windows la pagina de codigos por
 * defecto no es UTF-8 y los puntos saldrian como grupos de simbolos sueltos.
 * Se hace UNA vez, dentro del @c static, y solo si de verdad se va a dibujar.
 */
inline bool unicode_enabled() {
    static const bool v = [] {
        if (std::getenv("NO_UNICODE") != nullptr) return false;
#if defined(_WIN32)
        SetConsoleOutputCP(CP_UTF8);
#endif
        return true;
    }();
    return v;
}

/// Una serie del grafico: como se llama, sus valores y de que color se pinta.
struct Series {
    const char *label;
    const double *v;
    const char *color;
};

/**
 * @brief El patron de trazo de una serie: cuantos puntos pinta y cuantos salta.
 *
 * POR QUE NO BASTA EL COLOR.  Estas tres curvas van casi juntas -- esa es la
 * noticia, no un defecto del dibujo --, y donde coinciden solo cabe UN color
 * por celda: la ultima dibujada tapa a las otras y parece que hay una sola.
 * Con trazos distintos, el tramo compartido sale alternando los tres y se lee
 * como lo que es, "aqui van iguales", en vez de como "aqui solo hay una".
 *
 * Y de paso el grafico sigue siendo legible en blanco y negro, que es como
 * acaba cuando alguien lo pega en un fichero o en un correo.
 */
struct Dash {
    int on;     ///< puntos seguidos que se pintan
    int period; ///< cada cuantos se repite
};

/// Continua, discontinua y punteada, en ese orden.  Se reparten por posicion,
/// asi que la primera serie es siempre la mas visible: es la que suele ser el
/// sujeto de la comparacion.
///
/// LOS PERIODOS SON CORTOS a proposito.  Con treinta filas de tabla y ciento
/// cuarenta puntos de ancho, cada tramo entre dos medidas ocupa cuatro o cinco
/// columnas: un patron de periodo ocho dejaria tramos enteros dentro del hueco
/// y la curva desapareceria a trozos, que es peor que no distinguirla.
inline Dash dash_for(int series) {
    static const Dash kDashes[3] = {{1, 1}, {3, 5}, {2, 4}};
    return kDashes[series % 3];
}

/**
 * @brief El caracter que representa un grupo de puntos encendidos.
 *
 * @param bits Los ocho puntos de la celda, uno por bit.
 */
inline std::string cell_glyph(uint8_t bits) {
    if (unicode_enabled()) {
        /* U+2800 + bits, compuesto a mano en UTF-8 para no meter bytes no ASCII
         * en el fuente.  El punto de codigo cabe siempre en tres bytes y el
         * primero es constante. */
        char b[4];
        b[0] = char(0xE2);
        b[1] = char(0xA0 + (bits >> 6));
        b[2] = char(0x80 + (bits & 0x3F));
        b[3] = '\0';
        return std::string(b);
    }
    /* Sin unicode no hay resolucion de sub-caracter; lo unico que se puede
     * conservar es la DENSIDAD, que ya distingue una linea de una zona vacia. */
    int n = 0;
    for (int i = 0; i < 8; ++i)
        if (bits & (1u << i)) ++n;
    static const char kRamp[9] = {' ', '.', '.', ':', ':', '*', '*', '#', '#'};
    return std::string(1, kRamp[n]);
}

/// Los ocho puntos de una celda braille NO estan numerados en el orden que uno
/// esperaria: las tres primeras filas van en los bits 0..2 y 3..5, y la CUARTA
/// quedo fuera cuando se amplio el braille de 6 a 8 puntos, en los bits 6 y 7.
/// De ahi la tabla en vez de una formula.
inline uint8_t dot_bit(int row, int col) {
    static const uint8_t kBit[4][2] = {
        {0x01, 0x08}, {0x02, 0x10}, {0x04, 0x20}, {0x40, 0x80}};
    return kBit[row][col];
}

/// Un trocito del trazo de la serie @p j, para ponerlo en la leyenda.  Sin esto
/// la leyenda solo distingue por color, que es justo lo que falla cuando dos
/// curvas se cruzan o cuando el color no llega.
inline std::string dash_sample(int j) {
    const Dash d = dash_for(j);
    std::string out;
    for (int c = 0; c < 3; ++c) {
        uint8_t bits = 0;
        for (int h = 0; h < 2; ++h) {
            const int x = c * 2 + h;
            if (d.period <= 1 || (x % d.period) < d.on) bits |= dot_bit(1, h);
        }
        out += cell_glyph(bits);
    }
    return out;
}

/**
 * @brief Lienzo de puntos sobre una rejilla de caracteres.
 *
 * Cada caracter braille es una rejilla de 2 columnas por 4 filas de puntos que
 * se encienden por separado, asi que el lienzo tiene @c 2*cols por @c 4*rows
 * puntos direccionables.  Ese factor de ocho es toda la diferencia entre un
 * grafico y una fila de cuadrados de colores.
 *
 * El color se guarda POR CARACTER y no por punto, porque no hay mas: una celda
 * es una sola posicion de la terminal y solo admite un color.  Cuando dos
 * curvas caen en la misma celda gana la que se dibujo la ULTIMA, y por eso los
 * bancos pintan las nuestras primero y la referencia del sistema despues: si
 * dos curvas se solapan es que ahi van igual, y en la duda es mejor que se vea
 * la ajena que la propia.
 */
class Canvas {
  public:
    Canvas(int cols, int rows)
        : cols_(cols), rows_(rows), dots_(size_t(cols) * size_t(rows), 0),
          owner_(size_t(cols) * size_t(rows), -1) {}

    int dot_width() const { return cols_ * 2; }
    int dot_height() const { return rows_ * 4; }

    /// Enciende el punto (@p x, @p y), con el origen ARRIBA a la izquierda.
    void dot(int x, int y, int series) {
        if (x < 0 || y < 0 || x >= dot_width() || y >= dot_height()) return;

        const size_t idx = size_t(y / 4) * size_t(cols_) + size_t(x / 2);
        dots_[idx] |= dot_bit(y % 4, x % 2);
        owner_[idx] = int8_t(series);
    }

    /// Une dos puntos con una recta, con el trazo @p d.  Sin la recta el
    /// grafico es una nube de puntos sueltos y la pendiente -- que es justo lo
    /// que se viene a mirar -- hay que imaginarsela.
    void line(int x0, int y0, int x1, int y1, int series, Dash d) {
        const int dx = x1 > x0 ? x1 - x0 : x0 - x1;
        const int dy = y1 > y0 ? y1 - y0 : y0 - y1;
        const int sx = x0 < x1 ? 1 : -1;
        const int sy = y0 < y1 ? 1 : -1;
        int err = dx - dy;
        for (;;) {
            /* El hueco del trazo se decide por la COLUMNA absoluta y no por
             * cuantos puntos se llevan puestos: asi dos series con el mismo
             * patron quedarian en fase, y lo que interesa es justo lo
             * contrario -- que cada una tenga el suyo y no se confundan. */
            if (d.period <= 1 || (x0 % d.period) < d.on) dot(x0, y0, series);
            if (x0 == x1 && y0 == y1) break;
            const int e2 = err * 2;
            if (e2 > -dy) {
                err -= dy;
                x0 += sx;
            }
            if (e2 < dx) {
                err += dx;
                y0 += sy;
            }
        }
    }

    /// Marca una fila entera como referencia, en gris y solo donde no haya
    /// curva: si tapara una curva estaria escondiendo el dato para ensenar el
    /// eje, que es justo al reves de lo que hace falta.
    void reference_row(int y) { ref_row_ = y / 4; }

    /**
     * @brief Vuelca el lienzo, una linea por fila de caracteres.
     *
     * @param colors  Color de cada serie.
     * @param gutter  Ancho de la columna de etiquetas de la izquierda.
     * @param labels  Etiqueta de cada fila (puede llevar cadenas vacias).
     */
    void print(const Series *s, int k, int gutter,
               const std::vector<std::string> &labels) const {
        (void)k;
        for (int r = 0; r < rows_; ++r) {
            std::printf("%s%*s%s ", report::dim(), gutter,
                        r < int(labels.size()) ? labels[size_t(r)].c_str() : "",
                        report::reset());
            std::printf("%s|%s", report::dim(), report::reset());

            const char *cur = nullptr;
            for (int c = 0; c < cols_; ++c) {
                const size_t idx = size_t(r) * size_t(cols_) + size_t(c);
                const uint8_t bits = dots_[idx];

                if (bits == 0) {
                    /* Celda vacia: o la linea de referencia, o nada. */
                    if (r == ref_row_) {
                        if (cur != report::dim()) {
                            std::printf("%s", report::dim());
                            cur = report::dim();
                        }
                        std::printf("-");
                    } else {
                        if (cur != nullptr) {
                            std::printf("%s", report::reset());
                            cur = nullptr;
                        }
                        std::printf(" ");
                    }
                    continue;
                }

                const int who = owner_[idx];
                const char *col = (who >= 0) ? s[who].color : report::dim();
                if (cur != col) {
                    std::printf("%s", col);
                    cur = col;
                }
                std::printf("%s", cell_glyph(bits).c_str());
            }
            std::printf("%s\n", report::reset());
        }
    }

  private:
    int cols_, rows_;
    int ref_row_ = -1;
    std::vector<uint8_t> dots_;
    std::vector<int8_t> owner_;
};

/// Formatea un numero con la precision justa para que la etiqueta del eje se
/// lea: los tiempos van de centesimas a millares y un formato fijo o pierde la
/// diferencia o llena la columna de ceros.
inline std::string axis_label(double v) {
    char b[32];
    if (v >= 1000.0)
        std::snprintf(b, sizeof(b), "%.0f", v);
    else if (v >= 10.0)
        std::snprintf(b, sizeof(b), "%.1f", v);
    else
        std::snprintf(b, sizeof(b), "%.2f", v);
    return std::string(b);
}

/**
 * @brief Dibuja varias series en un panel comun.
 *
 * @param title   Que se esta mirando y en que unidad.
 * @param s       Las series.
 * @param k       Cuantas.
 * @param n       Cuantos puntos tiene cada una.
 * @param rows    Alto del panel en lineas de texto.
 * @param cols    Ancho en columnas.
 * @param ref     Valor al que se le pinta una linea de referencia, o 0.
 * @param x_first Etiqueta del extremo izquierdo del eje horizontal.
 * @param x_last  Etiqueta del extremo derecho.
 */
inline void plot(const char *title, const Series *s, int k, int n, int rows,
                 int cols, double ref, const char *x_first,
                 const char *x_last) {
    if (n <= 0 || k <= 0) return;

    /* Un rango COMUN a todas: cada una con su propio maximo saldria con la
     * misma altura que las demas y el dibujo diria lo contrario de lo que
     * pasa. */
    double lo = 0.0, hi = 0.0;
    for (int j = 0; j < k; ++j) {
        for (int i = 0; i < n; ++i) {
            const double v = s[j].v[i];
            if (!(v > 0.0)) continue; // un cero solo dice que no hubo medida
            if (lo == 0.0 || v < lo) lo = v;
            if (v > hi) hi = v;
        }
    }
    if (lo <= 0.0) return;

    /* La referencia entra en el rango aunque ninguna serie llegue: si el 1,00x
     * se sale del panel, el dibujo no dice si se gana o se pierde. */
    if (ref > 0.0) {
        if (ref < lo) lo = ref;
        if (ref > hi) hi = ref;
    }
    /* Un poco de aire arriba y abajo, o las curvas extremas se pegan al borde y
     * parecen recortadas. */
    const double pad = (hi > lo) ? (std::log2(hi) - std::log2(lo)) * 0.08 : 0.5;
    const double l_lo = std::log2(lo) - pad;
    const double l_hi = std::log2(hi) + pad;
    const double span = l_hi - l_lo;

    Canvas cv(cols, rows);
    const int H = cv.dot_height() - 1;
    const int W = cv.dot_width() - 1;

    auto to_y = [&](double v) {
        const double t = (std::log2(v) - l_lo) / span;
        int y = int((1.0 - t) * double(H) + 0.5);
        if (y < 0) y = 0;
        if (y > H) y = H;
        return y;
    };
    auto to_x = [&](int i) {
        return n == 1 ? 0 : int(double(i) * double(W) / double(n - 1) + 0.5);
    };

    if (ref > 0.0) cv.reference_row(to_y(ref));

    /* Las series se dibujan EN ORDEN, y donde coinciden gana la ultima.  Por
     * eso los bancos ponen las nuestras primero y la referencia del sistema al
     * final: si dos curvas se solapan es que ahi van igual, y en la duda es
     * mejor que se vea la ajena que la propia. */
    for (int j = 0; j < k; ++j) {
        const Dash d = dash_for(j);
        int px = -1, py = -1;
        for (int i = 0; i < n; ++i) {
            const double v = s[j].v[i];
            if (!(v > 0.0)) {
                px = -1;
                continue;
            }
            const int x = to_x(i), y = to_y(v);
            if (px >= 0) cv.line(px, py, x, y, j, d);
            cv.dot(x, y, j); // el vertice SIEMPRE, se lo salte el trazo o no:
                             // es un dato medido, no relleno entre dos
            px = x;
            py = y;
        }
    }

    /* Titulo con la leyenda al lado: cada nombre de su color y precedido de un
     * trocito de su propio trazo, que es lo que de verdad la identifica cuando
     * dos curvas se cruzan o cuando el color no llega. */
    std::printf("    %s%s%s  ", report::dim(), title, report::reset());
    for (int j = 0; j < k; ++j)
        std::printf("%s%s %s%s ", s[j].color, dash_sample(j).c_str(),
                    s[j].label, report::reset());
    std::printf("\n");

    /* Se rotulan la primera fila, la ultima y la de en medio.  Todas seria una
     * columna de cifras que nadie lee -- con escala logaritmica no caen en
     * numeros redondos --, y solo dos dejan sin referencia todo el centro, que
     * es donde suele estar lo interesante. */
    const int gutter = 9;
    std::vector<std::string> labels(static_cast<size_t>(rows));
    labels[0] = axis_label(std::exp2(l_hi));
    labels[size_t(rows) - 1] = axis_label(std::exp2(l_lo));
    if (rows >= 5)
        labels[size_t(rows / 2)] = axis_label(std::exp2((l_hi + l_lo) / 2.0));
    cv.print(s, k, gutter, labels);

    std::printf("%s%*s +", report::dim(), gutter, "");
    for (int c = 0; c < cols; ++c)
        std::printf("-");
    std::printf("\n%*s  %-*s%s%s\n", gutter, "", cols - int(std::strlen(x_last)),
                x_first, x_last, report::reset());
}

/// La leyenda, una sola vez por programa: repetirla en cada panel seria mas
/// texto que dibujo.
inline void legend() {
    std::printf(
        "\n%sEvery chart has one point per row of its table, in the same "
        "order,\njoined into a line.  The vertical axis is LOGARITHMIC, so "
        "what to\nread is the slope: a straight run is cost proportional to "
        "size, and\na knee is a change of path or a cache level running out.\n"
        "\n"
        "Each series has its own stroke as well as its own color -- solid,\n"
        "dashed, dotted, in the order of the legend.  Where the curves run\n"
        "together only one of them can own a cell, so a stretch that "
        "alternates\nstrokes is not three lines missing: it is three lines "
        "on top of each\nother, which is usually the answer.\n"
        "\n"
        "In the ratio charts the horizontal dashes are 1.00x, the tie: above\n"
        "them we win, below them we lose, and the distance is the same up and\n"
        "down because the scale is logarithmic.%s\n",
        report::dim(), report::reset());
}

} // namespace chart

#endif // VESTA_ALLOC_SUPPORT_CHART_H
