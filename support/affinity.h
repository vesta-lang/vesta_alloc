/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file support/affinity.h
 * @brief Averigua que nucleos son de cada clase y ata la medida a los que se
 *        quiera.
 *
 * POR QUE.  En un procesador hibrido no hay UN rendimiento: hay dos.  Los
 * nucleos grandes y los pequenos no comparten ancho de almacenamiento, ni
 * tamano de cacha de segundo nivel, ni el punto a partir del cual `rep movsb`
 * sale rentable -- que es justo de lo que va este banco --.  Con el sistema
 * moviendo el proceso a su gusto, una tanda puede empezar en uno y acabar en
 * otro, y entonces la media no describe a ninguno de los dos.
 *
 * Peor todavia para lo que se venia haciendo: dos corridas del banco pueden
 * caer en clases distintas, dar ordenaciones distintas, y no habria en el
 * fichero ni una pista de por que.
 *
 * ASI QUE SE MIDE TRES VECES -- solo grandes, solo pequenos, y como caiga -- y
 * se compara.  Si no hay diferencia, queda demostrado y se deja de sospechar de
 * ahi.  Si la hay, esta medida en vez de discutida.
 *
 * COMO SE SABE CUAL ES CUAL.  Preguntandoselo al procesador desde cada nucleo:
 * se ata el hilo a uno, se ejecuta @c CPUID en su hoja 0x1A -- que responde con
 * la clase del nucleo donde se ejecuta -- y se apunta.  Es mas rodeo que
 * llamar a la funcion del sistema que lo dice, y se hace igual por dos razones:
 * vale en Windows y en Linux con el mismo codigo, y sale del MISMO sitio del
 * que sale todo lo demas de `isa.h`, con lo que no puede discrepar.
 *
 * SE ATA AL GRUPO ENTERO, no a un nucleo suelto.  Fijar la medida a un unico
 * nucleo la deja a merced de lo que ese nucleo tenga que hacer ademas -- una
 * interrupcion, otro hilo del sistema -- sin ninguna salida.  Con el grupo, el
 * planificador sigue pudiendo apartarse de un estorbo sin salirse de la clase,
 * que es lo unico que aqui hace falta garantizar.
 */
#ifndef VESTA_ALLOC_SUPPORT_AFFINITY_H
#define VESTA_ALLOC_SUPPORT_AFFINITY_H

#include "isa.h"

#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <sched.h>
#endif

namespace affinity {

/// Los nucleos de cada clase, por su numero logico.
struct Topology {
    std::vector<int> perf; ///< los grandes
    std::vector<int> eff;  ///< los pequenos
    bool usable = false;   ///< si se pudo atar el hilo para preguntar
    /// POR QUE no se pudo separar, cuando no se pudo y habia motivo para
    /// esperar que si.  Vacio cuando no hay nada que explicar -- un procesador
    /// de una sola clase --.  Un banco que se salta una comparacion en silencio
    /// parece que la hizo y que salio igual.
    const char *why = "";
};

#if defined(VESTA_MEM_ARCH_X86) && (defined(_WIN32) || defined(__linux__))
#define VESTA_AFFINITY_AVAILABLE 1
#else
#define VESTA_AFFINITY_AVAILABLE 0
#endif

/// Ata el hilo actual a un conjunto de nucleos.  Conjunto vacio = todos.
inline bool pin(const std::vector<int> &cpus) {
#if VESTA_AFFINITY_AVAILABLE
#if defined(_WIN32)
    DWORD_PTR mask = 0;
    if (cpus.empty()) {
        DWORD_PTR proc = 0, sys = 0;
        if (GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys) == 0)
            return false;
        mask = proc;
    } else {
        for (int c : cpus) {
            if (c < 0 || c >= int(sizeof(DWORD_PTR) * 8)) continue;
            mask |= (DWORD_PTR(1) << c);
        }
    }
    if (mask == 0) return false;
    if (SetThreadAffinityMask(GetCurrentThread(), mask) == 0) return false;
#else
    cpu_set_t set;
    CPU_ZERO(&set);
    if (cpus.empty()) {
        for (int c = 0; c < CPU_SETSIZE; ++c)
            CPU_SET(c, &set);
    } else {
        for (int c : cpus)
            if (c >= 0 && c < CPU_SETSIZE) CPU_SET(c, &set);
    }
    if (sched_setaffinity(0, sizeof(set), &set) != 0) return false;
#endif

    /* Fijar la mascara no MUEVE el hilo por si solo: solo dice donde puede
     * estar.  Se ceden dos turnos para que el planificador lo lleve de verdad
     * antes de que nadie mida ni pregunte nada. */
#if defined(_WIN32)
    SwitchToThread();
    SwitchToThread();
#else
    sched_yield();
    sched_yield();
#endif
    return true;
#else
    (void)cpus;
    return false;
#endif
}

/// Devuelve el hilo a todos los nucleos.
inline bool unpin() { return pin(std::vector<int>()); }

/**
 * @brief Clasifica los nucleos preguntando desde cada uno.
 *
 * Se hace UNA vez y se guarda: el recorrido ata el hilo nucleo por nucleo, y
 * repetirlo a mitad de una tanda seria mover la medida de sitio.
 */
inline const Topology &topology() {
    static const Topology t = [] {
        Topology k;
#if VESTA_AFFINITY_AVAILABLE
        /* Sin bandera de hibrido no hay nada que separar, y tampoco nada que
         * explicar: es un procesador normal.  Ojo, esto tambien pasa DENTRO de
         * una maquina virtual, que suele esconder la bandera aunque el
         * anfitrion si la tenga. */
        if (!isa::cpu().hybrid) return k;

        const unsigned n = isa::threads();
        if (n == 0 || vesta_mem_x86_cpuid_max() < 0x1Au) {
            k.why = "the CPU says it has two kinds of core but will not say "
                    "which is which";
            return k;
        }

        for (unsigned i = 0; i < n; ++i) {
            if (!pin(std::vector<int>{int(i)})) {
                Topology bad;
                bad.why = "the CPU has two kinds of core but this process is "
                          "not allowed to pin threads";
                return bad;
            }
            const unsigned type =
                (vesta_mem_x86_cpuid(0x1Au, 0).eax >> 24) & 0xFFu;
            if (type == 0x40u)
                k.perf.push_back(int(i));
            else if (type == 0x20u)
                k.eff.push_back(int(i));
        }
        unpin();

        /* Solo sirve si salieron las DOS clases.  Con una sola, atar la medida
         * a "los grandes" seria atarla a todos y la comparacion no compararia
         * nada -- y peor, pareceria que si. */
        k.usable = !k.perf.empty() && !k.eff.empty();
        if (!k.usable)
            k.why = "the CPU has two kinds of core but every one of them "
                    "answered the same class";
#endif
        return k;
    }();
    return t;
}

/// Una pasada del banco: como se llama y sobre que nucleos corre.
struct Pass {
    const char *name;
    std::vector<int> cpus; ///< vacio = como caiga, sin atar
};

/**
 * @brief Las pasadas que hay que hacer en esta maquina.
 *
 * En un procesador normal es UNA, sin atar: separar por clases donde solo hay
 * una clase multiplicaria el tiempo para medir varias veces lo mismo.
 *
 * En uno hibrido son DOS por defecto -- atada a los grandes y sin atar -- y los
 * pequenos se piden aparte.  POR QUE SE QUEDAN FUERA, habiendo sido ellos los
 * que destaparon el problema: porque lo que destaparon ya esta sabido y no
 * cambia.  Que un nucleo pequeno tarde de dos a cinco veces mas es una
 * propiedad del PROCESADOR, no de nuestro codigo, asi que medirla otra vez en
 * cada tanda no informa ninguna decision -- y el veredicto que si importa, cual
 * de las tres implementaciones gana, se lee de la pasada atada a los grandes.
 *
 * Y no es gratis: al ser mas lentos tardan lo mismo en las MISMAS vueltas, asi
 * que esa pasada se llevaba mas de la mitad del reloj de la tanda entera.
 *
 * Se piden con @c VESTA_BENCH_PASSES, una lista separada por comas de @c p,
 * @c e y @c all.  Ejemplos: @c p para iterar deprisa sobre un cambio de codigo,
 * @c p,e,all para volver a mirar las diferencias entre clases de nucleo.
 */
inline std::vector<Pass> passes() {
    const Topology &t = topology();
    if (!t.usable) return {Pass{"all cores", {}}};

    /* Por defecto: la que decide y la que describe.  La de los pequenos hay que
     * pedirla. */
    std::string want = "p,all";
    const char *env = std::getenv("VESTA_BENCH_PASSES");
    if (env != nullptr && env[0] != '\0') want = env;

    std::vector<Pass> out;
    size_t i = 0;
    while (i <= want.size()) {
        const size_t j = want.find(',', i);
        std::string tok = want.substr(i, (j == std::string::npos) ? j : j - i);
        i = (j == std::string::npos) ? want.size() + 1 : j + 1;

        /* Se limpian espacios y mayusculas: la lista la escribe una persona en
         * una linea de ordenes, no un programa. */
        size_t a = tok.find_first_not_of(" \t");
        size_t b = tok.find_last_not_of(" \t");
        if (a == std::string::npos) continue;
        tok = tok.substr(a, b - a + 1);
        for (char &c : tok)
            if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');

        if (tok == "p")
            out.push_back(Pass{"P-cores", t.perf});
        else if (tok == "e")
            out.push_back(Pass{"E-cores", t.eff});
        else if (tok == "all")
            out.push_back(Pass{"all cores", {}});
    }

    /* Una lista que no nombra ninguna pasada valida no puede dejar el banco sin
     * medir nada: eso seria una errata que se lee como "no hay resultados". */
    if (out.empty()) out.push_back(Pass{"all cores", {}});
    return out;
}

} // namespace affinity

#endif // VESTA_ALLOC_SUPPORT_AFFINITY_H
