/*
 * SPDX-FileCopyrightText: 2026 Bruno Keymolen
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of Video Alarm Clock.
 *
 * Video Alarm Clock is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
/*
 * test_util.h - minimal test scaffolding.
 *
 * No dependencies, so the same tests build on the host and (later) inside an
 * ESP-IDF app. Write tests with TEST(...) and list them in a TEST_MAIN block:
 *
 *   TEST(create_destroy) { ... CHECK_EQ(NN20GEO_OK, rc); }
 *
 *   TEST_MAIN("lifecycle") {
 *       RUN(create_destroy);
 *   }
 *
 * A failing CHECK records the failure and lets the test continue;
 * REQUIRE aborts the current test, for when continuing would crash.
 */
#ifndef NN20VPS_TEST_UTIL_H
#define NN20VPS_TEST_UTIL_H

#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

static int nn20t_total;
static int nn20t_failed_tests;
static int nn20t_case_failures;
static jmp_buf nn20t_abort;

#define TEST(name) static void nn20t_case_##name(void)

#define RUN(name)                                                    \
    do {                                                             \
        nn20t_total++;                                               \
        nn20t_case_failures = 0;                                     \
        if (setjmp(nn20t_abort) == 0) {                              \
            nn20t_case_##name();                                     \
        }                                                            \
        if (nn20t_case_failures != 0) {                              \
            nn20t_failed_tests++;                                    \
            printf("  FAIL  %s\n", #name);                           \
        } else {                                                     \
            printf("  ok    %s\n", #name);                           \
        }                                                            \
    } while (0)

#define NN20T_FAIL(fmt, ...)                                         \
    do {                                                             \
        nn20t_case_failures++;                                       \
        printf("        %s:%d: " fmt "\n", __FILE__, __LINE__,       \
               __VA_ARGS__);                                         \
    } while (0)

#define CHECK(cond)                                                  \
    do {                                                             \
        if (!(cond)) {                                               \
            NN20T_FAIL("expected %s", #cond);                        \
        }                                                            \
    } while (0)

#define REQUIRE(cond)                                                \
    do {                                                             \
        if (!(cond)) {                                               \
            NN20T_FAIL("required %s", #cond);                        \
            longjmp(nn20t_abort, 1);                                 \
        }                                                            \
    } while (0)

/* Integer / enum equality. Both sides are widened to long long. */
#define CHECK_EQ(expected, actual)                                   \
    do {                                                             \
        const long long nn20t_e = (long long)(expected);             \
        const long long nn20t_a = (long long)(actual);               \
        if (nn20t_e != nn20t_a) {                                    \
            NN20T_FAIL("%s: expected %lld, got %lld", #actual,       \
                       nn20t_e, nn20t_a);                            \
        }                                                            \
    } while (0)

#define CHECK_NEAR(expected, actual, tol)                            \
    do {                                                             \
        const double nn20t_e = (double)(expected);                   \
        const double nn20t_a = (double)(actual);                     \
        if (!(fabs(nn20t_e - nn20t_a) <= (double)(tol))) {           \
            NN20T_FAIL("%s: expected %g +/- %g, got %g", #actual,    \
                       nn20t_e, (double)(tol), nn20t_a);             \
        }                                                            \
    } while (0)

#define CHECK_NAN(actual)                                            \
    do {                                                             \
        const double nn20t_a = (double)(actual);                     \
        if (!isnan(nn20t_a)) {                                       \
            NN20T_FAIL("%s: expected NAN, got %g", #actual, nn20t_a);\
        }                                                            \
    } while (0)

#define CHECK_STR_EQ(expected, actual)                               \
    do {                                                             \
        const char *nn20t_e = (expected);                            \
        const char *nn20t_a = (actual);                              \
        if (nn20t_a == NULL || strcmp(nn20t_e, nn20t_a) != 0) {      \
            NN20T_FAIL("%s: expected \"%s\", got \"%s\"", #actual,   \
                       nn20t_e, nn20t_a ? nn20t_a : "(null)");       \
        }                                                            \
    } while (0)

/* Defines main(). `suite` is printed as the suite heading. */
#define TEST_MAIN(suite)                                             \
    static void nn20t_suite(void);                                   \
    int main(void)                                                   \
    {                                                                \
        printf("%s\n", suite);                                       \
        nn20t_suite();                                               \
        printf("%s: %d/%d passed\n", suite,                          \
               nn20t_total - nn20t_failed_tests, nn20t_total);       \
        return nn20t_failed_tests == 0 ? 0 : 1;                      \
    }                                                                \
    static void nn20t_suite(void)

#endif /* NN20VPS_TEST_UTIL_H */
