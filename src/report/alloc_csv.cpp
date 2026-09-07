/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file util/alloc_csv.cpp
 * @brief The allocation report as data.  Reasons: the header.
 *
 * THE ONE RULE HERE: what goes out is what was MEASURED, with nothing rounded,
 * merged or left out.  A tool can fold and sort as it likes; it cannot invent
 * what was never written.  So the counts go raw -- including the part that is
 * an upper bound, in its own column, instead of quietly folded into the total.
 */

#include "util/report/alloc_csv.h"

#include "util/report/alloc_sites.h"
#include "util/alloc/alloc_tag.h"
#include "util/alloc/host_allocator.h"
#include "util/symbols/module_symbols.h" // si el sitio es de este modulo o de otro
#include "util/os/os_memory.h"
#include "util/alloc/size_buckets.h"
#include "util/mem/vesta_memcpy.h"
#include "util/mem/vesta_memset.h"

#include <cstdio>
#include <cstring>

/* Solo para crear la carpeta de salida.  Ver `ensure_dir`. */
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace util {

namespace {

/// Installed by the host.  Plain pointers: they are set once, at start-up.
AllocSymbolResolver g_resolver = nullptr;
AllocNameFormatter g_formatter = nullptr;

/**
 * @brief Writes a field, quoting it when it needs it.
 *
 * A C++ function name is FULL of commas and quotes -- one template argument
 * list has half a dozen -- so writing it raw would split it across columns and
 * the tool would read the tail of a type as if it were the next field.  The
 * rule is the usual one: wrap in quotes and double the inner ones.
 */
void csv_field(FILE *f, const char *s) {
    if (s == nullptr) return; // empty field: unknown, and it stays visible
    if (std::strpbrk(s, ",\"\n\r") == nullptr) {
        std::fputs(s, f);
        return;
    }
    std::fputc('"', f);
    for (const char *p = s; *p != '\0'; ++p) {
        if (*p == '"') std::fputc('"', f);
        std::fputc(*p, f);
    }
    std::fputc('"', f);
}

/**
 * @brief Makes @p path exist, creating every component it is missing.
 *
 * WHY THE LIBRARY DOES THIS instead of asking for a directory that is already
 * there.  Because the other way round has one failure mode and it is a bad
 * one: the run finishes, the report is gone, and the only trace is a line
 * saying a folder was missing.  Whoever asked for a report wants the report,
 * not homework -- and the directory is not a decision, it is plumbing.
 *
 * Component by component, because the useful thing to pass is a path with
 * several levels (`build/profile/alloc`), and creating only the last one would
 * fail for exactly the same reason.  A component that already exists is not an
 * error: two exports into the same place is the normal case.
 */
bool ensure_dir(const char *path) {
    if (path == nullptr || path[0] == '\0') return true; // el directorio actual
    char buf[1024];
    const size_t n = std::strlen(path);
    if (n >= sizeof(buf)) return false;
    vesta_memcpy(buf, path, n + 1);

    for (size_t i = 0; i <= n; ++i) {
        const bool end = i == n;
        if (!end && buf[i] != '/' && buf[i] != '\\') continue;
        /* Ni la raiz ni la unidad se crean: `C:` y `/` ya estan, y pedirlo
         * fallaria por algo que no es un fallo. */
        if (i == 0 || (i == 2 && buf[1] == ':')) continue;
        const char saved = buf[i];
        buf[i] = '\0';
#if defined(_WIN32)
        const bool ok = CreateDirectoryA(buf, nullptr) != 0 ||
                        GetLastError() == ERROR_ALREADY_EXISTS;
#else
        const bool ok = ::mkdir(buf, 0777) == 0 || errno == EEXIST;
#endif
        buf[i] = saved;
        if (!ok) return false;
    }
    return true;
}

/// Opens `<dir>/<name>`.  Null if it cannot, having said so.
FILE *open_in(const char *dir, const char *name) {
    char path[1024];
    const int n = std::snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (n <= 0 || size_t(n) >= sizeof(path)) {
        std::fprintf(stderr, "[allocator] CSV: path too long for %s\n", name);
        return nullptr;
    }
    FILE *f = std::fopen(path, "wb");
    if (f == nullptr)
        std::fprintf(stderr,
                     "[allocator] CSV: could not write %s -- does the "
                     "directory exist?\n",
                     path);
    return f;
}

/// How many frames are asked for per site.  Deep chains are real -- eighteen
/// were measured in one -- so this is generous; what does not fit is said in
/// `summary.csv` rather than silently cut.
constexpr unsigned kMaxFrames = 64;

/// Sites taken in one snapshot.  Same reasoning as the text report: with room
/// for only a few the list stops being "the sites" and becomes "some of them".
constexpr unsigned kMaxSites = 4096;

} // namespace

AllocSymbolResolver alloc_symbol_resolver() noexcept { return g_resolver; }

void alloc_set_symbol_resolver(AllocSymbolResolver fn) noexcept {
    g_resolver = fn;
}

void alloc_set_name_formatter(AllocNameFormatter fn) noexcept {
    g_formatter = fn;
}

const char *alloc_readable_name(const char *raw) noexcept {
    if (raw == nullptr) return nullptr;
    if (g_formatter == nullptr) return raw;
    const char *pretty = g_formatter(raw);
    /* Un formateador que no sabe que hacer devuelve nulo, y entonces se queda
     * el crudo.  Que no pueda EMPEORAR la respuesta es parte del contrato:
     * quien lo instala no tiene que acordarse de devolver el original. */
    return pretty != nullptr ? pretty : raw;
}

bool write_alloc_csv(const char *dir) {
    if (dir == nullptr || dir[0] == '\0') return false;
    if (!ensure_dir(dir)) {
        std::fprintf(stderr,
                     "[allocator] CSV: could not create '%s'.  The report was "
                     "asked for and cannot be given, so this says it instead "
                     "of finishing quietly.\n",
                     dir);
        return false;
    }

    static AllocSite sites[kMaxSites];
    const unsigned n = alloc_sites_snapshot(sites, kMaxSites);
    const HostAllocStats st = host_alloc_stats();
    const unsigned long long base =
        (unsigned long long)(uintptr_t)os_module_base();

    bool ok = true;
    unsigned long long deepest = 0;
    unsigned long long unresolved = 0;

    // -- sites.csv ----------------------------------------------------------
    //
    // `allocs` is what the site EARNED and `over` what it inherited when it
    // evicted a weaker entry: the truth is in [allocs, allocs+over].  The two
    // travel apart on purpose -- folding them would hand the tool a number that
    // looks exact and is not.
    if (FILE *f = open_in(dir, "sites.csv")) {
        std::fprintf(f,
                     "site_id,allocs,bytes,over_allocs,over_bytes,large,"
                     "size_classes,class_mask,tag,tag_name,pc_offset,"
                     "foreign\n");
        for (unsigned i = 0; i < n; ++i) {
            const AllocTag tag = AllocTag::from_raw(sites[i].tag);
            unsigned classes = 0;
            for (unsigned b = 0; b < 64; ++b)
                if ((sites[i].class_mask >> b) & 1u) ++classes;
            /* La MASCARA sale ademas de la cuenta, y no es redundante: la
             * cuenta de un sitio no se puede sumar con la de otro -- dos
             * sitios que usan la misma clase darian dos --, asi que una
             * herramienta que agrupe sitios no puede decir cuantas clases
             * distintas hay en un grupo sin los bits.  Con ellos es un OR y un
             * recuento; sin ellos hay que inventarse un numero o no decir
             * nada.  Es la regla de esta cabecera: lo que sale es lo MEDIDO,
             * no un resumen del que no se puede volver. */
            std::fprintf(f, "%u,%llu,%llu,%llu,%llu,%llu,%u,%llu,%u,", i,
                         (unsigned long long)(sites[i].count - sites[i].over),
                         (unsigned long long)(sites[i].bytes -
                                              sites[i].over_bytes),
                         (unsigned long long)sites[i].over,
                         (unsigned long long)sites[i].over_bytes,
                         (unsigned long long)sites[i].large, classes,
                         (unsigned long long)sites[i].class_mask,
                         unsigned(sites[i].tag));
            csv_field(f, alloc_tag_name(tag));
            /* The address goes as an OFFSET from the module base, never
             * absolute: with address space layout randomisation the absolute
             * one means nothing in another run, and the tool would resolve it
             * against the wrong place without any way to notice. */
            std::fprintf(f, ",%llu",
                         (unsigned long long)((uintptr_t)sites[i].pc) - base);
            /* WHETHER THE SITE IS OURS AT ALL, and it travels as a FACT rather
             * than being worked out again downstream.  Since the allocator
             * serves the C runtime from the inside, a site can belong to
             * `msvcrt.dll` or to `libc.so`, and a tool asked to show "only my
             * own calls" has to be able to tell.  It cannot: the `module`
             * column holds whatever the resolver put there -- often a LOGICAL
             * module of the project -- so from the outside a system library and
             * a part of the program look the same.
             *
             * The one who knows is here, and knows for certain, so it says so.
             * The offset above is the other half: for a foreign site it is a
             * displacement from OUR base, which means nothing -- this column is
             * what stops anyone reading it as if it did. */
            std::fprintf(f, ",%d\n",
                         vesta_module_is_self(sites[i].pc) != 0 ? 0 : 1);
        }
        if (std::fclose(f) != 0) ok = false;
    } else {
        ok = false;
    }

    // -- frames.csv ---------------------------------------------------------
    //
    // Depth 0 is the innermost frame -- where the code physically is -- and the
    // last one is the function that exists in the binary.  With no resolver
    // installed the file is written with just its header: an empty table says
    // "nothing is known", a missing file says "something broke".
    if (FILE *f = open_in(dir, "frames.csv")) {
        std::fprintf(f, "site_id,depth,inlined,function,file,line,module\n");
        if (g_resolver != nullptr) {
            AllocFrame frames[kMaxFrames];
            for (unsigned i = 0; i < n; ++i) {
                /* Se limpia ANTES de cada llamada, y no una sola vez al
                 * declararlo.  Un resolutor solo rellena lo que sabe -- el
                 * contrato lo dice --, asi que un campo que no toque quedaria
                 * con lo que dejo el sitio ANTERIOR: un modulo equivocado que
                 * parece bueno, que es peor que uno vacio. */
                /* The TYPED C++ form, not the byte one: it gets `sizeof` and
                 * `alignof` from the type instead of being told a byte count
                 * with the alignment thrown away, which is all the plain-C
                 * entry point can do. */
                vesta_memfill(frames, 0, kMaxFrames);
                const unsigned got = g_resolver(sites[i].pc, frames, kMaxFrames);
                if (got == 0) ++unresolved;
                if (got > deepest) deepest = got;
                for (unsigned k = 0; k < got; ++k) {
                    std::fprintf(f, "%u,%u,%d,", i, k, frames[k].inlined ? 1 : 0);
                    /* Por el formateador: lo que sale es como lo escribe quien
                     * nos enlaza, no un `_ZNSt7__cxx11` que obligaria a la
                     * herramienta a traer su propio desmanglador. */
                    csv_field(f, alloc_readable_name(frames[k].function));
                    std::fputc(',', f);
                    csv_field(f, frames[k].file);
                    std::fprintf(f, ",%u,", frames[k].line);
                    csv_field(f, frames[k].module);
                    std::fputc('\n', f);
                }
            }
        } else {
            unresolved = n;
        }
        if (std::fclose(f) != 0) ok = false;
    } else {
        ok = false;
    }

    // -- sizes.csv y tags.csv ----------------------------------------------
    if (FILE *f = open_in(dir, "sizes.csv")) {
        std::fprintf(f, "bucket,upper_bytes,allocs\n");
        for (unsigned b = 0; b < kSizeBuckets; ++b)
            std::fprintf(f, "%u,%llu,%llu\n", b, bucket_limit_out(b),
                         (unsigned long long)st.size_hist[b]);
        if (std::fclose(f) != 0) ok = false;
    } else {
        ok = false;
    }

    // -- site_sizes.csv -----------------------------------------------------
    //
    // The same split as `sizes.csv`, but PER SITE, which is the one the other
    // one cannot give: a global histogram cannot be handed back out to the
    // sites that formed it, so without this a branch of the tree can say which
    // sizes it touches (the class mask) but not how many of each -- and a site
    // with a million 32-byte allocations and one of 16 MiB looks exactly like
    // one that does the opposite.
    //
    // SPARSE, one row per (site, bucket) that is not zero.  Twelve rows per
    // site would be mostly zeros: most sites ask for one or two sizes.
    if (FILE *f = open_in(dir, "site_sizes.csv")) {
        std::fprintf(f, "site_id,bucket,upper_bytes,allocs\n");
        for (unsigned i = 0; i < n; ++i)
            for (unsigned b = 0; b < kSizeBuckets; ++b)
                if (sites[i].size_hist[b] != 0)
                    std::fprintf(f, "%u,%u,%llu,%u\n", i, b,
                                 bucket_limit_out(b), sites[i].size_hist[b]);
        if (std::fclose(f) != 0) ok = false;
    } else {
        ok = false;
    }

    if (FILE *f = open_in(dir, "tags.csv")) {
        std::fprintf(f, "tag,tag_name,allocs\n");
        for (uint32_t g = 0; g < AllocTag::kSlots; ++g) {
            std::fprintf(f, "%u,", g);
            csv_field(f, alloc_tag_name(AllocTag::from_raw((uint8_t)g)));
            std::fprintf(f, ",%llu\n", (unsigned long long)st.by_tag[g]);
        }
        if (std::fclose(f) != 0) ok = false;
    } else {
        ok = false;
    }

    // -- summary.csv --------------------------------------------------------
    //
    // Key/value, so a column added later does not break a tool that reads it.
    // And it carries what could NOT be done, which is the part a tool cannot
    // work out on its own: without it, a tree with no names looks like a
    // program that allocates from nowhere.
    if (FILE *f = open_in(dir, "summary.csv")) {
        std::fprintf(f, "key,value\n");
        std::fprintf(f, "sites,%u\n", n);
        std::fprintf(f, "sites_capacity,%u\n", kMaxSites);
        std::fprintf(f, "small_allocs,%llu\n",
                     (unsigned long long)st.small_allocs);
        std::fprintf(f, "small_frees,%llu\n",
                     (unsigned long long)st.small_frees);
        std::fprintf(f, "remote_frees,%llu\n",
                     (unsigned long long)st.remote_frees);
        std::fprintf(f, "large_allocs,%llu\n",
                     (unsigned long long)st.large_allocs);
        std::fprintf(f, "large_frees,%llu\n",
                     (unsigned long long)st.large_frees);
        std::fprintf(f, "chunks,%llu\n", (unsigned long long)st.chunks);
        std::fprintf(f, "bytes_reserved,%llu\n",
                     (unsigned long long)st.bytes_reserved);
        std::fprintf(f, "untagged,%llu\n",
                     (unsigned long long)st.by_tag[AllocTag{}.raw()]);
        std::fprintf(f, "evicted,%llu\n",
                     (unsigned long long)alloc_sites_overflow());
        std::fprintf(f, "module_base,%llu\n", base);
        std::fprintf(f, "has_symbols,%d\n", g_resolver != nullptr ? 1 : 0);
        std::fprintf(f, "sites_without_frames,%llu\n", unresolved);
        std::fprintf(f, "deepest_chain,%llu\n", deepest);
        std::fprintf(f, "frames_capacity,%u\n", kMaxFrames);
        if (std::fclose(f) != 0) ok = false;
    } else {
        ok = false;
    }

    if (!ok)
        std::fprintf(stderr,
                     "[allocator] CSV: the export is INCOMPLETE, so do not "
                     "read it: a report with holes looks like data.\n");
    return ok;
}

} // namespace util
