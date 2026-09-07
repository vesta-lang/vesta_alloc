/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file support/isa.h
 * @brief Que micro-ISA tiene la maquina y cual acabo usandose, en una etiqueta
 *        corta que sirve de nombre de fichero.
 *
 * POR QUE HACE FALTA.  Estas rutinas no son una: son varias, y cual corre lo
 * decide la maquina.  El mismo binario usa movimientos de 32 bytes donde hay
 * AVX2 y de 16 donde no, y `rep movsb` a partir de cierto tamano solo si el
 * procesador dice que le sale rentable.  Dos ficheros de medidas de la MISMA
 * version del codigo pueden tener numeros distintos por eso y nada mas -- y
 * puestos uno al lado del otro parecen decir que algo cambio.
 *
 * Por eso la etiqueta va en el NOMBRE del fichero y no solo en una columna: es
 * lo primero que se mira al elegir que dos tandas comparar, y con el nombre
 * delante nadie compara una tanda con AVX2 contra una sin el.
 *
 * SE PREGUNTA A CPUID, NO AL COMPILADOR, y es la unica forma de que la etiqueta
 * sea cierta.  Lo que sabe el compilador es para que se le PIDIO compilar
 * (`-march`), que no tiene por que ser lo que hay debajo: un binario de linea
 * base corriendo en una maquina con AVX2 detecta AVX2 y lo usa, y el compilador
 * no se entera.  Y al reves, cuando la micro-ISA se fija al compilar, la
 * deteccion de la libreria se pliega a una constante -- que es justo lo que se
 * quiere ahi -- y entonces YA NO PREGUNTA: sin acudir a CPUID por nuestra
 * cuenta, ese caso quedaria sin saber sobre que maquina corrio.
 *
 * TAMBIEN DICE SI ESTABA FIJADA.  No es lo mismo detectar AVX2 en ejecucion que
 * haberlo dado por hecho: en el segundo caso el despacho no se emite y el
 * codigo medido es OTRO, aunque la ruta acabe siendo la misma.  Es exactamente
 * una de las comparaciones que interesa hacer, y sin distinguirlo las dos
 * tandas se llamarian igual.
 *
 * ESTA CABECERA SI MIRA LA LIBRERIA, a diferencia de las otras de `support/`.
 * No es un descuido: usa su `CPUID` en vez de escribir otro, porque dos
 * detecciones distintas acaban discrepando y entonces la etiqueta miente sobre
 * el codigo que acompaña.
 */
#ifndef VESTA_ALLOC_SUPPORT_ISA_H
#define VESTA_ALLOC_SUPPORT_ISA_H

#include "util/mem/mem_config.h"
/* La copia es la de esta libreria, no la del sistema: seria raro que el
 * andamiaje que la mide se fiara de otra. */
#include "util/mem/vesta_memcpy.h"

#if defined(VESTA_MEM_ARCH_X86)
#include "util/mem/x86/x86_cpuid.h"
#endif

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

namespace isa {

/// Si la micro-ISA quedo decidida al compilar, en cuyo caso no hay despacho y
/// el codigo medido no es el mismo aunque la ruta coincida.
inline bool pinned() {
#if defined(VESTA_MEM_TARGET_AVX2) || defined(VESTA_MEM_TARGET_ERMS)
    return true;
#else
    return false;
#endif
}

#if defined(VESTA_MEM_ARCH_X86)

/**
 * @brief Lo que la maquina dice de si misma, preguntado una sola vez.
 *
 * Las banderas de vector NO se creen a la primera: tener la instruccion no basta
 * si el sistema operativo no guarda los registros anchos al cambiar de tarea, y
 * usarlas entonces corrompe los de otro hilo.  Por eso cada nivel se confirma
 * con @c XGETBV antes de darlo por bueno, igual que hace la libreria.
 */
struct Cpu {
    bool sse2 = false, ssse3 = false, sse42 = false;
    bool avx = false, avx2 = false, avx512f = false, avx512bw = false;
    bool erms = false, fsrm = false;
    /* Estas tres tocan de lleno lo que se mide aqui: las dos primeras escriben
     * saltandose la cache -- que es la otra forma de hacer un relleno grande --
     * y `clwb` decide una linea sin tirarla, que es lo que querria un asignador
     * al reciclar un bloque. */
    bool movdiri = false, movdir64b = false, clwb = false;
    /* Un procesador HIBRIDO tiene nucleos de dos clases con rendimiento muy
     * distinto en justo esto.  Sin apuntarlo, dos tandas de la misma maquina
     * pueden no ser comparables y nada en el fichero lo diria. */
    bool hybrid = false;
    const char *core = ""; ///< "p-core" / "e-core" donde se sepa

    unsigned family = 0, model = 0, stepping = 0;
    unsigned threads = 0; ///< hilos que el sistema deja ver

    /* Las cachas explican los codos del grafico: donde una serie se cae es casi
     * siempre donde el bloque dejo de caber en un nivel. */
    unsigned long long l1d = 0, l2 = 0, l3 = 0;
    unsigned line = 0; ///< bytes por linea, que es la unidad real de todo esto

    std::string vendor; ///< "intel", "amd", o lo que diga la hoja cero
    std::string brand;
};

/**
 * @brief Recorre la descripcion de cachas que da el procesador.
 *
 * Intel y AMD la dan en hojas distintas pero con el MISMO formato -- AMD copio
 * el de Intel al llegar Zen --, asi que solo cambia por cual se pregunta.
 */
inline void detect_caches(Cpu &k) {
    uint32_t leaf = 0;
    if (k.vendor == "amd") {
        if (vesta_mem_x86_cpuid(0x80000000u, 0).eax >= 0x8000001Du)
            leaf = 0x8000001Du;
    } else if (vesta_mem_x86_cpuid_max() >= 4) {
        leaf = 4;
    }
    if (leaf == 0) return;

    for (uint32_t i = 0; i < 16; ++i) {
        const vesta_cpuid_regs r = vesta_mem_x86_cpuid(leaf, i);

        /* Tipo cero significa "ya no hay mas": la lista no dice cuantas son, se
         * recorre hasta que se acaba. */
        const unsigned type = r.eax & 0x1Fu;
        if (type == 0) break;

        const unsigned level = (r.eax >> 5) & 0x7u;
        const unsigned line = (r.ebx & 0xFFFu) + 1u;
        const unsigned parts = ((r.ebx >> 12) & 0x3FFu) + 1u;
        const unsigned ways = ((r.ebx >> 22) & 0x3FFu) + 1u;
        const unsigned sets = r.ecx + 1u;
        const unsigned long long size =
            (unsigned long long)line * parts * ways * sets;

        if (k.line == 0) k.line = line;
        if (level == 1 && type == 1)
            k.l1d = size; // tipo 1 son DATOS; la de instrucciones no viene al caso
        else if (level == 2)
            k.l2 = size;
        else if (level == 3)
            k.l3 = size;
    }
}

inline const Cpu &cpu() {
    static const Cpu c = [] {
        Cpu k;

        /* El fabricante son doce caracteres repartidos en tres registros, y en
         * un orden que no es el natural: EBX, EDX, ECX. */
        const vesta_cpuid_regs r0 = vesta_mem_x86_cpuid(0, 0);
        char v[13];
        vesta_memcpy(v + 0, &r0.ebx, 4);
        vesta_memcpy(v + 4, &r0.edx, 4);
        vesta_memcpy(v + 8, &r0.ecx, 4);
        v[12] = '\0';
        if (std::strcmp(v, "GenuineIntel") == 0)
            k.vendor = "intel";
        else if (std::strcmp(v, "AuthenticAMD") == 0)
            k.vendor = "amd";
        else
            k.vendor = v;

        const vesta_cpuid_regs r1 = vesta_mem_x86_cpuid(1, 0);
        k.sse2 = (r1.edx & (1u << 26)) != 0;
        k.ssse3 = (r1.ecx & (1u << 9)) != 0;
        k.sse42 = (r1.ecx & (1u << 20)) != 0;

        /* Familia y modelo llevan la parte "extendida" sumada aparte desde que
         * los numeros base se agotaron; sin sumarla, media generacion de CPUs
         * sale con el mismo numero. */
        k.stepping = r1.eax & 0xF;
        const unsigned base_family = (r1.eax >> 8) & 0xF;
        const unsigned base_model = (r1.eax >> 4) & 0xF;
        k.family = base_family + ((base_family == 0xF) ? ((r1.eax >> 20) & 0xFF)
                                                       : 0);
        k.model = base_model + ((base_family == 0x6 || base_family == 0xF)
                                    ? (((r1.eax >> 16) & 0xF) << 4)
                                    : 0);

        /* El sistema tiene que estar guardando los registros anchos.  Sin esta
         * comprobacion la etiqueta diria AVX2 en una maquina donde usarlo seria
         * un error, que es peor que no decir nada. */
        const bool osxsave = (r1.ecx & (1u << 27)) != 0;
        const uint64_t xcr0 = osxsave ? vesta_mem_x86_xgetbv(0) : 0;
        const bool ymm_ok = (xcr0 & 0x6u) == 0x6u;
        const bool zmm_ok = ymm_ok && (xcr0 & 0xE0u) == 0xE0u;

        k.avx = ymm_ok && (r1.ecx & (1u << 28)) != 0;

        if (vesta_mem_x86_cpuid_max() >= 7) {
            const vesta_cpuid_regs r7 = vesta_mem_x86_cpuid(7, 0);
            k.avx2 = ymm_ok && (r7.ebx & (1u << 5)) != 0;
            k.avx512f = zmm_ok && (r7.ebx & (1u << 16)) != 0;
            k.avx512bw = zmm_ok && (r7.ebx & (1u << 30)) != 0;
            k.erms = (r7.ebx & (1u << 9)) != 0;
            k.clwb = (r7.ebx & (1u << 24)) != 0;
            k.movdiri = (r7.ecx & (1u << 27)) != 0;
            k.movdir64b = (r7.ecx & (1u << 28)) != 0;
            k.fsrm = (r7.edx & (1u << 4)) != 0;
            k.hybrid = (r7.edx & (1u << 15)) != 0;
        }

        /* En que clase de nucleo se esta corriendo AHORA.  Solo tiene sentido
         * en un procesador hibrido, y hay que leerlo con cuidado: el sistema
         * puede mover el proceso a otro nucleo a mitad de la tanda, asi que
         * esto es donde estaba al preguntar, no una promesa.  Aun asi vale la
         * pena apuntarlo: si dos tandas de la misma maquina no cuadran, es lo
         * primero que hay que mirar. */
        if (k.hybrid && vesta_mem_x86_cpuid_max() >= 0x1Au) {
            const unsigned t = (vesta_mem_x86_cpuid(0x1Au, 0).eax >> 24) & 0xFFu;
            if (t == 0x40u)
                k.core = "p-core";
            else if (t == 0x20u)
                k.core = "e-core";
        }

        k.threads = std::thread::hardware_concurrency();
        detect_caches(k);

        /* La cadena de marca son tres hojas de cuatro registros, en orden, sin
         * terminador propio: hay que ponerlo. */
        if (vesta_mem_x86_cpuid(0x80000000u, 0).eax >= 0x80000004u) {
            char b[49];
            for (unsigned i = 0; i < 3; ++i) {
                const vesta_cpuid_regs r =
                    vesta_mem_x86_cpuid(0x80000002u + i, 0);
                vesta_memcpy(b + i * 16 + 0, &r.eax, 4);
                vesta_memcpy(b + i * 16 + 4, &r.ebx, 4);
                vesta_memcpy(b + i * 16 + 8, &r.ecx, 4);
                vesta_memcpy(b + i * 16 + 12, &r.edx, 4);
            }
            b[48] = '\0';
            const char *p = b;
            while (*p == ' ')
                ++p; // suele venir con relleno delante
            k.brand = p;
        }
        return k;
    }();
    return c;
}

/**
 * @brief El nombre de la microarquitectura: Alder Lake, Zen 3, Skylake.
 *
 * POR QUE ESTE NOMBRE Y NO LAS BANDERAS.  Dos maquinas pueden tener las mismas
 * banderas y comportarse distinto en justo lo que aqui se mide: cuantos
 * almacenes por ciclo retira, si parte una escritura de 32 bytes que cruza
 * linea, a partir de que tamano le sale rentable `rep movsb`, cuanto tarda en
 * arrancarlo.  Eso no esta en ninguna bandera -- es la microarquitectura --, y
 * sin ella dos tandas comparables por sus banderas pueden no serlo.
 *
 * LA TABLA SOLO PONE NOMBRES.  No decide nada: ningun camino del codigo depende
 * de ella, asi que una entrada equivocada o que falte no puede cambiar una
 * medida, solo su etiqueta.  Por eso es aceptable que envejezca -- lo que no
 * seria aceptable es que decidiera.
 *
 * Y CUANDO NO SABE, LO DICE: un modelo que no este en la tabla no se aproxima
 * al vecino mas parecido, se queda en su numero exacto (`intel-f6m183`), que es
 * suficiente para buscarlo y no finge un nombre que podria ser el equivocado.
 *
 * @return El nombre, o cadena vacia si el modelo no esta en la tabla.
 */
inline const char *uarch_name() {
    const Cpu &k = cpu();

    if (k.vendor == "intel" && k.family == 6) {
        switch (k.model) {
        case 0x2A: case 0x2D: return "sandybridge";
        case 0x3A: case 0x3E: return "ivybridge";
        case 0x3C: case 0x3F: case 0x45: case 0x46: return "haswell";
        case 0x3D: case 0x47: case 0x4F: case 0x56: return "broadwell";
        case 0x4E: case 0x5E: return "skylake";
        /* El 0x55 es el Skylake de servidor y sus dos refritos.  Va aparte del
         * 0x4E/0x5E de sobremesa a proposito: lleva AVX-512 y baja de
         * frecuencia al usarlo, que en un banco de memoria se nota. */
        case 0x55: return "skylake-x";
        case 0x8E: case 0x9E: return "kabylake";
        case 0xA5: case 0xA6: return "cometlake";
        case 0x66: return "cannonlake";
        case 0x7D: case 0x7E: return "icelake";
        case 0x6A: case 0x6C: return "icelake-x";
        case 0x8C: case 0x8D: return "tigerlake";
        case 0xA7: return "rocketlake";
        case 0x97: case 0x9A: case 0xBE: return "alderlake";
        case 0xB7: case 0xBA: case 0xBF: return "raptorlake";
        case 0x8F: return "sapphirerapids";
        case 0xCF: return "emeraldrapids";
        case 0xAA: case 0xAC: return "meteorlake";
        case 0xBD: return "lunarlake";
        case 0xC6: return "arrowlake";
        /* Los nucleos pequenos cuando iban solos, antes de mezclarse con los
         * grandes en el mismo encapsulado. */
        case 0x5C: case 0x5F: return "goldmont";
        case 0x7A: return "goldmontplus";
        case 0x86: case 0x96: case 0x9C: return "tremont";
        default: return "";
        }
    }

    if (k.vendor == "amd") {
        /* AMD reparte generaciones por RANGOS de modelo dentro de una familia,
         * no uno a uno, asi que aqui se comparan intervalos. */
        if (k.family == 0x17) {
            if (k.model <= 0x0F || k.model == 0x18 || k.model == 0x20)
                return "zen";
            return "zen2";
        }
        if (k.family == 0x19) {
            const bool zen3 = k.model <= 0x0F || (k.model >= 0x20 &&
                                                  k.model <= 0x5F);
            return zen3 ? "zen3" : "zen4";
        }
        if (k.family == 0x1A) return "zen5";
        if (k.family == 0x15) return "bulldozer";
        if (k.family == 0x16) return "jaguar";
        return "";
    }

    return "";
}

#endif // VESTA_MEM_ARCH_X86

/**
 * @brief La microarquitectura, con su nombre si se conoce y con su numero si no.
 */
inline std::string uarch() {
#if defined(VESTA_MEM_ARCH_X86)
    const char *n = uarch_name();
    if (n[0] != '\0') return n;
    const Cpu &k = cpu();
    char b[64];
    std::snprintf(b, sizeof(b), "%s-f%um%u", k.vendor.c_str(), k.family,
                  k.model);
    return std::string(b);
#elif defined(__aarch64__)
    return "arm64";
#elif defined(__arm__)
    return "arm";
#else
    return "generic";
#endif
}

/**
 * @brief La etiqueta corta: microarquitectura mas el camino que de verdad usa.
 *
 * Vale para un nombre de fichero -- minusculas, guiones, sin espacios -- y se
 * lee de un vistazo: `alderlake-avx2-erms`, `zen3-avx2-erms`,
 * `skylake-avx2-erms-pinned`.
 *
 * VAN LAS DOS COSAS, y no sobra ninguna.  El nombre dice de que maquina se
 * trata, que es lo que gobierna cuanto cuesta una escritura ancha o desde que
 * tamano compensa `rep movsb`.  Las banderas dicen que camino se acabo
 * tomando, que no se deduce del nombre: en un binario de linea base corriendo
 * en Alder Lake, AVX2 se usa porque se detecto, mientras que otro compilado sin
 * el iria por SSE2 en la MISMA maquina.
 *
 * Lo demas -- el resto de banderas, la marca comercial, el escalon -- va en el
 * CSV, en sus columnas: en el nombre estorbaria, y ahi lo unico que hace falta
 * es decidir de un vistazo si dos tandas son comparables.
 */
inline std::string tag() {
    std::string s = uarch();
#if defined(VESTA_MEM_ARCH_X86)
    const Cpu &k = cpu();
#if defined(VESTA_MEM_ARCH_X86_64)
    s += k.avx512f ? "-avx512f"
                   : (k.avx2 ? "-avx2" : (k.avx ? "-avx" : "-sse2"));
#endif
    if (k.erms) s += "-erms";
#endif
    if (pinned()) s += "-pinned";
    return s;
}

/// Todas las banderas que se han mirado, para la columna del CSV.  Se apuntan
/// TAMBIEN las que hoy no cambian ningun camino: el dia que una de ellas se use,
/// las tandas viejas ya diran si la maquina la tenia.
inline std::string flags() {
#if defined(VESTA_MEM_ARCH_X86)
    const Cpu &k = cpu();
    std::string s;
    const char *names[] = {"sse2",     "ssse3",   "sse4.2",    "avx",
                           "avx2",     "avx512f", "avx512bw",  "erms",
                           "fsrm",     "clwb",    "movdiri",   "movdir64b",
                           "hybrid"};
    const bool on[] = {k.sse2,     k.ssse3,   k.sse42,     k.avx,
                       k.avx2,     k.avx512f, k.avx512bw,  k.erms,
                       k.fsrm,     k.clwb,    k.movdiri,   k.movdir64b,
                       k.hybrid};
    for (unsigned i = 0; i < sizeof(on) / sizeof(on[0]); ++i)
        if (on[i]) {
            if (!s.empty()) s += " ";
            s += names[i];
        }
    return s;
#else
    return "";
#endif
}

/// Cuantos hilos deja ver el sistema.  Es contexto del banco: en una maquina
/// ocupada, cuantos hay dice cuanta compania tuvo la medida.
inline unsigned threads() {
#if defined(VESTA_MEM_ARCH_X86)
    return cpu().threads;
#else
    return std::thread::hardware_concurrency();
#endif
}

/// En que clase de nucleo cayo la tanda, en los procesadores que tienen dos.
/// Vacio donde no aplique.  Ver la nota en `cpu`: es donde estaba al preguntar.
inline const char *core_kind() {
#if defined(VESTA_MEM_ARCH_X86)
    return cpu().core;
#else
    return "";
#endif
}

/**
 * @brief Lo mismo, pero preguntado AHORA en vez de recordado.
 *
 * `core_kind` guarda lo que se vio al arrancar, que es lo que se quiere en una
 * ficha del sistema.  Esta pregunta de nuevo, y hace falta para lo contrario:
 * comprobar que atar la medida a una clase de nucleos ha surtido efecto.  Con
 * la version guardada, una atadura que no funcionase seguiria diciendo que si.
 */
inline const char *current_core_kind() {
#if defined(VESTA_MEM_ARCH_X86)
    const Cpu &k = cpu();
    if (!k.hybrid || vesta_mem_x86_cpuid_max() < 0x1Au) return "";
    const unsigned t = (vesta_mem_x86_cpuid(0x1Au, 0).eax >> 24) & 0xFFu;
    if (t == 0x40u) return "p-core";
    if (t == 0x20u) return "e-core";
    return "";
#else
    return "";
#endif
}

/// Los tres niveles de cacha y la linea, en bytes, o cero donde no se sepa.
/// Son los numeros con los que se leen los codos de las curvas: una serie que
/// se cae entre dos tamanos se cayo casi siempre al salirse de un nivel.
inline unsigned long long l1d() {
#if defined(VESTA_MEM_ARCH_X86)
    return cpu().l1d;
#else
    return 0;
#endif
}
inline unsigned long long l2() {
#if defined(VESTA_MEM_ARCH_X86)
    return cpu().l2;
#else
    return 0;
#endif
}
inline unsigned long long l3() {
#if defined(VESTA_MEM_ARCH_X86)
    return cpu().l3;
#else
    return 0;
#endif
}
inline unsigned cache_line() {
#if defined(VESTA_MEM_ARCH_X86)
    return cpu().line;
#else
    return 0;
#endif
}

/// La marca que el procesador da de si mismo, o vacio donde no la haya.
inline std::string brand() {
#if defined(VESTA_MEM_ARCH_X86)
    return cpu().brand;
#else
    return "";
#endif
}

/// Familia, modelo y escalon, que identifican la microarquitectura EXACTA sin
/// depender de una tabla de nombres comerciales que envejece y se equivoca.
inline std::string signature() {
#if defined(VESTA_MEM_ARCH_X86)
    const Cpu &k = cpu();
    char b[48];
    std::snprintf(b, sizeof(b), "f%um%us%u", k.family, k.model, k.stepping);
    return std::string(b);
#else
    return "";
#endif
}

} // namespace isa

#endif // VESTA_ALLOC_SUPPORT_ISA_H
