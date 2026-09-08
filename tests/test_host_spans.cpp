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

    /* 6. Lo que pasa de `kMaxSpanBytes` lo sirve el SISTEMA en una reserva
     *    propia, y hay que reconocerlo al soltarlo.
     *
     * Es el camino con mas formas de romperse en silencio de todo el fichero:
     * un bloque asi cae FUERA de las dos regiones, asi que si la tabla no lo
     * situa, soltarlo no da un valor raro -- para el proceso --.  Y si lo situa
     * pero no lo saca, la tabla se llena y el camino se apaga solo, que es
     * exactamente el tipo de degradacion muda que este proyecto no admite. */
    {
        const size_t n = util::kMaxSpanBytes + 1; // el primero que pasa la raya
        const uint64_t d0 = util::host_direct_allocs();

        void *p = util::host_alloc(n);
        check(p != nullptr, "grande: se sirve una reserva mayor que un tramo");
        check(util::host_direct_allocs() > d0,
              "grande: y la sirve el SISTEMA, no la region");
        if (p != nullptr) {
            check((reinterpret_cast<uintptr_t>(p) & (util::kAlign - 1)) == 0,
                  "grande: sale alineada");
            check(write_and_verify(p, n, 0x5A),
                  "grande: se escribe hasta el ULTIMO byte");
            check(util::host_usable_size(p) >= n,
                  "grande: dice un tamano utilizable de al menos lo pedido");
        }

        // Crecer: no se estira en su sitio, pero tiene que conservar todo.
        void *mas = util::host_realloc(p, n + (1u << 20));
        bool conserva = mas != nullptr;
        if (conserva) {
            const unsigned char *b = static_cast<unsigned char *>(mas);
            for (size_t i = 0; i < n; i += 4096)
                if (b[i] != (unsigned char)(0x5A + (i & 0x7F))) {
                    conserva = false;
                    break;
                }
        }
        check(conserva, "grande: realloc que crece conserva el contenido");
        util::host_free(mas);

        // Y a cero de verdad, que es la mitad de por que existe este camino:
        // las paginas llegan limpias del sistema y nadie las repasa.
        unsigned char *z = static_cast<unsigned char *>(util::host_alloc_zeroed(n));
        bool ceros = z != nullptr;
        for (size_t i = 0; ceros && i < n; i += 4096)
            if (z[i] != 0) ceros = false;
        check(ceros, "grande: host_alloc_zeroed entrega TODO a cero");
        util::host_free(z);

        /* LA TABLA SE VACIA.  Mas vueltas que ranuras tiene: si soltar no
         * sacara el bloque, a partir de la 512 no cabria ninguno mas y el
         * contador de rechazos empezaria a subir -- seguiria funcionando, por
         * el camino lento, sin que nada lo dijera. */
        const uint64_t refused0 = util::host_direct_refused();
        for (int vuelta = 0; vuelta < int(util::kDirectSlots) + 100; ++vuelta) {
            void *q = util::host_alloc(n);
            util::host_free(q);
        }
        check(util::host_direct_refused() == refused0,
              "grande: soltar VACIA la ranura; la tabla no se llena en un bucle");

        /* UN PUNTERO SUBIDO A SU ALINEACION DENTRO DEL BLOQUE.  Es el caso que
         * este camino casi rompe: `host_alloc_aligned_freeable` promete que el
         * `host_free` NORMAL lo suelta, y en la region eso funciona porque la
         * mascara encuentra la cabecera desde cualquier sitio del trozo.  Aqui
         * no hay cabecera que encontrar, asi que lo situa la tabla -- y si solo
         * mirara la base, esto pararia el proceso.  En Windows no se veria: el
         * sistema entrega direcciones alineadas a 64 KiB y la subida no mueve
         * nada; en ELF, alineado solo a pagina, si. */
        const size_t alineaciones[] = {64, 4096, 32768};
        for (size_t align : alineaciones) {
            void *a = util::host_alloc_aligned_freeable(n, align);
            check(a != nullptr &&
                      (reinterpret_cast<uintptr_t>(a) & (align - 1)) == 0,
                  "grande: aligned_freeable entrega un puntero alineado");
            if (a != nullptr) {
                check(util::host_usable_size(a) >= n,
                      "grande: y el sitio se cuenta DESDE el, no desde la base");
                check(write_and_verify(a, n, 0x33),
                      "grande: alineado, escribible hasta el ultimo byte");
            }
            util::host_free(a); // el NORMAL: si no lo reconoce, para el proceso
        }

        /* Y EL CASO A PELO, porque el de arriba depende de la suerte: hoy
         * `mmap` devuelve bases ya alineadas a 64 KiB y la subida no mueve el
         * puntero, asi que en esta maquina no llega a probar nada -- pero el
         * sistema solo promete PAGINA, y donde no coincida el puntero se movera
         * y habra que situarlo igual.  Aqui se desplaza a mano, que es
         * determinista en las dos plataformas. */
        unsigned char *base = static_cast<unsigned char *>(util::host_alloc(n));
        if (base != nullptr) {
            const size_t entero = util::host_usable_size(base);
            unsigned char *dentro = base + 4096;
            check(util::host_usable_size(dentro) == entero - 4096,
                  "grande: un puntero de DENTRO se situa, y el sitio se resta");
            util::host_free(dentro); // suelta el bloque entero, desde el medio
            check(true, "grande: y soltarlo desde el medio no mata el proceso");
        }

        /* Y LA REGION NO SE CONSUME.  Un tramo de este tamano no cabia en el
         * banco, asi que sus trozos no se repartian nunca mas: el cursor solo
         * avanzaba.  Medido antes de este camino, 16.320 reservas de 16 MiB
         * agotaban una region de 256 GiB **sin nada vivo**, y a partir de ahi
         * el asignador contestaba nulo -- o sea `operator new` lanzando en un
         * programa que no retenia ni un byte.
         *
         * Las vueltas se DERIVAN de la region que se haya conseguido, no se
         * escriben: en una maquina que solo consiga el minimo, veinte mil
         * sobrarian y aqui harian falta muchas menos. */
        const size_t vueltas = util::host_region_reserved() / n + 16;
        bool servidas = true;
        for (size_t v = 0; v < vueltas && servidas; ++v) {
            void *q = util::host_alloc(n);
            if (q == nullptr) servidas = false;
            util::host_free(q);
        }
        check(servidas,
              "grande: pedir y soltar mas que la region entera no la agota");

        /* Y la raya esta donde dice: justo por debajo sigue siendo un tramo de
         * la region.  Sin esto, mover `kMaxSpanChunks` cambiaria de camino a
         * medio proyecto sin que ningun test se enterara. */
        const uint64_t d1 = util::host_direct_allocs();
        void *justo = util::host_alloc(util::kMaxSpanBytes);
        check(justo != nullptr && util::host_direct_allocs() == d1,
              "grande: justo por debajo de la raya lo sirve la REGION");
        util::host_free(justo);
    }

    /* 7. Declarar cuanto se va a tocar CAMBIA el camino, y no cambia el
     *    resultado.
     *
     * Es la unica pieza de informacion que el asignador no puede sacar solo:
     * un bloque grande a cero se sirve de dos formas opuestas -- guardado y
     * limpiado, o pedido fresco al sistema -- y se cruzan en una FRACCION del
     * bloque, no en un tamano.  Lo que se comprueba aqui es lo que tiene que
     * ser cierto pase lo que pase: que el bloque sale a cero por las dos ramas,
     * que se suelta bien por las dos, y que la afirmacion queda CONTADA para
     * poder contrastarla despues. */
    {
        const size_t n = util::kSparseDirectMin * 4;
        const uint64_t sparse_before =
            util::host_fill_allocs(util::AllocFill::Sparse);
        const uint64_t unknown_before =
            util::host_fill_allocs(util::AllocFill::Unknown);
        const uint64_t direct_before = util::host_direct_allocs();

        unsigned char *p = nullptr;
        {
            util::AllocScope f{util::AllocFill::Sparse};
            check(util::AllocScope::current_fill() == util::AllocFill::Sparse,
                  "fill: lo declarado es lo que se lee mientras dura el ambito");
            p = static_cast<unsigned char *>(util::host_alloc_zeroed(n));
        }
        check(util::AllocScope::current_fill() == util::AllocFill::Unknown,
              "fill: y al salir del ambito vuelve a \"no se\"");
        check(util::host_direct_allocs() > direct_before,
              "fill: declarado disperso, lo sirve el SISTEMA");

        bool ceros = p != nullptr;
        for (size_t i = 0; ceros && i < n; i += 4096)
            if (p[i] != 0) ceros = false;
        check(ceros, "fill: y sale a cero igual, que es lo que no puede cambiar");
        check(write_and_verify(p, n, 0x11), "fill: escribible entero");
        util::host_free(p);

        /* La misma peticion sin declarar nada: por el camino de siempre.  Sin
         * esta mitad, la de arriba pasaria igual si el eje no hiciera nada. */
        const uint64_t direct_mid = util::host_direct_allocs();
        void *q = util::host_alloc_zeroed(n);
        check(q != nullptr && util::host_direct_allocs() == direct_mid,
              "fill: sin declarar nada, la sirve la REGION");
        util::host_free(q);

        check(util::host_fill_allocs(util::AllocFill::Sparse) > sparse_before,
              "fill: la afirmacion queda contada, para poder contrastarla");
        check(util::host_fill_allocs(util::AllocFill::Unknown) > unknown_before,
              "fill: y lo que no declara nada tambien, que es lo que dice "
              "cuanto queda");
    }

    /* 8. Paginas EJECUTABLES cerca de un dato nuestro.
     *
     * Es lo que necesita un generador de codigo: alcanza sus datos con
     * desplazamientos de 32 bits, que cubren +-2 GB, asi que donde CAE el
     * codigo decide si funciona.  Y pedirle al sistema un rango pegado al dato
     * no vale cuando el dato esta dentro de una reserva mayor que esa ventana
     * -- medido: el recorrido ve 22 regiones y el hueco mayor es CERO, porque
     * +-2 GB cabe entero dentro de la region grande --.  Por eso se sirve desde
     * DENTRO, y eso es lo que se comprueba aqui.
     *
     * Las dos mitades fallan tarde y lejos si no se miran: unos permisos que no
     * llegaron a la pagina no fallan al reservarla sino al saltar a ella, y una
     * mala colocacion no falla nunca -- solo deja de caber un desplazamiento,
     * mucho despues. */
    {
        /* Un dato de la region GRANDE, que es donde caen los globales de un
         * modulo -- medido con el JIT --.  El tamano se DERIVA de las clases:
         * la region grande sirve de `kBigClassMin` hasta `kMaxSmall`, y por
         * encima de eso ya es un tramo de la pequena.  Escrito a mano, este
         * test dejaria de probar lo que dice en cuanto se moviera una de las
         * dos constantes, y sin avisar. */
        void *const dato = util::host_alloc(util::kMaxSmall);
        check(dato != nullptr && util::in_big_region(dato),
              "codigo: hay un dato en la region grande al que llegar");

        const size_t ventana = (size_t(1) << 31) - (128u << 20);
        bool colocado = false;
        const uint64_t antes = util::host_exec_allocs();
        auto *code = static_cast<unsigned char *>(util::host_alloc_pages(
            1u << 20, util::kOsReadWriteExec, dato, ventana, &colocado));

        check(code != nullptr, "codigo: se sirven paginas ejecutables");
        check(util::host_exec_allocs() > antes,
              "codigo: y salen de NUESTRA reserva, no del sistema");
        check(colocado, "codigo: colocadas dentro de la ventana pedida");

        if (code != nullptr) {
            const intptr_t d = reinterpret_cast<intptr_t>(code) -
                               reinterpret_cast<intptr_t>(dato);
            const size_t dist = size_t(d < 0 ? -d : d);
            std::printf("  el codigo cayo a %.1f MiB del dato (ventana %.0f "
                        "MiB)\n",
                        double(dist) / (1024.0 * 1024.0),
                        double(ventana) / (1024.0 * 1024.0));
            check(dist <= ventana,
                  "codigo: y la distancia lo confirma, no solo la bandera");

            /* EJECUTABLE DE VERDAD: un `ret` y se llama.  Si los permisos no
             * llegaron, esto no devuelve un valor raro: revienta. */
            *code = 0xC3;
            reinterpret_cast<void (*)()>(code)();
            check(true, "codigo: y las paginas se EJECUTAN");
        }
        util::host_free_pages(code, 1u << 20);
        util::host_free(dato);
    }

    std::printf(failures == 0 ? "TODO OK\n" : "%d FALLOS\n", failures);
    return failures == 0 ? 0 : 1;
}
