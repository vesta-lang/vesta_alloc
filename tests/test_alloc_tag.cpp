/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file tests/util/test_alloc_tag.cpp
 * @brief Que la etiqueta de reservas cuente donde tiene que contar.
 *
 * Lo que se comprueba, y por que cada cosa:
 *
 *  1. **Sin ningun ambito, todo cae en "no se".**  Es el default, y tiene que
 *     salir gratis y ser honesto.  Si esto fallara, el reparto entero mentiria
 *     desde el primer dia.
 *  2. **Dentro de un ambito, cuenta ahi.**  Lo minimo.
 *  3. **Los ambitos ANIDAN y restauran.**  Uno que no restaure contamina todo
 *     lo que venga despues en ese hilo, y eso no se nota mirando: el reparto
 *     sale plausible pero equivocado.
 *  4. **Un hilo NUEVO no hereda nada por su cuenta.**  Se comprueba a
 *     proposito, porque es la trampa: una fase etiquetada cuyo trabajo se
 *     reparta contaria como "no se", y eso es un dato FALSO, no uno que falta.
 *  5. **Y pasandola a mano, cuenta donde toca.**  Que es lo que hace
 *     `InPoolTaskScope` en `src/ir/parallel_for.cpp`.
 */

#include "util/alloc/alloc_tag.h"
#include "util/alloc/host_allocator.h"
/* Por `san_guarded_by_tag()`.  Sin el modo compilado la cabecera declara la
 * version que contesta cero, asi que esto vale igual en los dos builds. */
#include "util/alloc/sanitizer.h"

#include <cstdio>
#include <thread>

namespace {

int failures = 0;

void check(bool ok, const char *what) {
    std::printf("  [%s] %s\n", ok ? "OK  " : "FALLO", what);
    if (!ok) ++failures;
}

/// Reservas contadas con @p t desde que se tomo @p base.
/* LA FOTO SON LOS DOS QUE PUEDEN SERVIR, y por eso deja de ser solo las
 * estadisticas del asignador.  Su `by_tag` cuenta los bloques que EL recorto
 * bajo esa etiqueta; en el nivel de guarda del modo comprobacion el bloque sale
 * de paginas propias del comprobador, asi que el asignador no lo vio y hace
 * bien en no contarlo.  Pedirle solo a el la cuenta era preguntar "cuantos
 * recorto" cuando lo que este test pregunta es "cuantos se pidieron bajo este
 * proposito".
 *
 * Los DOS terminos se restan contra la misma foto: sumar el contador del
 * comprobador en absoluto y el del asignador en diferencia daria una cuenta que
 * crece con todo lo que el proceso hizo ANTES, y en un test que exige ">= 20"
 * eso pasa desapercibido -- pasaria siempre.
 *
 * Sin el modo compilado el segundo termino es la version que contesta cero, asi
 * que esto es literalmente lo de siempre y las comprobaciones de abajo no se
 * tocan. */
struct Shot {
    util::HostAllocStats stats;
    uint64_t guarded[VESTA_ALLOC_TAG_SLOTS];
};

Shot take() {
    Shot s;
    s.stats = util::host_alloc_stats();
    for (unsigned i = 0; i < VESTA_ALLOC_TAG_SLOTS; ++i)
        s.guarded[i] = util::san_guarded_by_tag(i);
    return s;
}

uint64_t delta(const Shot &base, util::AllocTag t) {
    const Shot now = take();
    return (now.stats.by_tag[t.raw()] - base.stats.by_tag[t.raw()]) +
           (now.guarded[t.raw()] - base.guarded[t.raw()]);
}

/**
 * @brief Pide y suelta @p n bloques pequenos, sin usar ningun contenedor.
 *
 * A proposito: un `std::vector` reservaria por su cuenta y ensuciaria justo lo
 * que se esta midiendo.
 */
void allocate_some(int n) {
    void *p[64];
    if (n > 64) n = 64;
    for (int i = 0; i < n; ++i)
        p[i] = util::host_alloc(32 + size_t(i % 4) * 16);
    for (int i = 0; i < n; ++i)
        util::host_free(p[i]);
}

const util::AllocTag kInstant{util::AllocUse::Instant, util::AllocShape::Fixed};
const util::AllocTag kLong{util::AllocUse::Long, util::AllocShape::Growing};
const util::AllocTag kUnknown{};

} // namespace

int main() {
    std::printf("== etiqueta de reservas ==\n");
    if (!util::host_alloc_active()) {
        std::printf("  el asignador no esta en vigor: no hay nada que "
                    "comprobar\n");
        return 0;
    }

    // 1. Sin ambito: todo a "no se".
    {
        const Shot base = take();
        allocate_some(20);
        check(delta(base, kUnknown) >= 20,
              "sin ambito, cuenta como \"no se\"");
        check(delta(base, kInstant) == 0,
              "sin ambito, no cuenta en ninguna otra");
    }

    // 2. Dentro de un ambito.
    {
        const Shot base = take();
        {
            const util::AllocScope scope{kInstant};
            allocate_some(20);
        }
        check(delta(base, kInstant) >= 20, "dentro del ambito, cuenta ahi");
        check(delta(base, kUnknown) == 0,
              "dentro del ambito, NO cuenta como \"no se\"");
    }

    // 3. Anidan y restauran.
    {
        const Shot base = take();
        {
            const util::AllocScope outer{kLong};
            allocate_some(10);
            {
                const util::AllocScope inner{kInstant};
                allocate_some(10);
            }
            // Al salir del de dentro tiene que volver el de fuera.
            check(util::AllocScope::current().raw() == kLong.raw(),
                  "al salir del anidado, vuelve el de fuera");
            allocate_some(10);
        }
        check(util::AllocScope::current().unknown(),
              "al salir del ultimo, vuelve a \"no se\"");
        check(delta(base, kLong) >= 20, "el de fuera conto sus dos tandas");
        check(delta(base, kInstant) >= 10, "el de dentro conto la suya");
    }

    // 4. Un hilo nuevo NO hereda.  Es la trampa que documenta el plan.
    {
        const Shot base = take();
        {
            const util::AllocScope scope{kInstant};
            std::thread t([] { allocate_some(20); });
            t.join();
        }
        check(delta(base, kUnknown) >= 20,
              "un hilo nuevo NO hereda la etiqueta (por eso hay que pasarla)");
    }

    // 5. Pasandola a mano: lo que hace InPoolTaskScope.
    {
        const Shot base = take();
        {
            const util::AllocScope scope{kLong};
            // Se lee en el hilo que reparte, NO dentro de la tarea.
            const util::AllocTag parent_tag = util::AllocScope::current();
            std::thread t([parent_tag] {
                const util::AllocScope inherited{parent_tag};
                allocate_some(20);
            });
            t.join();
        }
        check(delta(base, kLong) >= 20,
              "pasandola a mano, el hilo cuenta donde toca");

        /* Aqui habia un `== 0`, y era cierto MIENTRAS `malloc` no fuera
         * nuestro.  Desde que las entradas de C tambien pasan por aqui, crear
         * un hilo deja UNA reserva sin etiquetar: la primera del hilo NUEVO,
         * hecha por el runtime de hilos antes de que corra una linea nuestra.
         * Nadie podia declararla -- no es deuda que migrar, es codigo ajeno --
         * y con `VESTA_HOST_ALLOC_SITES` se ve de quien es, que es la unica
         * pregunta que esa reserva puede contestar.
         *
         * Asi que lo que se vigila ya no es un numero, que dependeria de la
         * implementacion de hilos de cada sistema, sino que ese resto sea
         * CONSTANTE por hilo.  Es una comprobacion mas fuerte que la de antes:
         * si creciera con el trabajo, la etiqueta heredada no estaria
         * funcionando, que es justo lo que este caso existe para vigilar. */
        const uint64_t leak_small = delta(base, kUnknown);

        const Shot base2 = take();
        {
            const util::AllocScope scope{kLong};
            const util::AllocTag parent_tag = util::AllocScope::current();
            std::thread t([parent_tag] {
                const util::AllocScope inherited{parent_tag};
                // Tres tandas y no `allocate_some(192)`: el ayudante TOPA en
                // 64 (guarda los punteros en un array de ese tamano), asi que
                // pedirle mas no reserva mas y la comprobacion de abajo se
                // cumpliria sola sin comprobar nada.
                allocate_some(64);
                allocate_some(64);
                allocate_some(64);
            });
            t.join();
        }
        check(delta(base2, kLong) >= 192,
              "con casi diez veces mas trabajo, cuenta casi diez veces mas");
        check(delta(base2, kUnknown) <= leak_small,
              "y lo que queda sin etiquetar NO crece con el trabajo: es el "
              "arranque del hilo, no lo que la tarea reserva");
    }

    std::printf(failures == 0 ? "TODO OK\n" : "%d FALLOS\n", failures);
    return failures == 0 ? 0 : 1;
}
