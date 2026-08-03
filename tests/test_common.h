#ifndef TEST_COMMON_H
#define TEST_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Global counters */
static int passes = 0;
static int failures = 0;

/* Test macros */
#define ASSERT(cond) \
  do { \
    if (!(cond)) { \
      fprintf(stderr, "  FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++; \
    } else { \
      passes++; \
    } \
  } while (0)

#define ASSERT_STR_EQ(a, b) \
  do { \
    const char *_a = (a), *_b = (b); \
    if (!_a || !_b || strcmp(_a, _b) != 0) { \
      fprintf(stderr, "  FAIL: %s:%d: \"%s\" != \"%s\"\n", \
              __FILE__, __LINE__, _a ? _a : "(null)", _b ? _b : "(null)"); \
      failures++; \
    } else { \
      passes++; \
    } \
  } while (0)

#define ASSERT_STR_CONTAINS(haystack, needle) \
  do { \
    const char *_h = (haystack), *_n = (needle); \
    if (!_h || !_n || !strstr(_h, _n)) { \
      fprintf(stderr, "  FAIL: %s:%d: \"%s\" not found in \"%.80s...\"\n", \
              __FILE__, __LINE__, _n ? _n : "(null)", _h ? _h : "(null)"); \
      failures++; \
    } else { \
      passes++; \
    } \
  } while (0)

#define ASSERT_NOT_NULL(p) ASSERT((p) != NULL)
#define ASSERT_NULL(p) ASSERT((p) == NULL)
#define ASSERT_EQ(a, b) ASSERT((a) == (b))
#define ASSERT_GT(a, b) ASSERT((a) > (b))

#define RUN_TEST(name) \
  do { \
    printf("  %-40s", #name); \
    fflush(stdout); \
    name(); \
    printf("ok\n"); \
  } while (0)

#define TEST_SUMMARY() \
  do { \
    printf("\n  %d passed, %d failed\n", passes, failures); \
    return failures > 0 ? 1 : 0; \
  } while (0)

/* Create a temporary directory for test isolation */
static char *make_test_dir(void) {
  char tmpl[] = "/tmp/nash_test_XXXXXX";
  char *dir = mkdtemp(tmpl);
  if (!dir) {
    perror("mkdtemp");
    exit(1);
  }
  return strdup(dir);
}

/* Recursively remove a directory */
static void rm_rf(const char *path) {
  char cmd[4096];
  snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
  system(cmd);
}

#endif
