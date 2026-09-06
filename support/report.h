/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file support/report.h
 * @brief Color y veredictos para lo que esta libreria IMPRIME: sus tests y sus
 *        bancos.
 *
 * POR QUE ESTA COPIADO Y NO COMPARTIDO.  El compilador de VestaVM tiene el
 * mismo ayudante en @c tests/util/test_report.h, y lo normal seria incluirlo.
 * No se puede, y no por descuido: esta libreria es un repositorio APARTE, con
 * licencia MIT frente a la GPLv2 del compilador, y su promesa es no depender de
 * nada de el.  Incluir aquel fichero romperia las dos cosas a la vez.
 *
 * Asi que se copia, y se copia IGUAL -- los mismos nombres de color, el mismo
 * @c NO_COLOR, el mismo formato de resumen -- para que los dos arboles se lean
 * de la misma forma aunque no compartan una linea.
 *
 * Vive en @c support/ y no en @c include/ a proposito: no es parte de la
 * interfaz de la libreria, es andamiaje de lo que la prueba y la mide.
 *
 * Cabecera sola, sin biblioteca que enlazar: un test de aqui se compila con un
 * @c g++ y sus objetos, y anadirle una dependencia de enlace por unas cadenas
 * de color seria peor que la duplicacion.
 */
#ifndef VESTA_ALLOC_SUPPORT_REPORT_H
#define VESTA_ALLOC_SUPPORT_REPORT_H

#include <cstdio>
#include <cstdlib>

namespace report {

/**
 * @brief Si la salida lleva color.
 *
 * Se apaga con @c NO_COLOR, que es la convencion de todas las herramientas: una
 * tabla redirigida a un fichero o leida por otro programa no debe llevar
 * escapes dentro.
 */
inline bool color_enabled() {
    static const bool v = std::getenv("NO_COLOR") == nullptr;
    return v;
}

/// Devuelve @p code si hay color, o la cadena vacia si no: asi el mismo
/// `printf` sirve para los dos casos sin duplicar el formato.
inline const char *ansi(const char *code) {
    return color_enabled() ? code : "";
}

/// @name Colores, nombrados por lo que SIGNIFICAN donde se pueda.
/// @{
inline const char *dim() { return ansi("\033[90m"); }   ///< detalle secundario
inline const char *red() { return ansi("\033[31m"); }   ///< algo esta mal
inline const char *green() { return ansi("\033[32m"); } ///< algo esta bien
inline const char *amber() { return ansi("\033[33m"); } ///< dudoso, no roto
inline const char *cyan() { return ansi("\033[36m"); }  ///< un grupo o seccion
inline const char *bold() { return ansi("\033[1m"); }   ///< titulo
inline const char *reset() { return ansi("\033[0m"); }  ///< vuelve a lo normal
/// @}

/**
 * @brief Recuento de un informe: lo correcto, lo dudoso y lo roto.
 *
 * Los tres van separados a proposito.  "Dudoso" no es "roto": en un banco es
 * una fila cuyo ruido no deja decidir, y en un test un caso que no se pudo
 * ejercer -- por ejemplo un camino de CPU que esta maquina no tiene --.
 * Mezclarlo con los fallos obliga a elegir entre dejarlo en rojo permanente,
 * que se acaba ignorando, o callarlo, que es peor porque entonces nadie sabe
 * que quedo sin mirar.
 */
struct Tally {
    int passed = 0;
    int unclear = 0;
    int failed = 0;

    /// Codigo de salida: solo los FALLOS lo ponen a uno.
    int exit_code() const { return failed == 0 ? 0 : 1; }
};

/// Titulo del informe.
inline void title(const char *text) {
    std::printf("%s[%s]%s\n", bold(), text, reset());
}

/// Cabecera de seccion, con lo que la describa.
inline void section(const char *name, const char *detail = "") {
    std::printf("\n%s== %s ==%s\n%s%s%s\n", bold(), name, reset(), dim(),
                detail, reset());
}

/// Una comprobacion que salio bien.  Se imprime TAMBIEN lo correcto: un informe
/// que solo ensena los fallos no dice que se comprobo, y entonces no se puede
/// distinguir "todo bien" de "no se miro".
inline void pass(Tally &t, const char *what) {
    std::printf("  %sok%s    %s\n", green(), reset(), what);
    ++t.passed;
}

/// Algo que no se pudo decidir.  Lleva SIEMPRE su motivo: sin el, es
/// indistinguible de un caso olvidado.
inline void unclear(Tally &t, const char *what, const char *why) {
    std::printf("  %s?%s     %-36s %s%s%s\n", amber(), reset(), what, dim(),
                why, reset());
    ++t.unclear;
}

/// Algo que esta mal.
inline void fail(Tally &t, const char *what) {
    std::printf("  %sFAIL%s  %s\n", red(), reset(), what);
    ++t.failed;
}

/// Resumen final con los tres recuentos.
inline void summary(const char *what, const Tally &t) {
    const char *tint =
        t.failed != 0 ? red() : (t.unclear != 0 ? amber() : green());
    std::printf("\n%s[%s]%s %s%d correctas%s, %s%d sin decidir%s, %s%d "
                "fallos%s -> %s%s%s\n",
                bold(), what, reset(), green(), t.passed, reset(), amber(),
                t.unclear, reset(), red(), t.failed, reset(), tint,
                t.failed == 0 ? "OK" : "CON FALLOS", reset());
}

} // namespace report

#endif // VESTA_ALLOC_SUPPORT_REPORT_H
