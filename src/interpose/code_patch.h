/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file src/interpose/code_patch.h
 * @brief
 * \~english Writing a jump over the entry of a function.
 * \~spanish Escribir un salto encima de la entrada de una funcion.
 * \~
 *
 * \~english
 * NOT API: it is not installed and nobody outside includes it.  It exists
 * because there are now TWO places that redirect a function by writing over its
 * first bytes, and they are the same five bytes:
 *
 *   - `call_site.cpp`, over `operator new`, so an allocation can say where it
 *     came from;
 *   - `msvcrt_hook.cpp`, over the C runtime's own `malloc`, so what the runtime
 *     allocates INSIDE itself is ours too.
 *
 * Two copies of code that rewrites executable memory is not a style problem.
 * The second copy is the one that does not get the fix when the first one turns
 * out to be wrong about a reach, a page permission or an encoding -- and being
 * wrong here does not fail, it jumps somewhere.
 *
 * \~spanish
 * NO ES API: no se instala y nadie de fuera lo incluye.  Existe porque ahora
 * hay DOS sitios que redirigen una funcion escribiendo encima de sus primeros
 * bytes, y son los mismos cinco bytes:
 *
 *   - `call_site.cpp`, sobre `operator new`, para que una reserva pueda decir
 *     de donde vino;
 *   - `msvcrt_hook.cpp`, sobre el `malloc` del propio runtime de C, para que lo
 *     que el runtime reserva POR DENTRO sea tambien nuestro.
 *
 * Dos copias de codigo que reescribe memoria ejecutable no es un problema de
 * estilo.  La segunda copia es la que NO recibe el arreglo cuando la primera
 * resulta estar equivocada sobre un alcance, un permiso de pagina o una
 * codificacion -- y equivocarse aqui no falla, salta a alguna parte.
 * \~
 */
#ifndef VESTA_SRC_CODE_PATCH_H
#define VESTA_SRC_CODE_PATCH_H

#include "util/os/os_memory.h"
#include "util/mem/vesta_memcpy.h"

#include <cstddef>
#include <cstdint>

namespace util {
namespace patch_detail {

/// `jmp rel32`: one opcode plus a signed displacement relative to the END of
/// the instruction.  The shortest encoding that still reaches the whole module.
inline constexpr std::size_t kJumpBytes = 5;

/**
 * @brief
 * \~english Writes `jmp target` over the first bytes of @p entry.
 * \~spanish Escribe `jmp target` encima de los primeros bytes de @p entry.
 * \~
 *
 * @return
 * \~english false if the system will not let us write there, or if the target
 *           falls outside the reach of a 32-bit relative jump.  Both cases are
 *           reported: whoever patches has to be able to SAY it could not.
 * \~spanish false si el sistema no deja escribir ahi, o si el destino cae fuera
 *           del alcance de un salto relativo de 32 bits.  Los dos casos se
 *           avisan: quien parchea tiene que poder DECIR que no pudo.
 * \~
 */
inline bool write_jump(void *entry, const void *target) noexcept {
    unsigned char *p = static_cast<unsigned char *>(entry);
    const std::int64_t rel = std::int64_t(std::uintptr_t(target)) -
                             std::int64_t(std::uintptr_t(p) + kJumpBytes);
    if (rel < INT32_MIN || rel > INT32_MAX) return false;

    if (!os_protect(p, kJumpBytes, OsProt::Read | OsProt::Write | OsProt::Exec))
        return false;
    const std::int32_t rel32 = std::int32_t(rel);
    p[0] = 0xE9;
    /* \~english OUR `memcpy`, not the library one.  It is four bytes, but that
     * is not the point: this library ships its own, and reaching outside for it
     * here would add a dependency we do not need, in the only place that writes
     * memory.
     *
     * \~spanish NUESTRO `memcpy`, no el de la libreria.  Son cuatro bytes, pero
     * no va de eso: esta libreria trae el suyo, y salir fuera a por el
     * justamente aqui anadiria una dependencia que no hace falta, en el unico
     * sitio que escribe memoria.  \~ */
    vesta_memcpy(p + 1, &rel32, sizeof(rel32));
    if (!os_protect(p, kJumpBytes, OsProt::Read | OsProt::Exec)) {
        /* \~english Written, but write permission could not be taken back.  The
         * jump is good, so it works; what is left is a page more permissive
         * than it should be.  Still returns true -- pretending it failed would
         * leave the code patched and the caller believing it is not.
         *
         * \~spanish Escrito, pero no se pudo retirar el permiso de escritura.
         * El salto esta bien, asi que funciona; lo que queda es una pagina mas
         * permisiva de lo que deberia.  Aun asi contesta true -- fingir que
         * fallo dejaria el codigo parcheado y a quien llama creyendo que no.
         * \~ */
    }
    return true;
}

/**
 * @brief
 * \~english `jmp [rip+0]` followed by the target, for when a relative jump
 *           cannot reach.
 * \~spanish `jmp [rip+0]` seguido del destino, para cuando un salto relativo no
 *           llega.
 * \~
 *
 * \~english
 * WHY THIS EXISTS.  The relative form above is five bytes and reaches two
 * gibibytes, which covers anything inside the same module -- and that is all it
 * ever had to cover, while the only thing being patched was our own
 * `operator new`.  Patching the C runtime is a different distance: msvcrt.dll
 * is loaded wherever the system puts it, and with address randomisation that is
 * routinely further than a signed 32-bit displacement can express.  It does not
 * misjump when that happens -- `write_jump` refuses -- but refusing was all it
 * could do, and the hook simply did not go in.
 *
 * WHY THIS ENCODING AND NOT `mov rax, imm64; jmp rax`, which is two bytes
 * shorter: that one CLOBBERS a register.  At a function entry rax happens to be
 * free under both calling conventions, so it would work -- and it would work by
 * relying on a property of the ABI, at the entry of a function belonging to
 * somebody else, patched at run time.  This one touches no register at all: the
 * processor loads the destination straight from the eight bytes that follow.
 *
 * \~spanish
 * POR QUE EXISTE.  La forma relativa de arriba son cinco bytes y alcanza dos
 * gibibytes, que cubre cualquier cosa dentro del mismo modulo -- y eso era todo
 * lo que tenia que cubrir mientras lo unico parcheado era nuestro propio
 * `operator new`.  Parchear el runtime de C es otra distancia: msvcrt.dll se
 * carga donde el sistema quiera, y con la aleatorizacion de direcciones eso
 * queda habitualmente mas lejos de lo que un desplazamiento de 32 bits con
 * signo puede expresar.  No salta mal cuando pasa -- `write_jump` se niega --
 * pero negarse era todo lo que podia hacer, y el gancho sencillamente no
 * entraba.
 *
 * POR QUE ESTA CODIFICACION Y NO `mov rax, imm64; jmp rax`, que son dos bytes
 * menos: esa PISA un registro.  En la entrada de una funcion rax resulta estar
 * libre en las dos convenciones de llamada, asi que funcionaria -- y
 * funcionaria apoyandose en una propiedad del ABI, en la entrada de una funcion
 * ajena, parcheada en ejecucion.  Esta no toca ningun registro: el procesador
 * carga el destino directamente de los ocho bytes que siguen.
 *
 * The price is FOURTEEN bytes written instead of five, so more of the original
 * prologue is overwritten.  That is the real cost of this call and it is why it
 * is a separate function: nobody should reach for it without meaning to.
 *
 * El precio son CATORCE bytes escritos en vez de cinco, asi que se pisa mas
 * prologo original.  Ese es el coste de verdad de esta llamada y es por lo que
 * es una funcion aparte: nadie deberia echar mano de ella sin querer hacerlo.
 * \~
 */
inline constexpr std::size_t kFarJumpBytes = 14;

inline bool write_jump_far(void *entry, const void *target) noexcept {
    unsigned char *p = static_cast<unsigned char *>(entry);
    if (!os_protect(p, kFarJumpBytes,
                    OsProt::Read | OsProt::Write | OsProt::Exec))
        return false;
    /* \~english ff 25 00000000 -- jump to the address stored at rip+0, which is
     * the eight bytes right after this instruction.
     *
     * \~spanish ff 25 00000000 -- salta a la direccion guardada en rip+0, que
     * son los ocho bytes de justo despues de esta instruccion.  \~ */
    p[0] = 0xFF;
    p[1] = 0x25;
    const std::int32_t zero = 0;
    vesta_memcpy(p + 2, &zero, sizeof(zero));
    const std::uintptr_t dst = std::uintptr_t(target);
    vesta_memcpy(p + 6, &dst, sizeof(dst));
    if (!os_protect(p, kFarJumpBytes, OsProt::Read | OsProt::Exec)) {
        // Written; only the permission could not be taken back.  See above.
    }
    return true;
}

} // namespace patch_detail
} // namespace util

#endif // VESTA_SRC_CODE_PATCH_H
