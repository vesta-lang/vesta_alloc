/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file tests/util/test_host_spans.cpp
 * @brief Reservas GRANDES servidas por nosotros, y la capa en C.
 *
 * Hasta 2026-09 todo lo que pasaba de 2 KiB se le pedia al asignador del
 * sistema.  Ahora lo sirve un TRAMO de trozos de nuestra region, y eso trae
 * cosas nuevas que comprobar:
 *
 *  1. **Se sirven de verdad y estan bien puestas**: alineadas, sin solaparse y
 *     enteramente escribibles.  Un tramo mal medido no falla al reservar --
 *     falla al escribir el ultimo byte, mucho despues.
 *  2. **Se reusan.**  Si no, cada fase pediria trozos nuevos y la region se
 *     consumiria sin parar.
 *  3. **Y NO se van al sistema**: el contador de rendiciones tiene que quedarse
 *     donde estaba.  Es la unica forma de comprobar que el camino nuevo se usa
 *     de verdad y no que simplemente todo sigue cayendo al respaldo.
 *  4. **La capa en C hace lo que dice**, incluida la parte de `realloc` que mas
 *     se equivoca la gente: conservar el contenido y NO perder el bloque viejo
 *     cuando no puede crecer.
 */

#include "util/alloc/host_allocator.h"
#include "util/alloc/host_allocator_c.h"
#include "util/alloc/host_allocator_layout.h"
#include "util/mem/vesta_memset.h"

#include <cstdio>
#include <cstring>

namespace {

int failures = 0;

void check(bool ok, const char *what) {
    std::printf("  [%s] %s\n", ok ? "OK  " : "FALLO", what);
    if (!ok) ++failures;
}

/// Escribe un patron reconocible en todo el bloque y comprueba que sigue ahi.
bool write_and_verify(void *p, size_t n, unsigned char seed) {
    unsigned char *b = static_cast<unsigned char *>(p);
    for (size_t i = 0; i < n; ++i)
        b[i] = static_cast<unsigned char>(seed + (i & 0x7F));
    for (size_t i = 0; i < n; ++i)
        if (b[i] != static_cast<unsigned char>(seed + (i & 0x7F))) return false;
    return true;
}

} // namespace

int main() {
    std::printf("== reservas grandes y capa en C ==\n");
    if (!util::host_alloc_active()) {
        std::printf("  el asignador no esta en vigor: nada que comprobar\n");
        return 0;
    }

    /* Tamanos repartidos por toda la cola medida: desde justo por encima del
     * tope de las clases hasta varios megas.
     *
     * SE DERIVAN DE `kMaxSmall`, no se escriben.  Estaban escritos, y al subir
     * el tope de las clases la mitad dejaron de ser grandes sin que el test lo
     * supiera: seguia comprobando el camino de TRAMOS con reservas que ya no
     * pasaban por ahi.  Un test que lleva dentro una constante del codigo que
     * prueba deja de probarlo en cuanto esa constante cambia, y no avisa. */
    const size_t big = util::kMaxSmall + 1;
    const size_t sizes[] = {big,      big + 951,  big * 2,  big * 3,
                            100000,   500000,     1u << 20, 4u << 20};
    const int kN = int(sizeof(sizes) / sizeof(sizes[0]));

    // 1. Se sirven, estan alineadas, y son escribibles ENTERAS.
    {
        void *p[kN];
        bool aligned = true, writable = true;
        for (int i = 0; i < kN; ++i) {
            p[i] = util::host_alloc(sizes[i]);
            if (p[i] == nullptr) {
                writable = false;
                break;
            }
            if ((reinterpret_cast<uintptr_t>(p[i]) & (util::kAlign - 1)) != 0)
                aligned = false;
            if (!write_and_verify(p[i], sizes[i], (unsigned char)(i * 17)))
                writable = false;
        }
        check(aligned, "toda reserva grande sale alineada a 16");
        check(writable, "y se puede escribir hasta el ULTIMO byte");

        // Que dos bloques vivos no se pisen: comprobando que los patrones
        // siguen intactos DESPUES de haberlos escrito todos.
        bool intact = true;
        for (int i = 0; i < kN && intact; ++i) {
            const unsigned char *b = static_cast<unsigned char *>(p[i]);
            for (size_t j = 0; j < sizes[i]; ++j)
                if (b[j] != (unsigned char)(i * 17 + (j & 0x7F))) {
                    intact = false;
                    break;
                }
        }
        check(intact, "dos reservas grandes vivas nunca se solapan");
        for (int i = 0; i < kN; ++i)
            util::host_free(p[i]);
    }

    // 2. Se reusan: pedir y soltar en bucle no debe consumir region sin fin.
    {
        const util::HostAllocStats before = util::host_alloc_stats();
        for (int vuelta = 0; vuelta < 200; ++vuelta) {
            // Las dos POR ENCIMA del tope de las clases; ver la nota de arriba.
            void *a = util::host_alloc(util::kMaxSmall + 1);
            void *b = util::host_alloc(300000);
            util::host_free(a);
            util::host_free(b);
        }
        const util::HostAllocStats after = util::host_alloc_stats();
        const uint64_t fresh = after.bytes_reserved - before.bytes_reserved;
        // 400 reservas reusando dos tramos: como mucho tendria que haber
        // pedido esos dos.  Se deja margen por si otro hilo pide a la vez.
        check(fresh < 4u * 1024u * 1024u,
              "reservar y soltar en bucle REUSA en vez de pedir mas region");
        check(after.large_allocs - before.large_allocs == 400,
              "las 400 grandes se contaron como tales");
        check(after.large_frees - before.large_frees == 400,
              "y las 400 devoluciones tambien");
    }

    // 2b. El tramo pequeno vuelve a ESTE hilo sin pasar por el cerrojo.
    //
    // Se comprueba por la DIRECCION: si soltar y volver a pedir el mismo tamano
    // devuelve el mismo bloque, es que salio de la reserva del propio hilo.  Si
    // hubiera ido a las listas compartidas podria volver otro, y sobre todo
    // habria pasado por el cerrojo -- que es el 76% de lo que costaba este
    // camino, y la razon de que exista `kSpanCacheSlots`.
    {
        const size_t n = util::kMaxSmall + 1; // un solo trozo de tramo
        bool mismo = true;
        void *primero = util::host_alloc(n);
        for (int i = 0; i < 50; ++i) {
            util::host_free(primero);
            void *otra = util::host_alloc(n);
            if (otra != primero) mismo = false;
            primero = otra;
        }
        util::host_free(primero);
        check(mismo, "un tramo pequeno vuelve al hilo que lo solto, sin cerrojo");

        /* Y un tamano por encima de lo que se guarda tiene que seguir yendo por
         * el camino compartido: guardarlo todo retendria memoria en cada hilo.
         * Aqui no se exige que la direccion coincida, solo que siga sirviendo. */
        void *grande = util::host_alloc(size_t(util::kSpanCacheSlots + 2) *
                                        util::kChunkBytes);
        check(grande != nullptr, "y uno mayor sigue sirviendose por el comun");
        util::host_free(grande);
    }

    // 3. No se cae al sistema.  Es lo que distingue "funciona" de "sigue
    //    funcionando porque el respaldo lo tapa".
    {
        const util::HostAllocStats s = util::host_alloc_stats();
        (void)s;
        // El contador de rendiciones no esta en las estadisticas publicas; lo
        // que si se puede comprobar es que TODO lo grande paso por nosotros:
        // si se hubiera ido al sistema, `large_allocs` no habria subido.
        void *p = util::host_alloc(200000);
        const util::HostAllocStats t = util::host_alloc_stats();
        check(t.large_allocs > s.large_allocs,
              "una reserva grande la sirve el asignador, no el sistema");
        util::host_free(p);
    }

    // 4. La capa en C.
    {
        void *p = vesta_host_alloc(5000);
        check(p != nullptr && vesta_host_usable_size(p) >= 5000,
              "C: el tamano utilizable es al menos el pedido");

        vesta_memset(p, 0xAB, 5000);
        void *mas = vesta_host_realloc(p, 200000);
        bool conserva = mas != nullptr;
        if (conserva) {
            const unsigned char *b = static_cast<unsigned char *>(mas);
            for (int i = 0; i < 5000; ++i)
                if (b[i] != 0xAB) {
                    conserva = false;
                    break;
                }
        }
        check(conserva, "C: realloc que CRECE conserva el contenido");

        // Encoger dentro de la misma clase no tiene por que mover nada.
        void *menos = vesta_host_realloc(mas, 100);
        check(menos == mas, "C: realloc que cabe no mueve el bloque");
        vesta_host_free(menos);

        int *v = static_cast<int *>(vesta_host_calloc(1000, sizeof(int)));
        bool ceros = v != nullptr;
        for (int i = 0; ceros && i < 1000; ++i)
            if (v[i] != 0) ceros = false;
        check(ceros, "C: calloc entrega TODO a cero");
        vesta_host_free(v);

        check(vesta_host_calloc(size_t(-1) / 2, 4) == nullptr,
              "C: calloc detecta el desbordamiento en vez de reservar de menos");
        check(vesta_host_usable_size(nullptr) == 0,
              "C: el tamano de un puntero nulo es cero, no basura");
        vesta_host_free(nullptr); // no debe explotar
    }

    // 5. La etiqueta desde C.
    {
        const util::HostAllocStats before = util::host_alloc_stats();
        const unsigned previous = vesta_host_push_tag(1, 1); // instantaneo/fijo
        void *p = vesta_host_alloc(64);
        vesta_host_pop_tag(previous);
        util::host_free(p);
        const util::AllocTag t{util::AllocUse::Instant, util::AllocShape::Fixed};
        const util::HostAllocStats after = util::host_alloc_stats();
        check(after.by_tag[t.raw()] > before.by_tag[t.raw()],
              "C: la etiqueta puesta desde C cuenta donde toca");
        check(util::AllocScope::current().unknown(),
              "C: y al quitarla vuelve a \"no se\"");
    }

    std::printf(failures == 0 ? "TODO OK\n" : "%d FALLOS\n", failures);
    return failures == 0 ? 0 : 1;
}
