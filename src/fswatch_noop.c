/*
 * fswatch_noop.c - No-op file watching backend for non-Linux platforms.
 *
 * All operations succeed silently but do nothing.  This allows Nash
 * to compile and run on any platform without ifdefs in the callers.
 */
#ifndef __linux__

#include "fswatch.h"
#include <stdlib.h>

struct fswatch {
  fswatch_cb cb;
  void *userdata;
};

fswatch_t *fswatch_init(fswatch_cb cb, void *userdata) {
  if (!cb) return NULL;
  fswatch_t *w = calloc(1, sizeof(*w));
  if (!w) return NULL;
  w->cb = cb;
  w->userdata = userdata;
  return w;
}

int fswatch_add(fswatch_t *w, const char *path, int recursive) {
  (void)w; (void)path; (void)recursive;
  return 0;
}

int fswatch_fd(fswatch_t *w) {
  (void)w;
  return -1; /* no pollable fd */
}

int fswatch_drain(fswatch_t *w) {
  (void)w;
  return 0; /* no events */
}

void fswatch_free(fswatch_t *w) {
  free(w);
}

#endif /* !__linux__ */
