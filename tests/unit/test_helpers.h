/* Minimal assertion harness: no external test framework, so the test build
 * has the same dependencies as the shell itself. */
#ifndef MSH_TEST_HELPERS_H
#define MSH_TEST_HELPERS_H

#include <stdio.h>

static int msh_test_failures;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__,      \
                    #cond);                                                \
            msh_test_failures++;                                           \
        }                                                                  \
    } while (0)

#define RUN(fn)                                                            \
    do {                                                                   \
        int before = msh_test_failures;                                    \
        fn();                                                              \
        printf("  %-40s %s\n", #fn,                                        \
               msh_test_failures == before ? "ok" : "FAILED");             \
    } while (0)

#define TEST_SUMMARY()                                                     \
    (msh_test_failures == 0                                                \
         ? (printf("  all checks passed\n"), 0)                            \
         : (fprintf(stderr, "  %d check(s) failed\n", msh_test_failures),  \
            1))

#endif /* MSH_TEST_HELPERS_H */
