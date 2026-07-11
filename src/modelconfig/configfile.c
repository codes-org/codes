/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include "codes_config.h"
#include <string.h>
#include <stdio.h>
#include <assert.h>
#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif

#include <codes/configfile.h>
#include "txt_configfile.h"

/* Build "path/name" (or just "name" at the root) into out, for diagnostics. */
static void cf_join_path(char* out, size_t outsz, const char* path, const char* name) {
    if (path && path[0])
        snprintf(out, outsz, "%s/%s", path, name);
    else
        snprintf(out, outsz, "%s", name ? name : "");
}

/* Does a section named `name` (case-insensitive, per the store) or a key named
 * `name` (case-sensitive) exist directly under section `s`? Used to decide
 * whether an entry seen only in the second tree is genuinely missing from the
 * first (rather than already reported by the forward pass as a type mismatch). */
static int cf_has_entry(struct ConfigVTable* h, SectionHandle s, const char* name) {
    SectionHandle sec;
    char** ptrs = NULL;
    size_t n = 0;
    if (cf_openSection(h, s, name, &sec) == 1) {
        cf_closeSection(h, sec);
        return 1;
    }
    if (cf_getMultiKey(h, s, name, &ptrs, &n) >= 0) {
        for (size_t j = 0; j < n; ++j)
            free(ptrs[j]);
        free(ptrs);
        return 1;
    }
    return 0;
}

/* Compare two value lists element-wise (a NULL value reads as ""). */
static int cf_value_lists_equal(char** a, size_t na, char** b, size_t nb) {
    if (na != nb)
        return 0;
    for (size_t j = 0; j < na; ++j)
        if (strcmp(a[j] ? a[j] : "", b[j] ? b[j] : ""))
            return 0;
    return 1;
}

static int cf_print_values(FILE* f, char** vals, size_t n) {
    fputc('(', f);
    for (size_t j = 0; j < n; ++j)
        fprintf(f, "%s\"%s\"", j ? ", " : "", vals[j] ? vals[j] : "");
    return fputc(')', f);
}

/*
 * Order-insensitive structural comparison of two config sections.
 *
 * The config store keys every lookup by name -- sections case-insensitively,
 * keys case-sensitively -- and never by position, so two trees describe the same
 * configuration when they hold the same named entries with the same values,
 * regardless of the order those entries were written. Comparing a key by its full
 * value *list* also subsumes the SE_KEY / SE_MULTIKEY split (which the store
 * derives purely from the value count), and the *order of values within* a key is
 * still significant and is checked -- so a list-valued key like modelnet_order is
 * compared as an ordered tuple.
 *
 * `path` is the section path so far, for diagnostics. When `report` is non-NULL
 * every divergence is written to it (all of them, not just the first) under its
 * path, so a failing comparison names the exact section/key that diverged. When
 * `report` is NULL the comparison short-circuits on the first divergence.
 * Returns 1 if the two sections are equal, 0 otherwise.
 */
static int cf_section_equal(struct ConfigVTable* h1, SectionHandle s1, struct ConfigVTable* h2,
                            SectionHandle s2, const char* path, FILE* report) {
    unsigned int size1 = 0;
    unsigned int size2 = 0;
    size_t count1;
    size_t count2;
    size_t i;
    int eq = 1;
    char child[1024];

    cf_getSectionSize(h1, s1, &size1);
    cf_getSectionSize(h2, s2, &size2);
    count1 = size1;
    count2 = size2;

    SectionEntry entries1[size1 ? size1 : 1];
    SectionEntry entries2[size2 ? size2 : 1];
    cf_listSection(h1, s1, &entries1[0], &count1);
    cf_listSection(h2, s2, &entries2[0], &count2);

    /* Forward pass: every entry in tree 1 must appear -- same kind, same value --
     * in tree 2. */
    for (i = 0; i < count1; ++i) {
        const char* name = entries1[i].name;
        cf_join_path(child, sizeof(child), path, name);

        if (entries1[i].type == SE_SECTION) {
            SectionHandle c1;
            SectionHandle c2;
            if (cf_openSection(h2, s2, name, &c2) != 1) {
                if (report)
                    fprintf(report, "  %s: section only in first tree\n", child);
                eq = 0;
            } else {
                cf_openSection(h1, s1, name, &c1);
                if (!cf_section_equal(h1, c1, h2, c2, child, report))
                    eq = 0;
                cf_closeSection(h1, c1);
                cf_closeSection(h2, c2);
            }
        } else {
            /* key or multikey in tree 1 */
            char** p1 = NULL;
            char** p2 = NULL;
            size_t n1 = 0;
            size_t n2 = 0;
            size_t j;
            int have2 = (cf_getMultiKey(h2, s2, name, &p2, &n2) >= 0);
            cf_getMultiKey(h1, s1, name, &p1, &n1); /* present by construction */

            if (!have2) {
                SectionHandle sec2;
                if (cf_openSection(h2, s2, name, &sec2) == 1) {
                    cf_closeSection(h2, sec2);
                    if (report)
                        fprintf(report, "  %s: a key in the first tree, a section in the second\n",
                                child);
                } else if (report) {
                    fprintf(report, "  %s: key only in first tree\n", child);
                }
                eq = 0;
            } else if (!cf_value_lists_equal(p1, n1, p2, n2)) {
                if (report) {
                    fprintf(report, "  %s: values differ: ", child);
                    cf_print_values(report, p1, n1);
                    fprintf(report, " vs ");
                    cf_print_values(report, p2, n2);
                    fputc('\n', report);
                }
                eq = 0;
            }

            for (j = 0; j < n1; ++j)
                free(p1[j]);
            for (j = 0; j < n2; ++j)
                free(p2[j]);
            free(p1);
            free(p2);
        }

        if (!eq && !report)
            break;
    }

    /* Reverse pass: an entry present only in tree 2 (a type mismatch was already
     * reported by the forward pass, so only flag names the first tree lacks
     * entirely). */
    if (eq || report) {
        for (i = 0; i < count2; ++i) {
            const char* name = entries2[i].name;
            if (!cf_has_entry(h1, s1, name)) {
                cf_join_path(child, sizeof(child), path, name);
                if (report)
                    fprintf(report, "  %s: %s only in second tree\n", child,
                            entries2[i].type == SE_SECTION ? "section" : "key");
                eq = 0;
                if (!report)
                    break;
            }
        }
    }

    for (i = 0; i < count1; ++i)
        free((char*)entries1[i].name);
    for (i = 0; i < count2; ++i)
        free((char*)entries2[i].name);

    return eq;
}

int cf_equal_report(struct ConfigVTable* h1, struct ConfigVTable* h2, FILE* report) {
    return cf_section_equal(h1, ROOT_SECTION, h2, ROOT_SECTION, "", report);
}

int cf_equal(struct ConfigVTable* h1, struct ConfigVTable* h2) {
    return cf_section_equal(h1, ROOT_SECTION, h2, ROOT_SECTION, "", NULL);
}


int cf_dump(struct ConfigVTable* cf, SectionHandle h, char** err) {
    return txtfile_writeConfig(cf, h, stdout, err);
}
