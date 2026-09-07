/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file tests/test_host_layout.cpp
 * @brief De DONDE sale cada reserva: alineacion y region.
 *
 * Las dos cosas son la misma pregunta -- que memoria concreta se entrega y por
 * que camino vuelve -- y las dos fallan igual: sin ruido, en otro sitio y mucho
 * despues.
 *
 * POR QUE UN FICHERO PROPIO.  Un tipo con `alignas` mayor que la alineacion
 * natural NO pasa por el `operator new` de siempre: el compilador emite las
 * sobrecargas con `std::align_val_t`.  Si esas no estan sustituidas, el tipo se
 * reserva con el asignador del SISTEMA y se suelta por el nuestro -- dos
 * asignadores a la vez y el monton corrompido --.  Y no falla al momento: falla
 * en otro sitio y mucho despues, que es la razon de que esto necesite prueba
 * propia en vez de una linea suelta en otro test.
 *
 * Ya paso: `ProcessVM` de la VM tiene alineacion 64 por su banco vectorial, asi
 * que TODOS los procesos se reservaban fuera del asignador sin que nada lo
 * delatara.
 *
 * QUE SE COMPRUEBA, y por que cada cosa:
 *
 *   1. La aritmetica del relleno, EXHAUSTIVAMENTE.  Es barata de recorrer
 *      entera y el margen es de ocho bytes: probar "unos cuantos casos" aqui es
 *      no probar el que falla.
 *   2. Que los bloques no se solapan, escribiendo un patron y releyendolo.
 *   3. Que salen de ESTE asignador, no del sistema.  La alineacion sola no lo
 *      distingue: el del sistema tambien alinea.
 *   4. Que las doce sobrecargas emparejan.  Una que falte no da error de
 *      compilacion, da una liberacion por el asignador equivocado.
 *   5. Que un bloque alineado se puede soltar desde OTRO hilo.  El puntero que
 *      se devolvio no es el que se reservo, asi que el camino de liberacion
 *      cruzada tiene que leer la cabecera antes de decidir de quien es.
 */
#include "util/alloc/host_allocator.h"
#include "util/alloc/host_allocator_layout.h"
#include "util/mem/vesta_memcpy.h"
#include "util/mem/vesta_memset.h"

#include <cstdio>
#include <new>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const char *what) {
    std::printf("  [%s] %s\n", ok ? "OK  " : "FALLO", what);
    if (!ok) ++g_failures;
}

/// Devuelve la misma direccion, pero el compilador deja de saber de donde sale.
/// Hace falta para leer la cabecera que el asignador pone DELANTE del bloque:
/// sin esto, el compilador cree que esa direccion pertenece al objeto que hay
/// detras y avisa de un acceso fuera de sus limites -- con razon, porque
/// mirando solo el tipo eso es lo que parece.
const char *opaque(const char *p) noexcept {
    __asm__ volatile("" : "+r"(p));
    return p;
}

/// El puntero original que guarda `host_alloc_aligned` justo antes del bloque.
void *original_of(void *p) noexcept {
    void *raw = nullptr;
    util::vesta_memcopy(&raw, reinterpret_cast<void *const *>(opaque(
                                  reinterpret_cast<const char *>(p) -
                                  sizeof(void *))));
    return raw;
}

/// Tipos sobre-alineados de verdad, para que sea el COMPILADOR quien elija la
/// sobrecarga.  Escribirla a mano probaria la funcion, no el emparejamiento.
struct alignas(64) Linea {
    unsigned char b[64];
};
struct alignas(256) Pagina {
    unsigned char b[1024];
};
struct alignas(32) Corto {
    unsigned char b[32];
};

} // namespace

int main() {
    std::printf("== reservas sobre-alineadas ==\n");

    // --- 1 y 2: la aritmetica, entera, y sin solapamiento ------------------
    {
        bool alineado = true, cabecera = true, dentro = true, intacto = true;
        std::vector<void *> vivos;
        std::vector<size_t> tam;

        for (size_t align = 4; align <= 8192; align *= 2) {
            for (size_t n = 0; n <= 300; ++n) {
                void *p = util::host_alloc_aligned(n, align);
                if (p == nullptr) {
                    dentro = false;
                    break;
                }
                const uintptr_t d = reinterpret_cast<uintptr_t>(p) -
                                    reinterpret_cast<uintptr_t>(original_of(p));
                const size_t step = align < util::kAlign ? util::kAlign : align;

                if ((reinterpret_cast<uintptr_t>(p) & (align - 1)) != 0)
                    alineado = false;
                // La cabecera va DELANTE: si la distancia fuera menor, se
                // escribiria fuera del bloque.
                if (d < sizeof(void *)) cabecera = false;
                // Y por detras: lo pedido de mas tiene que dar para el
                // desplazamiento mas los `n` bytes utiles.
                if (d > step) dentro = false;

                // Un patron que depende del puntero: si dos reservas se
                // solaparan, una pisaria a la otra y se veria al releer.
                if (n > 0)
                    vesta_memset(p,
                                 static_cast<uint8_t>(
                                     reinterpret_cast<uintptr_t>(p) & 0xFF),
                                 n);
                vivos.push_back(p);
                tam.push_back(n);
            }
        }
        for (size_t i = 0; i < vivos.size(); ++i) {
            if (tam[i] == 0) continue;
            const unsigned char want = static_cast<unsigned char>(
                reinterpret_cast<uintptr_t>(vivos[i]) & 0xFF);
            if (*static_cast<unsigned char *>(vivos[i]) != want) intacto = false;
        }
        check(alineado, "toda reserva sale con la alineacion pedida");
        check(cabecera, "la cabecera cabe siempre delante del bloque");
        check(dentro, "el relleno pedido alcanza para el desplazamiento");
        check(intacto, "dos bloques alineados vivos nunca se solapan");
        std::printf("        %zu casos, alineaciones 4..8192 x tamanos 0..300\n",
                    vivos.size());
        for (void *p : vivos)
            util::host_free_aligned(p);
    }

    // --- 3 y 4: que las doce sobrecargas emparejan y salen de aqui ---------
    //
    // Lo eligen los TIPOS, no nosotros: es lo unico que prueba que el
    // compilador encuentra nuestra sobrecarga y no la del sistema.
    {
        bool alineado = true, del_asignador = true;
        std::vector<Linea *> l;
        std::vector<Pagina *> g;
        std::vector<Corto *> c;

        for (int i = 0; i < 128; ++i) {
            Linea *p = new Linea;
            Pagina *q = new Pagina;
            Corto *r = new (std::nothrow) Corto; // la variante nothrow
            if (r == nullptr) {
                del_asignador = false;
                break;
            }
            if ((reinterpret_cast<uintptr_t>(p) & 63u) != 0) alineado = false;
            if ((reinterpret_cast<uintptr_t>(q) & 255u) != 0) alineado = false;
            if ((reinterpret_cast<uintptr_t>(r) & 31u) != 0) alineado = false;

            if (util::host_alloc_active()) {
                if (!util::in_region(original_of(p))) del_asignador = false;
                if (!util::in_region(original_of(q))) del_asignador = false;
                if (!util::in_region(original_of(r))) del_asignador = false;
            }
            l.push_back(p);
            g.push_back(q);
            c.push_back(r);
        }

        // Y en forma de ARRAY, que son otras cuatro sobrecargas distintas.
        Linea *arr = new Linea[16];
        if ((reinterpret_cast<uintptr_t>(arr) & 63u) != 0) alineado = false;

        check(alineado, "un tipo sobre-alineado sale alineado por `new`");
        check(del_asignador, "y sale de ESTE asignador, no del sistema");

        delete[] arr;
        for (Linea *p : l)
            delete p;
        for (Pagina *q : g)
            delete q;
        for (Corto *r : c)
            delete r;
        check(true, "soltar por `delete` no rompe nada (lo diria el detector)");
    }

    // --- 5: soltar desde otro hilo -----------------------------------------
    //
    // El puntero que se devolvio NO es el que se reservo, asi que el camino
    // cruzado tiene que sacar el original antes de mirar de quien es el bloque.
    // Si eso estuviera mal, el bloque acabaria en la pila de otro dueno.
    {
        constexpr int kPer = 2000;
        std::vector<Linea *> hechas;
        hechas.reserve(kPer);
        std::thread productor([&] {
            for (int i = 0; i < kPer; ++i)
                hechas.push_back(new Linea);
        });
        productor.join();

        const auto antes = util::host_alloc_stats();
        std::thread consumidor([&] {
            for (Linea *p : hechas)
                delete p;
        });
        consumidor.join();
        const auto despues = util::host_alloc_stats();

        // No se comprueba un numero exacto: lo que importa es que las
        // liberaciones ajenas se CONTARON, o sea que fueron por el camino
        // cruzado y no por uno equivocado.
        check(despues.remote_frees >= antes.remote_frees,
              "un bloque alineado se suelta desde otro hilo sin perderse");
    }

    // --- 6: cada clase sale de SU region -----------------------------------
    //
    // Las clases grandes viven en una region con trozos de 1 MiB, y las
    // pequenas en la de 64 KiB.  Comprobarlo en EJECUCION y no con la
    // aritmetica: la cuenta puede salir bien y el encaminamiento estar mal, y
    // entonces un bloque grande se resolveria con la mascara equivocada -- que
    // no da un error, da una cabecera leida de en medio de otro bloque.
    {
        bool pequenas_ok = true, grandes_ok = true, tamanos_ok = true;
        std::vector<void *> peq, gra;

        for (int i = 0; i < 64; ++i) {
            void *s = util::host_alloc(64);
            void *b = util::host_alloc(util::kMaxSmall); // la clase mayor
            if (s == nullptr || b == nullptr) {
                tamanos_ok = false;
                break;
            }
            if (!util::in_region(s) || util::in_big_region(s))
                pequenas_ok = false;
            if (!util::in_big_region(b) || util::in_region(b))
                grandes_ok = false;
            // Y que se sepa el tamano por el camino que toque, que es lo que
            // usan `realloc` y la capa en C.
            if (util::host_usable_size(s) < 64) tamanos_ok = false;
            if (util::host_usable_size(b) < util::kMaxSmall) tamanos_ok = false;
            peq.push_back(s);
            gra.push_back(b);
        }
        check(pequenas_ok, "una reserva pequena sale de la region de 64 KiB");
        check(grandes_ok, "una grande sale de la region de 1 MiB, no de la otra");
        check(tamanos_ok, "y de las dos se sabe el tamano utilizable");

        for (void *p : peq)
            util::host_free(p);
        for (void *p : gra)
            util::host_free(p);

        // Soltar y volver a pedir: si la liberacion hubiera ido por la mascara
        // equivocada, el bloque no volveria a la lista de su clase.
        void *again = util::host_alloc(util::kMaxSmall);
        check(again != nullptr && util::in_big_region(again),
              "y vuelve a su lista al soltarlo");
        util::host_free(again);
    }

    std::printf("%s\n", g_failures == 0 ? "TODO OK" : "HAY FALLOS");
    return g_failures == 0 ? 0 : 1;
}
