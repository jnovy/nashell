/* test_breadcrumbs.c — Unit tests for structural breadcrumb extraction
 * via repomap_file_symbols().
 *
 * Tests the repomap-based symbol extraction used by eviction breadcrumbs
 * to produce structural briefs ("func(), struct_t, MACRO") instead of
 * raw first-N-bytes from evicted file_read results.
 */

#include "test_common.h"
#include "../src/repomap.h"

/* ── Test Helpers ──────────────────────────────────────────────────── */

/* Helper: call repomap_file_symbols with a static buffer */
static int extract(const char *content, const char *filename,
                   char *out, int out_cap) {
  return repomap_file_symbols(content, (int)strlen(content),
                              filename, out, out_cap);
}

/* ── Tests: NULL/Edge Cases ────────────────────────────────────────── */

static void test_null_content(void) {
  char out[256];
  int n = repomap_file_symbols(NULL, 0, "foo.c", out, sizeof(out));
  ASSERT_EQ(n, 0);
  ASSERT_STR_EQ(out, "");
}

static void test_null_filename(void) {
  char out[256];
  int n = repomap_file_symbols("int x;", 6, NULL, out, sizeof(out));
  ASSERT_EQ(n, 0);
}

static void test_null_output(void) {
  /* Should not crash */
  int n = repomap_file_symbols("int x;", 6, "foo.c", NULL, 0);
  ASSERT_EQ(n, 0);
}

static void test_empty_content(void) {
  char out[256];
  int n = extract("", "foo.c", out, sizeof(out));
  ASSERT_EQ(n, 0);
}

static void test_unsupported_extension(void) {
  char out[256];
  int n = extract("def foo():\n    pass\n", "foo.py", out, sizeof(out));
  ASSERT_EQ(n, 0); /* No Python extractor yet */
}

static void test_no_extension(void) {
  char out[256];
  int n = extract("int main() { return 0; }\n", "Makefile", out, sizeof(out));
  ASSERT_EQ(n, 0); /* No dot in filename */
}

/* ── Tests: C Function Definitions ────────────────────────────────── */

static void test_single_function(void) {
  const char *src =
    "#include <stdio.h>\n"
    "\n"
    "int main(int argc, char **argv) {\n"
    "    return 0;\n"
    "}\n";
  char out[256];
  int n = extract(src, "test.c", out, sizeof(out));
  ASSERT_GT(n, 0);
  ASSERT_STR_CONTAINS(out, "main()");
}

static void test_multiple_functions(void) {
  const char *src =
    "static int helper(int x) {\n"
    "    return x + 1;\n"
    "}\n"
    "\n"
    "void process(const char *s) {\n"
    "    printf(\"%s\", s);\n"
    "}\n"
    "\n"
    "int main(void) {\n"
    "    return 0;\n"
    "}\n";
  char out[256];
  int n = extract(src, "prog.c", out, sizeof(out));
  ASSERT_GT(n, 0);
  ASSERT_STR_CONTAINS(out, "helper()");
  ASSERT_STR_CONTAINS(out, "process()");
  ASSERT_STR_CONTAINS(out, "main()");
  /* Check comma separation */
  ASSERT_STR_CONTAINS(out, ", ");
}

static void test_static_function(void) {
  const char *src =
    "static void internal_helper(int x) {\n"
    "    (void)x;\n"
    "}\n";
  char out[256];
  int n = extract(src, "util.c", out, sizeof(out));
  ASSERT_GT(n, 0);
  ASSERT_STR_CONTAINS(out, "internal_helper()");
}

/* ── Tests: Macros ────────────────────────────────────────────────── */

static void test_define_macro(void) {
  const char *src =
    "#define MAX_BUFFER_SIZE 4096\n"
    "#define MIN_VALUE 0\n"
    "\n"
    "int func(void) { return MAX_BUFFER_SIZE; }\n";
  char out[256];
  int n = extract(src, "defs.h", out, sizeof(out));
  ASSERT_GT(n, 0);
  ASSERT_STR_CONTAINS(out, "MAX_BUFFER_SIZE");
  /* MIN_VALUE is only 9 chars, might be filtered by len > 2 check */
}

static void test_short_macro_skipped(void) {
  /* Macros with name <= 2 chars are skipped by extract_c_tags */
  const char *src =
    "#define X 1\n"
    "#define AB 2\n";
  char out[256];
  int n = extract(src, "short.h", out, sizeof(out));
  ASSERT_EQ(n, 0); /* Both too short */
}

/* ── Tests: Types (typedef, struct, enum) ─────────────────────────── */

static void test_typedef_simple(void) {
  const char *src =
    "typedef unsigned int uint32_t;\n"
    "typedef void (*callback_fn)(int);\n";
  char out[256];
  int n = extract(src, "types.h", out, sizeof(out));
  ASSERT_GT(n, 0);
  ASSERT_STR_CONTAINS(out, "uint32_t");
}

static void test_typedef_struct(void) {
  const char *src =
    "typedef struct {\n"
    "    int x;\n"
    "    int y;\n"
    "} point_t;\n";
  char out[256];
  int n = extract(src, "geom.h", out, sizeof(out));
  ASSERT_GT(n, 0);
  ASSERT_STR_CONTAINS(out, "point_t");
}

static void test_struct_definition(void) {
  const char *src =
    "struct config_entry {\n"
    "    char *key;\n"
    "    char *value;\n"
    "};\n";
  char out[256];
  int n = extract(src, "config.h", out, sizeof(out));
  ASSERT_GT(n, 0);
  ASSERT_STR_CONTAINS(out, "config_entry");
}

static void test_enum_definition(void) {
  const char *src =
    "enum color {\n"
    "    RED,\n"
    "    GREEN,\n"
    "    BLUE\n"
    "};\n";
  char out[256];
  int n = extract(src, "colors.h", out, sizeof(out));
  ASSERT_GT(n, 0);
  ASSERT_STR_CONTAINS(out, "color");
}

/* ── Tests: Mixed Content ─────────────────────────────────────────── */

static void test_mixed_symbols(void) {
  const char *src =
    "/* A sample header */\n"
    "#ifndef SAMPLE_H\n"
    "#define SAMPLE_H\n"
    "\n"
    "#define MAX_ITEMS 100\n"
    "\n"
    "typedef struct {\n"
    "    int count;\n"
    "    char *name;\n"
    "} item_t;\n"
    "\n"
    "enum status {\n"
    "    STATUS_OK,\n"
    "    STATUS_ERR\n"
    "};\n"
    "\n"
    "int item_create(const char *name);\n"
    "void item_destroy(item_t *item);\n"
    "\n"
    "#endif\n";
  char out[512];
  int n = extract(src, "sample.h", out, sizeof(out));
  ASSERT_GT(n, 0);
  /* Should find: SAMPLE_H, MAX_ITEMS, item_t, status, item_create, item_destroy */
  ASSERT_STR_CONTAINS(out, "MAX_ITEMS");
  ASSERT_STR_CONTAINS(out, "item_t");
  ASSERT_STR_CONTAINS(out, "status");
  ASSERT_STR_CONTAINS(out, "item_create()");
  ASSERT_STR_CONTAINS(out, "item_destroy()");
  /* Function decorations */
  ASSERT_STR_CONTAINS(out, "()"); /* at least one function suffix */
}

/* ── Tests: Comment Handling ──────────────────────────────────────── */

static void test_block_comment_skipped(void) {
  const char *src =
    "/* This is a block comment\n"
    "int not_a_function(void) {\n"
    "*/\n"
    "int real_function(void) {\n"
    "    return 0;\n"
    "}\n";
  char out[256];
  int n = extract(src, "comment.c", out, sizeof(out));
  ASSERT_GT(n, 0);
  ASSERT_STR_CONTAINS(out, "real_function()");
  /* not_a_function should NOT appear (inside block comment) */
  ASSERT(strstr(out, "not_a_function") == NULL);
}

static void test_line_comment_skipped(void) {
  const char *src =
    "// int commented_out(void) {\n"
    "int actual(void) {\n"
    "    return 1;\n"
    "}\n";
  char out[256];
  int n = extract(src, "lc.c", out, sizeof(out));
  ASSERT_GT(n, 0);
  ASSERT_STR_CONTAINS(out, "actual()");
  ASSERT(strstr(out, "commented_out") == NULL);
}

/* ── Tests: Control Flow Keywords Filtered ────────────────────────── */

static void test_keywords_not_functions(void) {
  /* Control-flow keywords at column 0 with parens should NOT be
     * extracted as function definitions. */
  const char *src =
    "int real_func(void) {\n"
    "    if (x) return 1;\n"
    "    for (i = 0; i < n; i++) {\n"
    "        while (running) {\n"
    "            switch (state) {\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "    return 0;\n"
    "}\n";
  char out[256];
  int n = extract(src, "flow.c", out, sizeof(out));
  ASSERT_GT(n, 0);
  ASSERT_STR_CONTAINS(out, "real_func()");
  /* if/for/while/switch should not appear */
  ASSERT(strstr(out, "if()") == NULL);
  ASSERT(strstr(out, "for()") == NULL);
  ASSERT(strstr(out, "while()") == NULL);
  ASSERT(strstr(out, "switch()") == NULL);
}

/* ── Tests: Output Buffer Limits ──────────────────────────────────── */

static void test_small_buffer(void) {
  const char *src =
    "int function_alpha(void) { return 0; }\n"
    "int function_beta(void) { return 0; }\n"
    "int function_gamma(void) { return 0; }\n";
  /* Buffer too small for all three */
  char out[25];
  int n = extract(src, "small.c", out, sizeof(out));
  ASSERT_GT(n, 0);
  ASSERT(n < 25);
  /* At least one function should fit */
  ASSERT_STR_CONTAINS(out, "()");
  /* Output should be properly NUL-terminated */
  ASSERT_EQ((int)strlen(out), n);
}

static void test_buffer_cap_1(void) {
  char out[1];
  int n = repomap_file_symbols("int f(void){}", 13, "x.c", out, 1);
  ASSERT_EQ(n, 0);
  ASSERT_EQ(out[0], '\0');
}

/* ── Tests: C++ Extensions ────────────────────────────────────────── */

static void test_cpp_extension(void) {
  const char *src =
    "void cpp_function(int x) {\n"
    "    (void)x;\n"
    "}\n";
  char out[256];
  /* .cpp extension */
  int n = extract(src, "test.cpp", out, sizeof(out));
  ASSERT_GT(n, 0);
  ASSERT_STR_CONTAINS(out, "cpp_function()");
}

static void test_hpp_extension(void) {
  const char *src =
    "struct Widget {\n"
    "    int width;\n"
    "};\n";
  char out[256];
  int n = extract(src, "widget.hpp", out, sizeof(out));
  ASSERT_GT(n, 0);
  ASSERT_STR_CONTAINS(out, "Widget");
}

/* ── Tests: Header Function Declarations ──────────────────────────── */

static void test_header_declarations(void) {
  const char *src =
    "#ifndef API_H\n"
    "#define API_H\n"
    "\n"
    "int api_init(const char *path);\n"
    "void api_shutdown(void);\n"
    "char *api_query(const char *q, int flags);\n"
    "\n"
    "#endif\n";
  char out[512];
  int n = extract(src, "api.h", out, sizeof(out));
  ASSERT_GT(n, 0);
  ASSERT_STR_CONTAINS(out, "api_init()");
  ASSERT_STR_CONTAINS(out, "api_shutdown()");
  ASSERT_STR_CONTAINS(out, "api_query()");
}

/* ── Tests: Realistic File Content ────────────────────────────────── */

static void test_realistic_header(void) {
  /* Simulates a real Nash-style header */
  const char *src =
    "/* eviction.h -- Context eviction API */\n"
    "#ifndef EVICTION_H\n"
    "#define EVICTION_H\n"
    "\n"
    "#include \"llm.h\"\n"
    "#include \"config.h\"\n"
    "\n"
    "#define EVICT_FLOOR_PCT     20\n"
    "#define EVICT_TRIGGER_PCT   90\n"
    "#define EVICT_TARGET_PCT    70\n"
    "\n"
    "typedef struct {\n"
    "    int *partner;\n"
    "    int n_msgs;\n"
    "} partner_map_t;\n"
    "\n"
    "typedef enum {\n"
    "    SCORE_LOW,\n"
    "    SCORE_NORMAL,\n"
    "    SCORE_HIGH\n"
    "} score_level_t;\n"
    "\n"
    "int evict_score(const void *chat, int mi, int ri, int n, void *ud);\n"
    "int evict_mark(const void *chat, int start, int end);\n"
    "void evict_sweep(void *chat, const int *marks, int n);\n"
    "\n"
    "#endif\n";
  char out[512];
  int n = extract(src, "eviction.h", out, sizeof(out));
  ASSERT_GT(n, 0);
  /* Macros */
  ASSERT_STR_CONTAINS(out, "EVICT_FLOOR_PCT");
  ASSERT_STR_CONTAINS(out, "EVICT_TRIGGER_PCT");
  ASSERT_STR_CONTAINS(out, "EVICT_TARGET_PCT");
  /* Types */
  ASSERT_STR_CONTAINS(out, "partner_map_t");
  ASSERT_STR_CONTAINS(out, "score_level_t");
  /* Functions */
  ASSERT_STR_CONTAINS(out, "evict_score()");
  ASSERT_STR_CONTAINS(out, "evict_mark()");
  ASSERT_STR_CONTAINS(out, "evict_sweep()");
}

/* ── Main ─────────────────────────────────────────────────────────── */

int main(void) {
  printf("test_breadcrumbs:\n");

  /* Edge cases */
  RUN_TEST(test_null_content);
  RUN_TEST(test_null_filename);
  RUN_TEST(test_null_output);
  RUN_TEST(test_empty_content);
  RUN_TEST(test_unsupported_extension);
  RUN_TEST(test_no_extension);

  /* Function definitions */
  RUN_TEST(test_single_function);
  RUN_TEST(test_multiple_functions);
  RUN_TEST(test_static_function);

  /* Macros */
  RUN_TEST(test_define_macro);
  RUN_TEST(test_short_macro_skipped);

  /* Types */
  RUN_TEST(test_typedef_simple);
  RUN_TEST(test_typedef_struct);
  RUN_TEST(test_struct_definition);
  RUN_TEST(test_enum_definition);

  /* Mixed content */
  RUN_TEST(test_mixed_symbols);

  /* Comment handling */
  RUN_TEST(test_block_comment_skipped);
  RUN_TEST(test_line_comment_skipped);

  /* Keyword filtering */
  RUN_TEST(test_keywords_not_functions);

  /* Buffer limits */
  RUN_TEST(test_small_buffer);
  RUN_TEST(test_buffer_cap_1);

  /* C++ extensions */
  RUN_TEST(test_cpp_extension);
  RUN_TEST(test_hpp_extension);

  /* Header declarations */
  RUN_TEST(test_header_declarations);

  /* Realistic content */
  RUN_TEST(test_realistic_header);

  TEST_SUMMARY();
}
