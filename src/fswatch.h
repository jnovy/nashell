/*
 * fswatch.h - Portable file system change notification.
 *
 * Thin abstraction over platform-specific APIs:
 *   Linux:  inotify   (fswatch_linux.c)
 *   Other:  no-op     (fswatch_noop.c)
 *
 * Usage:
 *   fswatch_t *w = fswatch_init(my_callback, userdata);
 *   fswatch_add(w, "/path/to/dir", 1);  // recursive=1
 *   int fd = fswatch_fd(w);             // pollable fd (-1 if unavailable)
 *   // in event loop:
 *   fswatch_drain(w);                   // invoke callback for pending events
 *   fswatch_free(w);
 */
#ifndef FSWATCH_H
#define FSWATCH_H

/* Event types (bitmask) */
#define FSW_MODIFY  1
#define FSW_CREATE  2
#define FSW_DELETE  4
#define FSW_RENAME  8

/* Opaque handle */
typedef struct fswatch fswatch_t;

/* Callback: invoked once per detected change.
 *   path     - absolute path of the changed file/directory
 *   event    - bitmask of FSW_* flags
 *   userdata - pointer passed to fswatch_init() */
typedef void (*fswatch_cb)(const char *path, int event, void *userdata);

/* Create a new watcher. Returns NULL on failure.
 *   cb       - callback invoked on file changes
 *   userdata - opaque pointer forwarded to cb */
fswatch_t *fswatch_init(fswatch_cb cb, void *userdata);

/* Add a path to watch.
 *   path      - directory (or file) to watch
 *   recursive - if non-zero, also watch all subdirectories
 * Returns 0 on success, -1 on error. */
int fswatch_add(fswatch_t *w, const char *path, int recursive);

/* Return a pollable file descriptor for use with poll()/select().
 * Returns -1 if the backend has no pollable fd (e.g. no-op backend). */
int fswatch_fd(fswatch_t *w);

/* Process pending events and invoke the callback for each.
 * Returns the number of events processed, or -1 on error. */
int fswatch_drain(fswatch_t *w);

/* Destroy the watcher and release all resources. */
void fswatch_free(fswatch_t *w);

#endif /* FSWATCH_H */
