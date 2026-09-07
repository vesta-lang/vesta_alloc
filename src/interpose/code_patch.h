/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file src/code_patch.h
 * @brief Writing a jump over the entry of a function.
 *
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
 * @brief Writes `jmp target` over the first bytes of @p entry.
 *
 * @return false if the system will not let us write there, or if the target
 *         falls outside the reach of a 32-bit relative jump.  Both cases are
 *         reported: whoever patches has to be able to SAY it could not.
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
    /* OUR `memcpy`, not the library one.  It is four bytes, but that is not the
     * point: this library ships its own, and reaching outside for it here would
     * add a dependency we do not need, in the only place that writes memory. */
    vesta_memcpy(p + 1, &rel32, sizeof(rel32));
    if (!os_protect(p, kJumpBytes, OsProt::Read | OsProt::Exec)) {
        /* Written, but write permission could not be taken back.  The jump is
         * good, so it works; what is left is a page more permissive than it
         * should be.  Still returns true -- pretending it failed would leave
         * the code patched and the caller believing it is not. */
    }
    return true;
}

/**
 * @brief `jmp [rip+0]` followed by the target, for when a relative jump cannot
 *        reach.
 *
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
 * The price is FOURTEEN bytes written instead of five, so more of the original
 * prologue is overwritten.  That is the real cost of this call and it is why it
 * is a separate function: nobody should reach for it without meaning to.
 */
inline constexpr std::size_t kFarJumpBytes = 14;

inline bool write_jump_far(void *entry, const void *target) noexcept {
    unsigned char *p = static_cast<unsigned char *>(entry);
    if (!os_protect(p, kFarJumpBytes,
                    OsProt::Read | OsProt::Write | OsProt::Exec))
        return false;
    /* ff 25 00000000 -- jump to the address stored at rip+0, which is the
     * eight bytes right after this instruction. */
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
