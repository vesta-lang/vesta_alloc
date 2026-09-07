/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file util/call_site.h
 * @brief WHERE each allocation comes from, at no cost to the normal path.
 *
 * THE PROBLEM.  To record who allocates you need the return address of
 * `operator new`.  It used to come from `__builtin_return_address(0)`, which IS
 * reliable for zero -- but it is a value the compiler hands you, and the moment
 * you want to walk one frame further up GCC itself says of
 * `__builtin_return_address(N>0)` that it "may have unpredictable effects,
 * including crashing the calling program".
 *
 * HOW WE STOP DEPENDING ON ANYONE.  The calling convention says that ON ENTRY
 * to a function the return address sits at `[rsp]`.  One `mov`.  No stack
 * searching, no heuristics, and nothing the optimiser can change.  All it takes
 * is for the prologue to be OURS, which is what an assembly thunk gives us.
 *
 * AND WHY A PATCH.  Because a permanent thunk would be paid by EVERY
 * allocation, including those of people who are not measuring: one `mov` and
 * one `jmp` per `new`.  In this allocator that is not negligible -- moving the
 * measurement into the cold path was already worth a measured 3.3% --.  So
 * `operator new` stays exactly as it was, and when measurement is requested a
 * jump is written over its entry.  With it off there is nothing to pay because
 * there is nothing there.
 *
 * WHAT IT DOES NOT SOLVE.  The address is that of the IMMEDIATE caller.  For a
 * `std::string` that caller is the standard library, not the code that wanted
 * the string.  This makes the value OURS and exact; reaching further up is a
 * separate conversation.
 */
#ifndef VESTA_UTIL_CALL_SITE_H
#define VESTA_UTIL_CALL_SITE_H

namespace util {

/**
 * @brief Makes the `operator new` family record where they were called from.
 *
 * Writes a jump to the thunk over each one's entry point.  Until this is
 * called, `operator new` is the usual one and costs NOT ONE extra instruction:
 * that is the whole reason this is a patch and not a permanent thunk.
 *
 * Covers all EIGHT: `new`, `new[]`, their two `nothrow` forms, and the four
 * over-aligned ones.  Leaving the over-aligned ones out was the kind of hole
 * you cannot see -- anything aligned to a cache line did not show up in the
 * report as "undeclared", it did not show up at all -- and a list of who
 * allocates that is missing allocations is not incomplete, it is wrong.
 *
 * WHEN TO CALL IT: once, when measurement is switched on, BEFORE the first
 * allocation and with a single thread running.  It is not meant to be switched
 * on hot -- it writes over code other threads could be executing -- which is
 * why a second call does nothing.
 *
 * @return false if some operator could not be patched: the system refused to
 *         make that page writable, or the thunk fell outside the reach of a
 *         relative jump.  THAT operator then keeps recording nothing, and
 *         whoever dumps the report has to say so rather than show a list that
 *         is quietly missing allocations.
 */
bool install_call_site_patch() noexcept;

/**
 * @brief Was the patch installed?
 *
 * So the report can say whether the list is complete.  Without this, a patch
 * that failed would produce a shorter list that looks perfectly correct, which
 * is the worst way to be wrong.
 */
bool call_site_patch_installed() noexcept;

} // namespace util

#endif // VESTA_UTIL_CALL_SITE_H
