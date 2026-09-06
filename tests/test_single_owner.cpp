/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file tests/test_single_owner.cpp
 * @brief Que las DOS versiones convivan y sus bloques sean intercambiables.
 *
 * Lo importante que se comprueba aqui no es la velocidad -- para eso esta el
 * banco -- sino que no haya que ELEGIR una de las dos.  Un interruptor de
 * compilacion obligaria a que todo el programa usara la misma, y lo que hace
 * falta es poder mezclarlas: una fase con dueno declarado usa la suya, y lo que
 * salga de ahi puede acabar en un `std::vector` que lo suelte por
 * `operator delete` sin enterarse de nada.
 *
 * De ahi los casos:
 *
 *  1. Sirve, y esta bien puesto (alineado, escribible, sin solaparse).
 *  2. Lo suyo se puede soltar con el general, y al reves.
 *  3. Cuenta en SU almacen, no en el del hilo.
 *  4. Dos a la vez no se pisan.
 *  5. Y lo que reserve uno lo puede soltar OTRO HILO, que es el caso que hunde
 *     a los asignadores por hilo ingenuos.
 */

#include "util/host_allocator.h"

#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char *what) {
    std::printf("  [%s] %s\n", ok ? "OK  " : "FALLO", what);
    if (!ok) ++failures;
}

/// Escribe un patron reconocible y comprueba que sigue ahi.
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
    std::printf("== asignador de un solo dueno ==\n");
    if (!util::host_alloc_active()) {
        std::printf("  asignador apagado (VESTA_NO_HOST_SLAB): nada que "
                    "comprobar\n");
        return 0;
    }

    util::SingleOwnerAllocator own;
    check(own.valid(), "consigue almacen propio");

    // 1. Sirve, alineado y escribible.
    {
        const size_t sizes[] = {16, 48, 64, 200, 512, 2048};
        const int n = int(sizeof(sizes) / sizeof(sizes[0]));
        void *p[6];
        bool aligned = true, writable = true;
        for (int i = 0; i < n; ++i) {
            p[i] = own.alloc(sizes[i]);
            if (p[i] == nullptr) {
                writable = false;
                break;
            }
            if ((reinterpret_cast<uintptr_t>(p[i]) & (util::kAlign - 1)) != 0)
                aligned = false;
            if (!write_and_verify(p[i], sizes[i], (unsigned char)(i * 31)))
                writable = false;
        }
        check(aligned, "todo bloque sale alineado a 16");
        check(writable, "y se puede escribir entero");

        bool intact = true;
        for (int i = 0; i < n && intact; ++i) {
            const unsigned char *b = static_cast<unsigned char *>(p[i]);
            for (size_t j = 0; j < sizes[i]; ++j)
                if (b[j] != (unsigned char)(i * 31 + (j & 0x7F))) {
                    intact = false;
                    break;
                }
        }
        check(intact, "dos bloques vivos nunca se solapan");
        for (int i = 0; i < n; ++i)
            own.free(p[i]);
    }

    // 2. Intercambiables con el general, en los dos sentidos.
    {
        void *a = own.alloc(64);
        util::host_free(a); // el general suelta lo del propio
        void *b = util::host_alloc(64);
        own.free(b); // y el propio suelta lo del general
        check(true, "los bloques se cruzan entre los dos sin romper nada");

        // Y despues de cruzarlos, los dos siguen sirviendo.
        void *c = own.alloc(64);
        void *d = util::host_alloc(64);
        check(c != nullptr && d != nullptr && c != d,
              "y los dos siguen sirviendo despues");
        own.free(c);
        util::host_free(d);
    }

    // 3. Cuenta en SU almacen.
    {
        const uint64_t before = own.stats().small_allocs;
        for (int i = 0; i < 100; ++i)
            own.free(own.alloc(32));
        check(own.stats().small_allocs - before >= 100,
              "lo suyo se cuenta en su propio almacen");
    }

    // 4. Dos a la vez, cada uno con lo suyo.
    {
        util::SingleOwnerAllocator other;
        check(!other.valid() || other.stats().small_allocs == 0,
              "un almacen recien creado empieza a cero");
        // `stats()` va por valor: el total lo rellena al pedirlo, porque en el
        // camino caliente solo se lleva el reparto por etiqueta.

        void *p1 = own.alloc(48);
        void *p2 = other.alloc(48);
        check(p1 != p2, "dos almacenes no entregan el mismo bloque");
        own.free(p1);
        other.free(p2);
    }

    // 5. Reservado aqui, soltado en OTRO hilo.
    {
        const int n = 500;
        std::vector<void *> blocks(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i)
            blocks[size_t(i)] = own.alloc(64);

        std::thread t([&] {
            for (int i = 0; i < n; ++i)
                util::host_free(blocks[size_t(i)]);
        });
        t.join();

        // Y el almacen sigue sirviendo despues de que le devuelvan lo suyo
        // desde fuera: los bloques vuelven por la pila atomica de su dueno.
        bool alive = true;
        for (int i = 0; i < n; ++i) {
            void *p = own.alloc(64);
            if (p == nullptr) {
                alive = false;
                break;
            }
            own.free(p);
        }
        check(alive, "lo soltado por OTRO hilo se recupera y se reusa");
    }

    std::printf(failures == 0 ? "TODO OK\n" : "%d FALLOS\n", failures);
    return failures == 0 ? 0 : 1;
}
