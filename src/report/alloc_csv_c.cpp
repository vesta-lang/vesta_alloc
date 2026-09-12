/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file util/alloc_csv_c.cpp
 * @brief The C layer of the report.  The reasons are in the header.
 *
 * A THIN layer on purpose: every function leans on the C++ one and repeats not
 * one decision.  Two implementations of the same export would drift, and the
 * one that fell behind would be the one nobody looks at.
 *
 * There is no conversion anywhere in here, and that is the point of declaring
 * the frame struct and the two hook types in the C header: both languages name
 * the SAME types, so a resolver written in C is installed as it is, called as
 * it is, and writes into the same struct the C++ side reads.  Translating
 * between two look-alike structs would be a copy per frame, and worse, a place
 * for the two to disagree.
 */

#include "util/report/alloc_csv_c.h"

#include "util/report/alloc_csv.h"

extern "C" {

void vesta_alloc_set_symbol_resolver(VestaAllocSymbolResolver fn) {
    util::alloc_set_symbol_resolver(fn);
}

void vesta_alloc_set_name_formatter(VestaAllocNameFormatter fn) {
    util::alloc_set_name_formatter(fn);
}

void vesta_alloc_set_module_classifier(VestaAllocModuleClassifier fn) {
    util::alloc_set_module_classifier(fn);
}

const char *vesta_alloc_root_of(const char *file_macro, const char *relative) {
    return util::alloc_root_of(file_macro, relative);
}

void vesta_alloc_declare_module(const char *dir, const char *name) {
    util::alloc_declare_module(dir, name);
}

void vesta_alloc_declare_module_of_file(const char *file_macro,
                                        const char *name) {
    util::alloc_declare_module_of_file(file_macro, name);
}

void vesta_alloc_declare_code(const VestaAllocCodeRange *ranges,
                              unsigned count) {
    util::alloc_declare_code(ranges, count);
}

void vesta_alloc_declare_file(const void *pc, const char *file) {
    util::alloc_declare_file(pc, file);
}

const char *vesta_alloc_readable_name(const char *raw) {
    return util::alloc_readable_name(raw);
}

int vesta_alloc_write_csv(const char *dir) {
    /* C has no `bool` before C99 and no guarantee that anyone included
     * <stdbool.h> afterwards, so the answer travels as an int.  Non-zero is
     * success, which is the convention the caller of a `write_*` expects. */
    return util::write_alloc_csv(dir) ? 1 : 0;
}

} // extern "C"
