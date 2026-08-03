#include "test_common.h"
#include "../src/str.h"
/* linked via LIB_OBJ */

/* ── test_new_and_free ── */
static void test_new_and_free(void) {
  str_t s = str_new(64);
  ASSERT_NOT_NULL(s.data);
  ASSERT_EQ((int)s.len, 0);
  ASSERT_EQ(s.data[0], '\0');
  str_free(&s);
  ASSERT_NULL(s.data);
}

/* ── test_append ── */
static void test_append(void) {
  str_t s = str_new(8);
  str_append(&s, "hello", 5);
  ASSERT_EQ((int)s.len, 5);
  ASSERT_STR_EQ(s.data, "hello");

  str_append(&s, " world", 6);
  ASSERT_EQ((int)s.len, 11);
  ASSERT_STR_EQ(s.data, "hello world");

  str_free(&s);
}

/* ── test_append_cstr ── */
static void test_append_cstr(void) {
  str_t s = str_new(4);
  str_append_cstr(&s, "foo");
  str_append_cstr(&s, "bar");
  ASSERT_STR_EQ(s.data, "foobar");
  ASSERT_EQ((int)s.len, 6);
  str_free(&s);
}

/* ── test_appendf ── */
static void test_appendf(void) {
  str_t s = str_new(16);
  str_appendf(&s, "x=%d y=%s", 42, "hello");
  ASSERT_STR_EQ(s.data, "x=42 y=hello");
  str_free(&s);
}

/* ── test_clear ── */
static void test_clear(void) {
  str_t s = str_new(16);
  str_append_cstr(&s, "some data");
  ASSERT_GT((int)s.len, 0);
  str_clear(&s);
  ASSERT_EQ((int)s.len, 0);
  ASSERT_EQ(s.data[0], '\0');
  str_free(&s);
}

/* ── test_steal ── */
static void test_steal(void) {
  str_t s = str_new(16);
  str_append_cstr(&s, "stolen");
  char *p = str_steal(&s);
  ASSERT_STR_EQ(p, "stolen");
  ASSERT_NULL(s.data);
  ASSERT_EQ((int)s.len, 0);
  free(p);
}

/* ── test_grow ── */
static void test_grow(void) {
  str_t s = str_new(4); /* tiny initial cap */
  /* Append more than initial capacity */
  for (int i = 0; i < 100; i++)
    str_append_cstr(&s, "x");
  ASSERT_EQ((int)s.len, 100);
  ASSERT_EQ(s.data[100], '\0');
  str_free(&s);
}

/* ── test_cstr ── */
static void test_cstr(void) {
  str_t s = str_new(8);
  ASSERT_STR_EQ(str_cstr(&s), "");
  str_append_cstr(&s, "test");
  ASSERT_STR_EQ(str_cstr(&s), "test");
  str_free(&s);

  /* str_cstr on freed str should return "" */
  ASSERT_STR_EQ(str_cstr(&s), "");
}

int main(void) {
  printf("test_str:\n");
  RUN_TEST(test_new_and_free);
  RUN_TEST(test_append);
  RUN_TEST(test_append_cstr);
  RUN_TEST(test_appendf);
  RUN_TEST(test_clear);
  RUN_TEST(test_steal);
  RUN_TEST(test_grow);
  RUN_TEST(test_cstr);
  TEST_SUMMARY();
}
