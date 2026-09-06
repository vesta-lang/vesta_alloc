/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file tests/util/test_mem_ops.cpp
 * @brief Que copiar, rellenar y mover den EXACTAMENTE lo mismo que la libc.
 *
 * Un asignador que se escribe su propio @c memcpy tiene que demostrarlo, y la
 * unica demostracion que vale es comparar contra la implementacion de
 * referencia byte a byte.  Lo que se comprueba, y por que cada cosa:
 *
 *  1. **Todas las longitudes de 0 a 300**, no una muestra.  Es donde estan los
 *     fallos de este tipo de codigo: en los saltos entre caminos (16, 32, 64,
 *     128) y en el byte de mas o de menos de la cola.  Trescientas longitudes
 *     cruzan todos los saltos con margen.
 *  2. **Todos los DESALINEAMIENTOS de 0 a 15**, en origen y destino por
 *     separado.  Los movimientos vectoriales de aqui son no alineados a
 *     proposito; si alguno se colara alineado, esto es lo que lo caza -- y no
 *     con un resultado raro, sino con una violacion de segmento.
 *  3. **Que no se escriba NI UN BYTE fuera.**  El truco de los bloques
 *     solapados escribe dos veces en el medio; equivocarse ahi pisa lo de al
 *     lado, que es un fallo que no se ve en el resultado de la propia copia.
 *     Por eso hay centinelas alrededor de cada destino.
 *  4. **El solapamiento, en los dos sentidos.**  Hacia atras es un camino
 *     APARTE, y solo se ejerce si el destino esta por delante del origen.
 *  5. **Los dos caminos de despacho, no solo el que tenga esta maquina.**  Se
 *     llama a mano al de SSE2 y al de AVX2 -- este ultimo solo si la CPU lo
 *     admite -- para que una maquina con AVX2 no deje el camino base sin
 *     probar, que es como se quedaria sin cubrir la mitad del fichero.
 *  6. **Y las versiones que no llaman a nadie**, que son codigo DISTINTO: se
 *     quedan en el camino base y tienen ademas una rama para cuando el tamano
 *     es constante.  Sin esto, en una maquina con AVX2 no las mira nadie.
 */

#include "util/vesta_memcpy.h"
#include "util/vesta_memset.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char *what) {
    if (!ok) {
        std::printf("  [FALLO] %s\n", what);
        ++failures;
    }
}

/// Relleno de los centinelas: un valor que no sale de ningun patron de prueba.
constexpr uint8_t kGuard = 0xA5;
constexpr size_t kPad = 32; ///< centinelas a cada lado

/**
 * @brief Un bufer con centinelas a los lados.
 *
 * @c data() apunta al primer byte util; lo de antes y lo de despues tiene que
 * seguir siendo @c kGuard al terminar.
 */
struct Guarded {
    std::vector<uint8_t> raw;
    explicit Guarded(size_t n) : raw(n + 2 * kPad, kGuard) {}
    uint8_t *data() noexcept { return raw.data() + kPad; }
    bool intact(size_t n) const noexcept {
        for (size_t i = 0; i < kPad; ++i)
            if (raw[i] != kGuard) return false;
        for (size_t i = 0; i < kPad; ++i)
            if (raw[kPad + n + i] != kGuard) return false;
        return true;
    }
};

/// Datos reconocibles: cada byte depende de su posicion.
void fill_pattern(uint8_t *p, size_t n, unsigned seed) noexcept {
    for (size_t i = 0; i < n; ++i)
        p[i] = static_cast<uint8_t>((i * 31u + seed) & 0xFF);
}

// -------------------------------------------------------------------------

/// @brief Copiar @p n bytes con los desalineamientos @p da y @p ds.
void one_copy(size_t n, size_t da, size_t ds) {
    Guarded dst(n + da);
    std::vector<uint8_t> src(n + ds + kPad);
    fill_pattern(src.data(), src.size(), 7);

    util::vesta_memcpy(dst.data() + da, src.data() + ds, n);

    check(std::memcmp(dst.data() + da, src.data() + ds, n) == 0,
          "vesta_memcpy: contenido");
    check(dst.intact(n + da), "vesta_memcpy: se escribio fuera del destino");
}

/// @brief Rellenar @p n bytes con el desalineamiento @p da.
void one_fill(size_t n, size_t da) {
    Guarded dst(n + da);
    std::vector<uint8_t> ref(n, 0x5C);

    util::vesta_memset(dst.data() + da, 0x5C, n);

    check(std::memcmp(dst.data() + da, ref.data(), n) == 0,
          "vesta_memset: contenido");
    check(dst.intact(n + da), "vesta_memset: se escribio fuera del destino");
}

/**
 * @brief Mover con solape, comparando contra @c memmove.
 *
 * @param n     Bytes a mover.
 * @param shift Desplazamiento del destino respecto al origen.  Positivo mueve
 *              hacia delante (el caso que obliga a recorrer HACIA ATRAS),
 *              negativo hacia atras.
 */
void one_move(size_t n, int shift) {
    const size_t span = n + 64;
    std::vector<uint8_t> ours(span), ref(span);
    fill_pattern(ours.data(), span, 13);
    ref = ours;

    const size_t src_off = 32;
    const size_t dst_off = static_cast<size_t>(int(src_off) + shift);

    util::vesta_memmove(ours.data() + dst_off, ours.data() + src_off, n);
    std::memmove(ref.data() + dst_off, ref.data() + src_off, n);

    check(ours == ref, "vesta_memmove: no coincide con memmove");
}

/// @brief Los caminos de despacho, llamados a mano para cubrir los dos.
void dispatch_paths() {
#if defined(VESTA_MEM_ARCH_X86)
    constexpr size_t kN = 517; // cruza 128, 64, 32, 16 y deja cola
    std::vector<uint8_t> src(kN), dst(kN), ref(kN, 0x11);
    fill_pattern(src.data(), kN, 3);

    vesta_mem_sse2_copy(dst.data(), src.data(), kN);
    check(dst == src, "sse2_copy: contenido");
    vesta_mem_sse2_fill(dst.data(), 0x11, kN);
    check(dst == ref, "sse2_fill: contenido");

#if defined(VESTA_MEM_ARCH_X86_64)
    if (vesta_mem_x86_has_avx2()) {
        vesta_mem_avx2_copy(dst.data(), src.data(), kN);
        check(dst == src, "avx2_copy: contenido");
        vesta_mem_avx2_fill(dst.data(), 0x11, kN);
        check(dst == ref, "avx2_fill: contenido");
    } else {
        std::printf("  (esta CPU no tiene AVX2: ese camino no se prueba)\n");
    }
#endif
#endif
}

/**
 * @brief Que la version que NO llama a nadie de el mismo resultado.
 *
 * Renuncia a AVX2, asi que es codigo DISTINTO del que prueba todo lo de arriba:
 * si solo se comprobara @c vesta_memcpy, en una maquina con AVX2 el camino de
 * @c vesta_memcpy_inline no lo miraria nadie.
 */
void inline_variants() {
    for (size_t n = 0; n <= 200; ++n) {
        Guarded dst(n);
        std::vector<uint8_t> src(n + kPad);
        fill_pattern(src.data(), src.size(), 5);

        util::vesta_memcpy_inline(dst.data(), src.data(), n);
        check(std::memcmp(dst.data(), src.data(), n) == 0,
              "vesta_memcpy_inline: contenido");
        check(dst.intact(n), "vesta_memcpy_inline: se escribio fuera");

        Guarded f(n);
        std::vector<uint8_t> ref(n, 0x3B);
        util::vesta_memset_inline(f.data(), 0x3B, n);
        check(std::memcmp(f.data(), ref.data(), n) == 0,
              "vesta_memset_inline: contenido");
        check(f.intact(n), "vesta_memset_inline: se escribio fuera");
    }

    /* Con tamano CONSTANTE toman el otro camino -- lo expande el compilador --,
     * asi que hay que ejercerlo a proposito: no lo cubre el bucle de arriba. */
    struct Header {
        uint64_t a, b;
        uint32_t c;
    };
    Header h;
    util::vesta_memset_inline(&h, 0, sizeof h);
    check(h.a == 0 && h.b == 0 && h.c == 0, "inline con tamano constante");
    Header copy;
    util::vesta_memcpy_inline(&copy, &h, sizeof h);
    check(copy.a == 0 && copy.b == 0 && copy.c == 0,
          "inline con tamano constante: copia");
}

} // namespace

int main() {
    std::printf("== primitivas de memoria ==\n");

    for (size_t n = 0; n <= 300; ++n)
        for (size_t off = 0; off < 16; ++off) {
            one_copy(n, off, 0);
            one_copy(n, 0, off);
            one_fill(n, off);
        }

    for (size_t n = 1; n <= 200; ++n)
        for (int shift = -17; shift <= 17; ++shift)
            one_move(n, shift);

    dispatch_paths();
    inline_variants();

    /* Casos de longitud cero: tienen que ser un no-op limpio, no un acceso a
     * una direccion que quiza no sea valida. */
    util::vesta_memcpy(nullptr, nullptr, 0);
    util::vesta_memset(nullptr, 0, 0);
    util::vesta_memmove(nullptr, nullptr, 0);

    if (failures == 0) {
        std::printf("TODO OK\n");
        return 0;
    }
    std::printf("FALLOS: %d\n", failures);
    return 1;
}
