/*
 * test_fswatch.c - Test the fswatch file watching abstraction.
 *
 * Tests:
 *   1. Init / free lifecycle
 *   2. Add a directory watch
 *   3. Detect file creation
 *   4. Detect file modification
 *   5. Detect file deletion
 *   6. Recursive subdirectory watching
 *   7. New subdirectories are auto-watched
 */
#include "fswatch.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_pass = 0;
static int g_fail = 0;

#define ASSERT(cond, msg) \
  do { \
    if (!(cond)) { \
      fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); \
      g_fail++; \
    } else { \
      printf("  PASS: %s\n", msg); \
      g_pass++; \
    } \
  } while (0)

/* Callback state */
typedef struct {
  char last_path[PATH_MAX];
  int last_event;
  int count;
} cb_state_t;

static void test_cb(const char *path, int event, void *userdata) {
  cb_state_t *st = (cb_state_t *)userdata;
  if (path) {
    strncpy(st->last_path, path, PATH_MAX - 1);
    st->last_path[PATH_MAX - 1] = '\0';
  }
  st->last_event = event;
  st->count++;
}

/* Wait for backend events (up to timeout_ms). Returns events drained. */
static int wait_and_drain(fswatch_t *w, int timeout_ms) {
  int fd = fswatch_fd(w);
  if (fd >= 0) {
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    poll(&pfd, 1, timeout_ms);
  } else {
    /* No pollable fd (noop backend) - just sleep briefly */
    usleep((unsigned)(timeout_ms * 1000));
  }
  return fswatch_drain(w);
}

/* Create a temporary directory for testing */
static char *make_tmpdir(void) {
  static char tmpl[PATH_MAX];
  snprintf(tmpl, sizeof(tmpl), "%s/fswatch_test_XXXXXX",
           getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
  char *d = mkdtemp(tmpl);
  if (!d) {
    perror("mkdtemp");
    exit(1);
  }
  return d;
}

/* Write a string to a file */
static void write_file(const char *path, const char *content) {
  FILE *f = fopen(path, "w");
  if (!f) {
    perror(path);
    return;
  }
  fputs(content, f);
  fclose(f);
}

/* Recursive rm -rf */
static void rmrf(const char *path) {
  char cmd[PATH_MAX + 16];
  snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
  (void)system(cmd);
}

static void test_init_free(void) {
  printf("\n--- test_init_free ---\n");
  cb_state_t st = {0};
  fswatch_t *w = fswatch_init(test_cb, &st);
  ASSERT(w != NULL, "fswatch_init returns non-NULL");
  fswatch_free(w);
  ASSERT(1, "fswatch_free does not crash");

  /* NULL callback should fail */
  fswatch_t *w2 = fswatch_init(NULL, NULL);
  ASSERT(w2 == NULL, "fswatch_init with NULL cb returns NULL");

  /* free(NULL) should be safe */
  fswatch_free(NULL);
  ASSERT(1, "fswatch_free(NULL) does not crash");
}

static void test_add_watch(void) {
  printf("\n--- test_add_watch ---\n");
  cb_state_t st = {0};
  fswatch_t *w = fswatch_init(test_cb, &st);
  char *dir = make_tmpdir();

  int rc = fswatch_add(w, dir, 0);
  ASSERT(rc == 0, "fswatch_add succeeds for tmpdir");

  int fd = fswatch_fd(w);
#if defined(__linux__) || defined(__APPLE__)
  ASSERT(fd >= 0, "fswatch_fd returns a valid native watcher fd");
#else
  ASSERT(fd == -1, "fswatch_fd returns -1 on non-Linux");
#endif

  fswatch_free(w);
  rmrf(dir);
}

static void test_create_detect(void) {
  printf("\n--- test_create_detect ---\n");
  cb_state_t st = {0};
  fswatch_t *w = fswatch_init(test_cb, &st);
  char *dir = make_tmpdir();
  fswatch_add(w, dir, 0);

  /* Create a file */
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/newfile.txt", dir);
  write_file(path, "hello");

  int n = wait_and_drain(w, 1000);
#if defined(__linux__) || defined(__APPLE__)
  ASSERT(n > 0, "events detected after file creation");
  ASSERT(st.count > 0, "callback invoked");
  ASSERT(strstr(st.last_path, "newfile.txt") != NULL,
         "callback path contains filename");
#else
  ASSERT(n == 0, "noop backend returns 0 events");
#endif

  fswatch_free(w);
  rmrf(dir);
}

static void test_modify_detect(void) {
  printf("\n--- test_modify_detect ---\n");
  cb_state_t st = {0};
  fswatch_t *w = fswatch_init(test_cb, &st);
  char *dir = make_tmpdir();
  fswatch_add(w, dir, 0);

  /* Create file first */
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/existing.txt", dir);
  write_file(path, "original");

  /* Drain creation events */
  wait_and_drain(w, 100);
  st.count = 0;
  st.last_event = 0;

  /* Modify the file */
  write_file(path, "modified content");

  int n = wait_and_drain(w, 1000);
#if defined(__linux__) || defined(__APPLE__)
  ASSERT(n > 0, "events detected after file modification");
  ASSERT(st.last_event & FSW_MODIFY, "event includes FSW_MODIFY");
#else
  (void)n;
  ASSERT(1, "noop backend - skip modify test");
#endif

  fswatch_free(w);
  rmrf(dir);
}

static void test_delete_detect(void) {
  printf("\n--- test_delete_detect ---\n");
  cb_state_t st = {0};
  fswatch_t *w = fswatch_init(test_cb, &st);
  char *dir = make_tmpdir();
  fswatch_add(w, dir, 0);

  /* Create then delete */
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/todelete.txt", dir);
  write_file(path, "delete me");
  wait_and_drain(w, 100);
  st.count = 0;

  unlink(path);
  int n = wait_and_drain(w, 1000);
#if defined(__linux__) || defined(__APPLE__)
  ASSERT(n > 0, "events detected after file deletion");
  ASSERT(st.last_event & FSW_DELETE, "event includes FSW_DELETE");
#else
  (void)n;
  ASSERT(1, "noop backend - skip delete test");
#endif

  fswatch_free(w);
  rmrf(dir);
}

static void test_recursive_watch(void) {
  printf("\n--- test_recursive_watch ---\n");
  cb_state_t st = {0};
  fswatch_t *w = fswatch_init(test_cb, &st);
  char *dir = make_tmpdir();

  /* Create a subdirectory before watching */
  char subdir[PATH_MAX];
  snprintf(subdir, sizeof(subdir), "%s/sub", dir);
  mkdir(subdir, 0755);

  fswatch_add(w, dir, 1); /* recursive */

  /* Create a file in the subdirectory */
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/sub/deep.txt", dir);
  write_file(path, "deep content");

  int n = wait_and_drain(w, 1000);
#if defined(__linux__) || defined(__APPLE__)
  ASSERT(n > 0, "events detected in subdirectory");
  ASSERT(strstr(st.last_path, "deep.txt") != NULL,
         "callback path contains subdirectory filename");
#else
  (void)n;
  ASSERT(1, "noop backend - skip recursive test");
#endif

  fswatch_free(w);
  rmrf(dir);
}

static void test_auto_watch_new_subdir(void) {
  printf("\n--- test_auto_watch_new_subdir ---\n");
  cb_state_t st = {0};
  fswatch_t *w = fswatch_init(test_cb, &st);
  char *dir = make_tmpdir();
  fswatch_add(w, dir, 1); /* recursive */

  /* Create a new subdirectory AFTER watching started */
  char newdir[PATH_MAX];
  snprintf(newdir, sizeof(newdir), "%s/newsubdir", dir);
  mkdir(newdir, 0755);

  /* Wait for the dir creation event to be processed */
  wait_and_drain(w, 200);
  st.count = 0;

  /* Create a file in the new subdirectory */
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/newsubdir/auto.txt", dir);
  write_file(path, "auto-watched");

  int n = wait_and_drain(w, 1000);
#if defined(__linux__) || defined(__APPLE__)
  ASSERT(n > 0, "events detected in auto-watched new subdirectory");
  ASSERT(strstr(st.last_path, "auto.txt") != NULL,
         "callback path contains new subdir filename");
#else
  (void)n;
  ASSERT(1, "noop backend - skip auto-watch test");
#endif

  fswatch_free(w);
  rmrf(dir);
}

static void test_hidden_dirs_skipped(void) {
  printf("\n--- test_hidden_dirs_skipped ---\n");
  cb_state_t st = {0};
  fswatch_t *w = fswatch_init(test_cb, &st);
  char *dir = make_tmpdir();

  /* Create a hidden directory before watching */
  char hidden[PATH_MAX];
  snprintf(hidden, sizeof(hidden), "%s/.hidden", dir);
  mkdir(hidden, 0755);

  fswatch_add(w, dir, 1); /* recursive */

  /* Create file in hidden dir */
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/.hidden/secret.txt", dir);
  write_file(path, "hidden content");

  int n = wait_and_drain(w, 1000);
#ifdef __linux__
  /* The hidden directory is not watched, so no events for files inside it.
   * However, the parent dir IS watched, so creating .hidden itself
   * generates an event. The file inside .hidden should NOT. */
  int found_secret = (strstr(st.last_path, "secret.txt") != NULL);
  ASSERT(!found_secret, "hidden directory contents not watched");
#elif defined(__APPLE__)
  ASSERT(n > 0, "events detected in hidden subdirectory");
  ASSERT(strstr(st.last_path, "secret.txt") != NULL,
         "kqueue reports hidden-directory contents");
#else
  (void)n;
  ASSERT(1, "noop backend - skip hidden dir test");
#endif

  fswatch_free(w);
  rmrf(dir);
}

static void test_recreate_watch(void) {
  printf("\n--- test_recreate_watch ---\n");
  cb_state_t st = {0};
  fswatch_t *w = fswatch_init(test_cb, &st);
  char *dir = make_tmpdir();
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/recreated.txt", dir);
  write_file(path, "before");

  ASSERT(fswatch_add(w, path, 0) == 0, "watch initial file");
  unlink(path);
  wait_and_drain(w, 1000);
  write_file(path, "after");
  ASSERT(fswatch_add(w, path, 0) == 0, "watch recreated file");
  st.count = 0;
  write_file(path, "updated");
  int n = wait_and_drain(w, 1000);
#ifdef __APPLE__
  ASSERT(n > 0, "events detected after recreating watched path");
  ASSERT(strstr(st.last_path, "recreated.txt") != NULL,
         "callback path is recreated file");
#else
  (void)n;
  ASSERT(1, "recreate watch test is specific to kqueue");
#endif

  fswatch_free(w);
  rmrf(dir);
}

static void test_nonrecursive_watch_stays_shallow(void) {
  printf("\n--- test_nonrecursive_watch_stays_shallow ---\n");
  cb_state_t st = {0};
  fswatch_t *w = fswatch_init(test_cb, &st);
  char *dir = make_tmpdir();
  char subdir[PATH_MAX], rootfile[PATH_MAX], nested[PATH_MAX];
  snprintf(subdir, sizeof(subdir), "%s/sub", dir);
  snprintf(rootfile, sizeof(rootfile), "%s/root.txt", dir);
  snprintf(nested, sizeof(nested), "%s/sub/deep.txt", dir);
  mkdir(subdir, 0755);

  ASSERT(fswatch_add(w, dir, 0) == 0, "add non-recursive directory watch");
  write_file(rootfile, "root event");
  wait_and_drain(w, 1000);
  st.count = 0;
  write_file(nested, "nested event");
  int n = wait_and_drain(w, 250);
#ifdef __APPLE__
  ASSERT(n == 0, "non-recursive watch ignores nested changes");
#else
  (void)n;
  ASSERT(1, "non-recursive behavior is tested by the kqueue backend");
#endif

  fswatch_free(w);
  rmrf(dir);
}

int main(void) {
  printf("=== test_fswatch ===\n");

  test_init_free();
  test_add_watch();
  test_create_detect();
  test_modify_detect();
  test_delete_detect();
  test_recursive_watch();
  test_auto_watch_new_subdir();
  test_hidden_dirs_skipped();
  test_recreate_watch();
  test_nonrecursive_watch_stays_shallow();

  printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
  return g_fail > 0 ? 1 : 0;
}
