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

#include "util/mem/vesta_memcpy.h"
#include "util/mem/vesta_memset.h"

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

    /* Con 1 se le dice que NO se sabe nada de la alineacion, que es el caso
     * que hay que probar: es el camino con prologo. */
    vesta_mem_sse2_copy(dst.data(), src.data(), kN, 1);
    check(dst == src, "sse2_copy: contenido");
    vesta_mem_sse2_fill(dst.data(), 0x11, kN, 1);
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

/// Un tipo con alineacion declarada, que es lo que la capa con tipo aprovecha.
struct alignas(32) Aligned32 {
    uint8_t b[256];
};
/// Y uno sin nada declarado, que tiene que seguir funcionando igual.
struct Plain {
    uint8_t b[100];
};

/**
 * @brief Que la capa con TIPO de C++ de lo mismo que la de bytes.
 *
 * Es codigo DISTINTO -- se salta el prologo de alineacion porque el tipo se lo
 * garantiza --, asi que probar solo la de bytes lo dejaria sin mirar.  Y la
 * garantia hay que comprobarla en las dos direcciones: con un tipo alineado,
 * que es donde se salta el prologo, y con uno plano, donde NO puede saltarselo.
 */
void typed_layer() {
    /* Un objeto suelto, alineado. */
    {
        Aligned32 src, dst;
        fill_pattern(src.b, sizeof src.b, 11);
        util::vesta_memfill(&dst, 0);
        check(dst.b[0] == 0 && dst.b[255] == 0, "vesta_memfill: un objeto");
        util::vesta_memcopy(&dst, &src);
        check(std::memcmp(dst.b, src.b, sizeof src.b) == 0,
              "vesta_memcopy: un objeto alineado");
    }
    /* Un array, con la cuenta en EJECUCION: ahi el tamano no es constante pero
     * la alineacion si, que es la mitad de lo que aporta el tipo. */
    for (size_t n = 0; n <= 5; ++n) {
        std::vector<Aligned32> src(n + 1), dst(n + 1);
        for (size_t i = 0; i < n; ++i)
            fill_pattern(src[i].b, sizeof src[i].b, unsigned(i) + 3);
        util::vesta_memcopy(dst.data(), src.data(), n);
        check(n == 0 || std::memcmp(dst.data(), src.data(),
                                    n * sizeof(Aligned32)) == 0,
              "vesta_memcopy: array alineado");
    }
    /* Y un tipo SIN alineacion declarada y de tamano que no es potencia de dos,
     * que es donde no se puede saltar nada. */
    {
        Plain src, dst;
        fill_pattern(src.b, sizeof src.b, 17);
        util::vesta_memfill(&dst, 0xEE);
        check(dst.b[0] == 0xEE && dst.b[99] == 0xEE,
              "vesta_memfill: tipo plano");
        util::vesta_memcopy(&dst, &src);
        check(std::memcmp(dst.b, src.b, sizeof src.b) == 0,
              "vesta_memcopy: tipo plano");
    }
}

/**
 * @brief Las variantes que SI llaman.
 *
 * Tienen su propio cuerpo -- no son un alias -- asi que si nadie las ejecuta,
 * nadie las prueba.
 */
void noinline_variants() {
    for (size_t n = 0; n <= 300; n += 7) {
        Guarded dst(n);
        std::vector<uint8_t> src(n + kPad);
        fill_pattern(src.data(), src.size(), 23);

        util::vesta_memcpy_noinline(dst.data(), src.data(), n);
        check(std::memcmp(dst.data(), src.data(), n) == 0,
              "vesta_memcpy_noinline: contenido");
        check(dst.intact(n), "vesta_memcpy_noinline: se escribio fuera");

        Guarded f(n);
        std::vector<uint8_t> ref(n, 0x7A);
        util::vesta_memset_noinline(f.data(), 0x7A, n);
        check(std::memcmp(f.data(), ref.data(), n) == 0,
              "vesta_memset_noinline: contenido");
        check(f.intact(n), "vesta_memset_noinline: se escribio fuera");
    }
}

/**
 * @brief Rellenar un bloque MAYOR QUE LA CACHE, que es otro camino.
 *
 * Pasado el ultimo nivel de cache el relleno deja de escribir por las caches y
 * usa almacenes no temporales, que son la clase de instruccion con la que un
 * error no se nota: escriben bien el grueso y se salen por la cabeza o por la
 * cola, o dejan de ser visibles por faltar la barrera.  Nada de eso lo pilla un
 * caso de 300 bytes, que es hasta donde llegaba este fichero.
 *
 * El tamano se DERIVA de la cache, no se escribe: en otra maquina el camino
 * empieza en otro sitio, y un test con la cifra dentro dejaria de probarlo sin
 * decir nada.
 */
void past_the_cache() {
#if !defined(VESTA_MEM_ARCH_X86_64)
    /* DICIENDOLO.  Sin esta linea, un fichero donde la macro de arquitectura no
     * llegue compila el caso entero fuera y el test sale verde habiendo
     * probado cero -- que es peor que fallar. */
    std::printf("  (no es x86-64: no hay camino no temporal que comprobar)\n");
#else
    const unsigned int llc = vesta_mem_x86_llc_bytes();
    if (llc == VESTA_MEM_X86_LLC_UNKNOWN) {
        std::printf("  (la CPU no describe su cache: el camino no temporal no "
                    "se usa aqui, nada que comprobar)\n");
        return;
    }
    std::printf("  cache de ultimo nivel: %.1f MiB -> se prueban %.0f y %.0f\n",
                (double)llc / 1048576.0, (double)(llc + (1u << 20)) / 1048576.0,
                (double)(llc - (1u << 20)) / 1048576.0);

    /* Uno por encima de la raya y otro por debajo: el primero recorre el camino
     * nuevo, el segundo confirma que la raya esta donde se cree y que el de
     * siempre sigue dando lo mismo. */
    const size_t sizes[] = {size_t(llc) + (1u << 20), size_t(llc) - (1u << 20)};
    for (size_t n : sizes) {
        // Desalineados a proposito, que es donde viven la cabeza y la cola.
        for (size_t off : {size_t(0), size_t(1), size_t(17), size_t(31)}) {
            Guarded dst(n + off);
            util::vesta_memset(dst.data() + off, 0x3C, n);
            bool ok = true;
            const uint8_t *p = dst.data() + off;
            for (size_t i = 0; i < n && ok; ++i)
                if (p[i] != 0x3C) ok = false;
            check(ok, "vesta_memset pasada la cache: contenido");
            check(dst.intact(n + off),
                  "vesta_memset pasada la cache: se escribio fuera");
        }
    }

    /* Y las dos rutinas a pelo con tamanos que el despacho NUNCA les manda --
     * por debajo de su propio minimo --, porque esa rama existe: son publicas y
     * quien las llame no tiene por que saber donde empieza a compensar. */
    for (size_t n : {size_t(0), size_t(1), size_t(15), size_t(31), size_t(63),
                     size_t(127)}) {
        Guarded a(n);
        vesta_mem_nt_fill16(a.data(), 0x77, n);
        bool ok16 = true;
        for (size_t i = 0; i < n; ++i)
            if (a.data()[i] != 0x77) ok16 = false;
        check(ok16 && a.intact(n), "nt_fill16 por debajo de su minimo");

        if (vesta_mem_x86_has_avx2()) {
            Guarded b(n);
            vesta_mem_nt_fill32(b.data(), 0x77, n);
            bool ok32 = true;
            for (size_t i = 0; i < n; ++i)
                if (b.data()[i] != 0x77) ok32 = false;
            check(ok32 && b.intact(n), "nt_fill32 por debajo de su minimo");
        }
    }
#endif
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
    past_the_cache();
    inline_variants();
    noinline_variants();
    typed_layer();

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
