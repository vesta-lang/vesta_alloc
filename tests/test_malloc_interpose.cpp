/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file tests/test_malloc_interpose.cpp
 * @brief That `malloc` really is this allocator -- in C++ as well as in C.
 *
 * The point of the whole thing is that the CALLER changes nothing, so the
 * checks below call plain `malloc`, `calloc`, `realloc` and `free`, exactly as
 * any code in the tree does, and then ask the allocator whether the block that
 * came back is one of its own.  A test that called `vesta_host_alloc` would
 * pass whether or not the interposition works, which is the same as not
 * testing it.
 *
 * The foreign-block case gets its own checks because it is the one that can
 * only appear once this is in force, and it is the one that would corrupt
 * memory rather than fail loudly: the C runtime allocated during start-up and
 * hands blocks back for us to release.
 */

#include "util/alloc/host_allocator.h"
#include "util/alloc/host_allocator_layout.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/* COMO SE CONSIGUE UN BLOQUE AJENO, que es lo que hace falta para probar el
 * caso que solo existe cuando esto esta puesto.  Depende de por que via se
 * interpone, y las dos vias tienen fuentes distintas:
 *
 *  - renombrando al enlazar, el original sigue ahi con otro nombre;
 *  - definiendo el simbolo, no hay `__real_` de nada y hay que preguntarle al
 *    enlazador dinamico por el siguiente de la cadena -- lo mismo que hace
 *    `malloc_define.cpp` para encontrar el `free` de la libreria de C.
 *
 * `RTLD_NEXT` y no `RTLD_DEFAULT`: el segundo encontraria el NUESTRO, que es
 * el primero en el orden de busqueda, y no habria bloque ajeno ninguno. */
#if defined(VESTA_ALLOC_DEFINE_MALLOC)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <dlfcn.h>
namespace {
void *__real_malloc(size_t n) {
    static auto fn = reinterpret_cast<void *(*)(size_t)>(dlsym(RTLD_NEXT, "malloc"));
    return fn != nullptr ? fn(n) : nullptr;
}
} // namespace
#else
extern "C" {
void *__real_malloc(size_t n);
}
#endif

extern "C" {

/* DECLARADAS AQUI, y no incluyendo la cabecera del sistema, porque no todas
 * existen en todos los sitios: en Windows el CRT no trae ninguna de las tres.
 * Da igual: `--wrap` renombra la REFERENCIA al enlazar, asi que basta con que
 * la llamada exista para que acabe en nuestra entrada.  Y llamarlas por su
 * nombre es justamente lo que se quiere comprobar -- llamar a `__wrap_...`
 * pasaria aunque el renombrado no estuviera puesto. */
void *memalign(size_t align, size_t n);
void *aligned_alloc(size_t align, size_t n);
int posix_memalign(void **out, size_t align, size_t n);
}

#if defined(_WIN32)
/* Y EL PAR DE WINDOWS, POR SU CABECERA DE VERDAD.  Aqui no vale declararlo a
 * mano, que es lo contrario que arriba: la cabecera del CRT los marca
 * `dllimport`, y esa marca es lo que decide por que camino se les llama.
 * Declarandolos aqui saldria una referencia PLANA, la cogeria `--wrap`, y el
 * test pasaria sin haber tocado el mecanismo que de verdad los sostiene en
 * Windows -- el `__imp__` que define `malloc_interpose.cpp` --.  Con la
 * cabecera se llama como llama todo el mundo. */
#include <malloc.h>
/* `_aligned_msize` NO la declara la cabecera de MinGW, aunque msvcrt la
 * exporte.  Se declara aqui a mano, que ademas dice algo del caso real: un
 * llamante que la quiera usar tiene que hacer justo esto, y entonces su
 * referencia es PLANA -- la coge el renombrado del enlazador, no el puntero
 * `__imp__`.  Las dos vias llevan al mismo sitio, que es la gracia. */
extern "C" size_t _aligned_msize(void *p, size_t align, size_t offset);
#endif

namespace {

int g_failures = 0;

void check(bool ok, const char *what) {
    std::printf("  [%s] %s\n", ok ? "OK  " : "FAIL", what);
    if (!ok) ++g_failures;
}

bool ours(const void *p) {
    return util::in_region(p) || util::in_big_region(p);
}

/// Donde va a parar un puntero que hay que impedir que el optimizador borre.
/// `volatile` es lo que lo hace un efecto observable: una reserva que acaba
/// aqui ya no es codigo muerto para nadie.
void *volatile g_escape = nullptr;

/// A C++ translation unit calling `malloc` -- which is the half that was
/// missing: `operator new` was already replaced, this was not.
void small_and_large() {
    void *small = std::malloc(64);
    check(small != nullptr && ours(small),
          "malloc desde C++ devuelve un bloque NUESTRO");
    std::memset(small, 0xA5, 64); // que se pueda escribir entero
    std::free(small);

    /* Lo grande va a la region de las clases grandes, que tambien es nuestra.
     * Se comprueba aparte porque son dos caminos distintos y el segundo se
     * olvida con facilidad. */
    void *big = std::malloc(4u << 20);
    check(big != nullptr && ours(big),
          "y uno grande tambien, por la region de lo grande");
    std::memset(big, 0x5A, 4u << 20);
    std::free(big);
}

void calloc_zeroes_and_checks_overflow() {
    unsigned char *p = static_cast<unsigned char *>(std::calloc(128, 4));
    bool zeroed = p != nullptr && ours(p);
    if (p != nullptr)
        for (size_t i = 0; i < 128 * 4; ++i)
            if (p[i] != 0) zeroed = false;
    check(zeroed, "calloc devuelve un bloque nuestro y PUESTO A CERO");
    std::free(p);

    /* El desbordamiento del producto reserva de menos y deja escribir de mas.
     * Tiene que salir nulo, no un bloque pequeno.
     *
     * La cuenta pasa por una variable `volatile` para que el compilador no la
     * resuelva y avise de un tamano imposible: el aviso es correcto -- ese
     * tamano ES imposible -- pero lo que se comprueba aqui es justo que
     * pedirlo devuelva nulo en vez de reservar de menos. */
    volatile size_t count = size_t(-1) / 2 + 1;
    void *huge = std::calloc(count, 4);
    /* Y EL RESULTADO TIENE QUE ESCAPAR, o esto no comprueba nada con Clang.
     * A `-O3` el optimizador ve una reserva cuyo unico destino es compararse
     * con nulo y liberarse, la da por muerta y se lleva la llamada entera --
     * asi que la comprobacion del desbordamiento no llega a ejecutarse y el
     * test pasaba en GCC y fallaba en Clang por una razon que no tiene nada
     * que ver con el asignador.  Con `-O0` pasaba en los dos, que es como se
     * localizo.  Es lo mismo que ya obliga a `g_sink` en el ejemplo en C. */
    g_escape = huge;
    check(huge == nullptr, "y un calloc que se desbordaria devuelve nulo");
    std::free(huge);
}

void realloc_grows_and_keeps_the_data() {
    char *p = static_cast<char *>(std::malloc(32));
    std::memcpy(p, "esto tiene que sobrevivir", 26);
    char *q = static_cast<char *>(std::realloc(p, 4096));
    check(q != nullptr && ours(q) &&
              std::memcmp(q, "esto tiene que sobrevivir", 26) == 0,
          "realloc crece y NO pierde lo que habia");
    std::free(q);

    void *fresh = std::realloc(nullptr, 100);
    check(fresh != nullptr && ours(fresh),
          "realloc(NULL, n) reserva, como manda el estandar");
    std::free(fresh);
}

/**
 * @brief Un bloque que NO hicimos nosotros.
 *
 * Es el caso que solo existe cuando esto esta puesto, y el unico que
 * corromperia en vez de fallar de cara: el CRT reserva durante su arranque --
 * antes de que nada de esto exista -- y luego devuelve bloques para que los
 * suelte quien llama.  Aqui se imita pidiendoselos directamente al de verdad.
 */
void foreign_blocks_go_home() {
#if defined(VESTA_ALLOC_HOOK_MSVCRT)
    /* CON EL GANCHO PUESTO YA NO HAY BLOQUE AJENO QUE FABRICAR, y eso no es que
     * el test se quede corto: es la demostracion de lo que el gancho compra.
     * `__real_malloc` es el thunk que salta a `msvcrt!malloc`, y esa entrada
     * lleva ahora un salto al asignador -- asi que hasta lo que se pide por la
     * puerta de atras acaba aqui.  Comprobarlo AL REVES es lo unico que tiene
     * sentido ya. */
    void *by_the_back_door = __real_malloc(256);
    check(by_the_back_door != nullptr && ours(by_the_back_door),
          "con el gancho, hasta `msvcrt!malloc` devuelve un bloque NUESTRO");
    std::free(by_the_back_door);
    check(true, "y se suelta sin morir");
    std::free(nullptr);
    check(true, "free(NULL) no hace nada, como siempre");
}
#else
    void *foreign = __real_malloc(256);
    check(foreign != nullptr && !ours(foreign),
          "un bloque del CRT no se confunde con uno nuestro");
    /* Que no reviente es LA comprobacion: `host_free` mata el proceso ante un
     * puntero ajeno, y con razon.  El punto interpuesto es el unico sitio
     * donde eso es normal, y tiene que reenviarlo. */
    std::free(foreign);
    check(true, "y soltarlo por `free` lo devuelve al suyo sin morir");

    char *grow = static_cast<char *>(__real_malloc(16));
    std::memcpy(grow, "de fuera", 9);
    char *bigger = static_cast<char *>(std::realloc(grow, 512));
    check(bigger != nullptr && std::memcmp(bigger, "de fuera", 9) == 0,
          "y un realloc sobre uno ajeno lo estira quien lo hizo");
    std::free(bigger);

    std::free(nullptr);
    check(true, "free(NULL) no hace nada, como siempre");
}
#endif // VESTA_ALLOC_HOOK_MSVCRT

/**
 * @brief Una reserva alineada, y que la suelte el `free` de siempre.
 *
 * ESTO es lo que hay que comprobar y no que el puntero salga alineado, que es
 * la parte facil.  En C nadie llama a un `free_aligned`: el bloque tiene que
 * decir lo que es POR SI MISMO, y lo dice porque se sirve como un TRAMO, cuya
 * cabecera `free` ya encuentra enmascarando.  Por eso se mira la marca del
 * trozo: sin ella, soltarlo caeria en el camino de corrupcion, que no revienta
 * -- avisa por la salida de errores y sigue --, asi que un test que solo
 * mirara la alineacion pasaria con la memoria ya rota.
 */
void aligned_entries_are_freed_by_plain_free() {
    const size_t alignments[] = {16, 32, 64, 256, 4096, util::kChunkBytes / 2};
    bool all_aligned = true, all_ours = true, all_span = true;

    for (size_t i = 0; i < sizeof(alignments) / sizeof(alignments[0]); ++i) {
        const size_t a = alignments[i];
        void *p = nullptr;
        if (posix_memalign(&p, a, 300) != 0 || p == nullptr) {
            all_ours = false;
            continue;
        }
        if ((reinterpret_cast<uintptr_t>(p) & (a - 1)) != 0) all_aligned = false;
        if (!ours(p)) all_ours = false;
        /* Con 16 basta lo que el asignador ya da, asi que ese va por el camino
         * barato y NO es un tramo.  Por encima si tiene que serlo. */
        if (a > util::kAlign && util::chunk_of(p)->magic != util::kSpanMagic)
            all_span = false;
        if (util::host_usable_size(p) < 300) all_span = false;
        std::memset(p, 0x33, 300); // que se pueda escribir lo pedido
        std::free(p);              // el `free` de siempre, sin decirle nada
    }
    check(all_aligned, "posix_memalign devuelve punteros REALMENTE alineados");
    check(all_ours, "y son bloques nuestros, no del sistema");
    check(all_span,
          "y por encima de 16 vienen de un TRAMO, que es lo que hace que "
          "`free` los reconozca");

    void *c11 = aligned_alloc(128, 1000);
    check(c11 != nullptr && (reinterpret_cast<uintptr_t>(c11) & 127) == 0 &&
              ours(c11),
          "aligned_alloc, igual");
    std::free(c11);

    void *old = memalign(64, 500);
    check(old != nullptr && (reinterpret_cast<uintptr_t>(old) & 63) == 0 &&
              ours(old),
          "y memalign, que es la que llama el codigo viejo");
    std::free(old);
}

/// Lo que NO se puede servir se DICE.  Una alineacion que no es potencia de dos
/// no se puede redondear con una mascara, y una mayor que un trozo dejaria el
/// puntero en un trozo sin cabecera -- y ahi `free` no lo encontraria.
void impossible_alignments_are_refused() {
    void *p = reinterpret_cast<void *>(uintptr_t(1)); // un valor reconocible
    check(posix_memalign(&p, 24, 100) == EINVAL,
          "una alineacion que no es potencia de dos se rechaza");
    check(p == reinterpret_cast<void *>(uintptr_t(1)),
          "y al rechazar NO se toca lo que el llamante tenia ahi");
    check(posix_memalign(&p, 2, 100) == EINVAL,
          "ni una mas fina que un puntero, que es un error del llamante");
    /* EL LIMITE EXACTO, que es donde estaba el fallo.  Con la alineacion IGUAL
     * al trozo, redondear `trozo + 16` cae en la base del trozo SIGUIENTE -- que
     * no tiene cabecera --, asi que `free` no lo encontraria.  Se probo con
     * `<=` y este mismo test lo saco. */
    check(aligned_alloc(util::kChunkBytes, 100) == nullptr,
          "una alineacion de un trozo entero se rechaza: el puntero se iria al "
          "trozo siguiente, que no tiene cabecera");
    check(aligned_alloc(util::kChunkBytes * 2, 100) == nullptr,
          "y por encima, igual");
}

/// Crecer una reserva alineada.  Es donde el tamano util se calcula DESDE el
/// puntero y no desde la cabecera: con la cuenta vieja, `realloc` creeria que
/// hay `alineacion - 16` bytes de mas y devolveria el bloque corto tal cual.
void realloc_of_an_aligned_block() {
    void *p = nullptr;
    const size_t a = 4096;
    if (posix_memalign(&p, a, 64) != 0) {
        check(false, "no se pudo reservar el bloque alineado de partida");
        return;
    }
    std::memcpy(p, "alineado y con datos", 21);
    /* Se pide MAS de lo que queda desde el puntero hasta el final del tramo, que
     * es justo el caso que la cuenta vieja daba por bueno. */
    const size_t want = util::host_usable_size(p) + 1;
    char *q = static_cast<char *>(std::realloc(p, want));
    check(q != nullptr && ours(q) &&
              std::memcmp(q, "alineado y con datos", 21) == 0,
          "realloc de un bloque alineado crece y conserva los datos");
    check(q != nullptr && util::host_usable_size(q) >= want,
          "y lo que devuelve tiene DE VERDAD el tamano pedido");
    std::free(q);
}

#if defined(_WIN32)
/// En Windows el par esta en el contrato, asi que el bloque no tiene que
/// describirse solo y sale mucho mas barato.  Lo que hay que comprobar es que
/// el par CASA y que uno ajeno se devuelve al suyo.
void windows_aligned_pair() {
    void *p = _aligned_malloc(200, 64);
    check(p != nullptr && (reinterpret_cast<uintptr_t>(p) & 63) == 0 && ours(p),
          "_aligned_malloc alinea y es nuestro");
    std::memset(p, 0x77, 200);
    _aligned_free(p);
    check(true, "y _aligned_free lo suelta sin morir");

    _aligned_free(nullptr);
    check(true, "_aligned_free(NULL) no hace nada");
}

/**
 * @brief El RESTO de la familia alineada, que era el ultimo hueco.
 *
 * `_aligned_realloc` y sus parientes reciben un puntero YA reservado.  Sin
 * cubrirlas, una de nuestras reservas acababa en el monton del runtime -- y
 * eso no falla en la llamada: falla mucho despues, en otro sitio y sin nada
 * que apunte hasta aqui.  Que hoy no las llame nadie era una propiedad del
 * codigo de hoy, no una garantia.
 */
void windows_aligned_family() {
    char *p = static_cast<char *>(_aligned_malloc(64, 64));
    std::memcpy(p, "esto tiene que sobrevivir al crecimiento", 40);

    char *q = static_cast<char *>(_aligned_realloc(p, 4096, 64));
    check(q != nullptr && ours(q) &&
              (reinterpret_cast<uintptr_t>(q) & 63) == 0 &&
              std::memcmp(q, "esto tiene que sobrevivir al crecimiento", 40) == 0,
          "_aligned_realloc crece, sigue alineado y conserva los datos");
    check(q != nullptr && _aligned_msize(q, 64, 0) >= 4096,
          "y _aligned_msize dice un tamano que de verdad se puede usar");
    _aligned_free(q);

    void *fresh = _aligned_realloc(nullptr, 128, 32);
    check(fresh != nullptr && ours(fresh) &&
              (reinterpret_cast<uintptr_t>(fresh) & 31) == 0,
          "_aligned_realloc(NULL, ...) reserva, como manda el contrato");
    _aligned_free(fresh);

    /* La familia con DESPLAZAMIENTO no la sabemos servir -- devuelve un bloque
     * donde lo alineado es `p + offset`, y aqui no existe esa forma --, asi
     * que se rechaza EN LA LLAMADA.  Lo que no vale es dejarla pasar: iria a
     * msvcrt y su bloque acabaria en nuestro `_aligned_free`, o al reves. */
    check(_aligned_offset_malloc(64, 32, 8) == nullptr,
          "lo que no sabemos servir se rechaza en la llamada, no se deja "
          "pasar al runtime");
    void *at_zero = _aligned_offset_malloc(64, 32, 0);
    check(at_zero != nullptr && ours(at_zero),
          "y con desplazamiento cero, que es lo mismo que sin el, si se sirve");
    _aligned_free(at_zero);
}
#endif

#if defined(VESTA_ALLOC_DEFINE_MALLOC)
/**
 * @brief LO QUE JUSTIFICA definir el simbolo en vez de que lo renombren.
 *
 * `strdup` reserva DENTRO de la libreria de C y devuelve el bloque para que lo
 * suelte quien llama.  Esa reserva no es una referencia pendiente de ningun
 * enlace, asi que `--wrap` no puede tocarla por definicion: con el renombrado
 * esta comprobacion falla, y esa memoria no sale en el informe -- lo que se lee
 * como que no existe.
 *
 * Definiendo el simbolo si se alcanza, porque la libreria llama a `malloc` por
 * la PLT y ahi ganamos nosotros.  Si esta comprobacion se pusiera roja, esta
 * via habria dejado de comprar lo unico que compra, y mas valdria volver al
 * renombrado, que es mas simple.
 */
void reaches_inside_the_c_library() {
    char *s = ::strdup("una cadena que reserva la libreria de C por dentro");
    check(s != nullptr && ours(s),
          "lo que `strdup` reserva DENTRO de la libc es un bloque NUESTRO");
    std::free(s);

    /* Y con una reserva mayor, para que caiga en otro camino del asignador: lo
     * de arriba solo prueba el de bloques pequenos. */
    char big[4096];
    std::memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    char *b = ::strdup(big);
    check(b != nullptr && ours(b), "y tambien cuando lo que reserva es grande");
    std::free(b);
}
#endif

/// Que `new` siga yendo donde iba.  Interponer las entradas de C no puede
/// haber movido las de C++: son dos puertas al MISMO sitio.
void new_still_lands_here() {
    int *p = new int[64];
    check(ours(p), "y `new` sigue cayendo en el mismo asignador");
    delete[] p;
}

} // namespace

int main() {
    std::printf("== malloc, calloc, realloc y free SON este asignador ==\n");
    small_and_large();
    calloc_zeroes_and_checks_overflow();
    realloc_grows_and_keeps_the_data();
    foreign_blocks_go_home();
    aligned_entries_are_freed_by_plain_free();
    impossible_alignments_are_refused();
    realloc_of_an_aligned_block();
#if defined(_WIN32)
    windows_aligned_pair();
    windows_aligned_family();
#endif
#if defined(VESTA_ALLOC_DEFINE_MALLOC)
    reaches_inside_the_c_library();
#endif
    new_still_lands_here();

    if (g_failures == 0) {
        std::printf("TODO OK\n");
        return 0;
    }
    std::printf("%d COMPROBACIONES FALLIDAS\n", g_failures);
    return 1;
}
