/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file src/module_symbols.cpp
 * @brief Asking the LOADER whose address this is, instead of guessing.
 *
 * The two systems answer the same question through completely different doors,
 * and neither door needs anything read off disk -- which matters, because this
 * runs while a report is being written and the process is still the one being
 * described.
 *
 *   - ELF: `dladdr`, which walks the loaded objects and their dynamic symbol
 *     tables.  One call gives module, base and nearest symbol at once.
 *   - PE: `GetModuleHandleEx` by address gives the module; the symbol comes
 *     from its EXPORT table, read straight out of the mapped image.
 *
 * WHY THE EXPORT TABLE ON WINDOWS AND NOT SOMETHING BETTER.  Because it is what
 * is THERE.  Real symbols live in a `.pdb` next to the DLL, and reading those
 * means DbgHelp, a symbol path, and a network call to a symbol server on a bad
 * day -- inside a report, for a name.  The export table is in the image the
 * process already has mapped, it costs a walk, and for the modules that matter
 * here (a system DLL, a plugin) the exported name is the name anybody would
 * recognise.  What it will not name is a static function; that comes back as
 * the module plus an offset, which is true.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "util/symbols/module_symbols.h"

#include "util/os/os_memory.h"
/* La tabla de simbolos del FICHERO del modulo, que tiene mas que lo que el
 * cargador sabe decir: este solo conoce lo EXPORTADO. */
#include "util/symbols/self_symbols.h"

#include <cstring>
/* Las dos ramas lo usan: la de PE para quedarse con la ruta que le escribe
 * `GetModuleFileNameA` en un bufer prestado.  Estuvo dentro del `#else` y en
 * Windows no compilaba -- un `#include` bajo condicion es facil de colocar en
 * la rama equivocada, porque la otra ni se mira. */
#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

extern "C" {

#if defined(_WIN32)

namespace {

/**
 * @brief The nearest exported name at or before @p pc inside @p mod.
 *
 * Walks the export directory of the image as it is mapped, which is why there
 * is no file reading here: the loader already put it in memory, and the
 * addresses in it are relative to the module base.
 *
 * The exports are sorted by NAME, not by address, so finding the nearest one
 * before an address is a linear scan.  That is fine: it happens once per site
 * of a report, not once per allocation.
 */
const char *export_near(HMODULE mod, const void *pc, size_t *sym_offset) {
    auto base = reinterpret_cast<const unsigned char *>(mod);
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

    const IMAGE_DATA_DIRECTORY &dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (dir.VirtualAddress == 0 || dir.Size == 0) return nullptr;

    auto exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY *>(
        base + dir.VirtualAddress);
    auto names = reinterpret_cast<const DWORD *>(base + exp->AddressOfNames);
    auto ordinals =
        reinterpret_cast<const WORD *>(base + exp->AddressOfNameOrdinals);
    auto funcs = reinterpret_cast<const DWORD *>(base + exp->AddressOfFunctions);

    const auto want = reinterpret_cast<const unsigned char *>(pc);
    const char *best = nullptr;
    const unsigned char *best_at = nullptr;

    for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
        const unsigned char *at = base + funcs[ordinals[i]];
        /* At or before, and closer than whatever we had.  An export that comes
         * AFTER the address says nothing about it. */
        if (at <= want && (best_at == nullptr || at > best_at)) {
            best_at = at;
            best = reinterpret_cast<const char *>(base + names[i]);
        }
    }
    if (best == nullptr) return nullptr;
    if (sym_offset != nullptr) *sym_offset = size_t(want - best_at);
    return best;
}

/// The module that owns @p pc, without touching its reference count -- this is
/// a question, not a claim on the module's lifetime.
HMODULE module_at(const void *pc) {
    HMODULE h = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(pc), &h))
        return nullptr;
    return h;
}

/**
 * @brief Remembers the path of a module, because the API writes into a buffer.
 *
 * `dladdr` hands back a pointer the loader owns; `GetModuleFileNameA` fills in
 * a buffer of ours, so something has to keep it alive for the caller.  A small
 * table keyed by module handle, which is stable for as long as the module is
 * loaded -- and a module that allocated memory is loaded for good.
 */
const char *remember_path(HMODULE mod) {
    /* \~english THE PATH IS A BUFFER AND NOT A `std::string`, and that is the
     * whole point of this note.
     *
     * Whoever asks here is, among others, the checker's report -- and that runs
     * from the exit list, AFTER static destruction.  With a `std::string` in
     * this table its destructor has already run by then, so what is handed back
     * points at a block the allocator has already taken: at the ordinary levels
     * it is still mapped and reads fine, which is why it went unnoticed, and at
     * the guard level its pages are gone and the process dies inside the report.
     *
     * Found exactly that way, and it is worth saying that the level did its job
     * -- it caught a use after free that had been there all along, in the code
     * that was supposed to help explain other bugs.
     *
     * An array of characters has no destructor that can have run and no
     * allocation that can have been given back.  It costs `64 * 520` bytes of
     * the binary, in a translation unit that only exists to put names on things.
     *
     * \~spanish LA RUTA ES UN BUFFER Y NO UN `std::string`, y esa es toda la
     * razon de esta nota.
     *
     * Quien pregunta aqui es, entre otros, el informe del comprobador -- y ese
     * corre desde la lista de salida, DESPUES de la destruccion de estaticos.
     * Con un `std::string` en esta tabla, para entonces su destructor ya paso,
     * asi que lo que se devuelve apunta a un bloque que el asignador ya recogio:
     * en los niveles normales sigue mapeado y se lee bien, que es por lo que no
     * se noto, y en el de guarda sus paginas ya no estan y el proceso muere
     * dentro del informe.
     *
     * Encontrado justo asi, y conviene decir que el nivel hizo su trabajo --
     * cazo un uso despues de liberar que llevaba ahi desde siempre, en el codigo
     * que estaba para ayudar a explicar otros fallos.
     *
     * Un array de caracteres no tiene destructor que pueda haber corrido ni
     * reserva que pueda haberse devuelto.  Cuesta `64 * 520` bytes del binario,
     * en una unidad que solo existe para ponerle nombre a las cosas.  \~ */
    struct Entry {
        HMODULE mod;
        char path[MAX_PATH * 2];
    };
    static Entry table[64];
    static unsigned used = 0;
    for (unsigned i = 0; i < used; ++i)
        if (table[i].mod == mod) return table[i].path;

    if (used >= 64) return nullptr; // more modules than a report needs to name
    const DWORD n = GetModuleFileNameA(mod, table[used].path,
                                       sizeof(table[used].path));
    if (n == 0 || n >= sizeof(table[used].path)) return nullptr;
    table[used].mod = mod;
    return table[used++].path;
}

} // namespace

int vesta_module_of(const void *pc, VestaModuleInfo *out) {
    if (pc == nullptr || out == nullptr) return 0;
    HMODULE mod = module_at(pc);
    if (mod == nullptr) return 0;

    out->path = remember_path(mod);
    out->base = reinterpret_cast<const void *>(mod);
    out->offset = size_t(reinterpret_cast<const unsigned char *>(pc) -
                         reinterpret_cast<const unsigned char *>(mod));
    out->sym_offset = 0;
    /* PRIMERO EL FICHERO y luego lo exportado, y ese orden es el que da los
     * nombres buenos: la tabla de simbolos del PE nombra tambien lo que no se
     * exporta, mientras que la tabla de exportacion solo llega a la fachada.
     * Lo exportado queda de respaldo para un modulo despojado, que no tiene
     * tabla pero sigue teniendo exportaciones. */
    out->symbol = util::module_symbol(out->path, out->base, pc,
                                      &out->sym_offset);
    if (out->symbol == nullptr) {
        out->sym_offset = 0;
        out->symbol = export_near(mod, pc, &out->sym_offset);
    }
    return 1;
}

int vesta_module_is_self(const void *pc) {
    if (pc == nullptr) return 0;
    /* The handle of the module that owns the address against the handle of the
     * process image, which is what `GetModuleHandle(NULL)` means.  No string
     * comparison, and no path that could be spelled two ways. */
    return module_at(pc) == GetModuleHandleA(nullptr) ? 1 : 0;
}

#else // ELF

int vesta_module_of(const void *pc, VestaModuleInfo *out) {
    if (pc == nullptr || out == nullptr) return 0;
    Dl_info info;
    std::memset(&info, 0, sizeof(info));
    /* Zero means "not found", which is the opposite of every other function in
     * this library -- and of most of POSIX.  It is easy to get backwards. */
    if (::dladdr(pc, &info) == 0 || info.dli_fbase == nullptr) return 0;

    out->path = info.dli_fname;
    out->base = info.dli_fbase;
    out->offset = size_t(reinterpret_cast<const unsigned char *>(pc) -
                         reinterpret_cast<const unsigned char *>(info.dli_fbase));
    /* EL FICHERO PRIMERO.  `dladdr` mira `.dynsym`, o sea lo EXPORTADO, y una
     * funcion interna no exporta nada: ahi devuelve nulo teniendo el fichero
     * del modulo delante con su `.symtab` entera dentro.  Se lee ese, y lo del
     * cargador queda de respaldo para un modulo despojado. */
    out->symbol = util::module_symbol(out->path, out->base, pc,
                                      &out->sym_offset);
    if (out->symbol == nullptr) {
        out->symbol = info.dli_sname;
        out->sym_offset =
            info.dli_saddr != nullptr
                ? size_t(reinterpret_cast<const unsigned char *>(pc) -
                         reinterpret_cast<const unsigned char *>(info.dli_saddr))
                : 0;
    }
    return 1;
}

int vesta_module_is_self(const void *pc) {
    if (pc == nullptr) return 0;
    Dl_info info;
    std::memset(&info, 0, sizeof(info));
    if (::dladdr(pc, &info) == 0 || info.dli_fbase == nullptr) return 0;
    /* BY LOAD BASE, and the first attempt was by path -- which was wrong in a
     * way worth keeping written down, because it looked right and broke
     * everything.  `dli_fname` for the main program is the path it was STARTED
     * with, so a program run as `./prog` reports `./prog` while
     * `/proc/self/exe` resolves to the absolute path: the comparison never
     * matched, every address looked foreign, and `main` and `parse_tokens`
     * turned into offsets.  A base address has no two spellings. */
    return info.dli_fbase == util::os_module_base() ? 1 : 0;
}

#endif

} // extern "C"
