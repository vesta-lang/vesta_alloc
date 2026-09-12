/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file tests/util/test_os_memory.cpp
 * @brief That asking the system what a RANGE costs answers about that range.
 *
 * WHAT MUST NOT BREAK.  `util::os_range_memory` is what turns "the process
 * holds 1,7 GiB" into "the allocator's own ranges hold 999 MiB of it, and 725
 * of those were never handed out".  That whole sentence rests on the figure
 * being about the RANGE and not about the process: a function that quietly
 * answered with the process total would still look plausible -- it grows, it is
 * in the right order of magnitude, and nothing on screen would say it is the
 * wrong question.
 *
 * So what is checked is the part that separates the two.  A range is reserved
 * and nothing else done to it, and the answer must be near zero; then half of
 * it is committed and touched, and the answer must follow THAT half and not the
 * whole; then it is given back, and the answer must come down.  A process-wide
 * figure fails the first check and the third.
 *
 * THE TWO SYSTEMS ANSWER DIFFERENT QUESTIONS, and the test knows it: Windows
 * counts pages with backing store reserved, Linux counts pages actually
 * resident, because there is no such thing as committing on Linux.  That is why
 * the committed half is WRITTEN to and not merely committed -- on Linux
 * untouched pages are not resident and the honest answer there is zero.  The
 * bounds are deliberately loose: what is being tested is that the function
 * tracks the range, not that it agrees with a page count to the byte.
 */
#include "util/os/os_memory.h"

#include <cstdio>
#include <cstring>

namespace {

int g_failures = 0;

void check(bool ok, const char *what) {
    std::printf("  [%s] %s\n", ok ? "OK  " : "FAIL", what);
    if (!ok) ++g_failures;
}

/// Prints a figure with its name, so a failure says WHAT came back.
void say(const char *what, size_t bytes) {
    std::printf("        %-34s %10llu bytes\n", what,
                (unsigned long long)bytes);
}

} // namespace

int main() {
    using namespace util;

    std::printf("[test_os_memory]\n");

    check(os_range_memory(nullptr, 1 << 20) == 0,
          "a null range costs nothing, and does not crash");

    /* Un rango GRANDE a proposito: con uno de dos paginas, cualquier respuesta
     * cae dentro de cualquier margen y el test pasaria contestando otra cosa.
     * Ocho MiB son suficientes para que la mitad comprometida se distinga de la
     * reserva entera sin pedirle nada raro al sistema. */
    const size_t kBytes = size_t(8) << 20;
    void *base = os_reserve(kBytes);
    check(base != nullptr, "there is a range to ask about");
    if (base == nullptr) return 1;

    const size_t reserved_only = os_range_memory(base, kBytes);
    say("just reserved", reserved_only);
    /* Ni una pagina: una reserva no es memoria.  El margen es una pagina por si
     * el sistema apunta algo suyo dentro, no un colchon para medio rango. */
    check(reserved_only <= os_page_size(),
          "a range only reserved costs nothing -- a reservation is not memory");

    const size_t half = kBytes / 2;
    check(os_commit(base, half), "half of it is committed");
    /* Y se ESCRIBE.  En Linux comprometer no existe: una pagina no respaldada
     * no es residente, y la respuesta honesta ahi seria cero.  Tocarla es lo
     * que hace que la pregunta signifique lo mismo en los dos sistemas. */
    std::memset(base, 0x5a, half);

    const size_t with_half = os_range_memory(base, kBytes);
    say("half committed and written", with_half);
    check(with_half >= half - os_page_size(),
          "and then the range costs at least that half");
    /* Y NO MAS que esa mitad: es la comprobacion que separa esta funcion de
     * `os_process_memory`.  Un total del proceso -- decenas de MiB solo de
     * imagen y pilas -- se sale por arriba y este test es el unico sitio donde
     * eso se ve. */
    check(with_half <= half + (size_t(1) << 20),
          "and not more -- this is the range, not the process");

    check(os_decommit(base, half), "the half is given back");
    const size_t after = os_range_memory(base, kBytes);
    say("after giving it back", after);
    check(after < half / 2,
          "and then it stops costing -- what was released is not counted");

    os_release(base, kBytes);

    /* UN RANGO QUE YA NO ESTA no es un error: es cero.  Quien llama a esto es
     * el comprobador en un corte, y puede preguntar por un rango que el
     * asignador acaba de soltar. */
    check(os_range_memory(base, kBytes) == 0,
          "a range no longer mapped costs nothing, and does not crash");

    std::printf("[test_os_memory] %s\n",
                g_failures == 0 ? "TODO OK" : "CON FALLOS");
    return g_failures == 0 ? 0 : 1;
}
