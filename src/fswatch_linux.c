/*
 * fswatch_linux.c - inotify-based file watching backend for Linux.
 *
 * Watches directories recursively using per-directory inotify watches.
 * New subdirectories created after fswatch_add() are automatically watched
 * (IN_CREATE triggers a recursive add).
 *
 * The inotify fd is pollable - integrate with poll()/select() in the
 * event loop for efficient wake-up.
 */
#ifdef __linux__

#include "fswatch.h"
#include <sys/inotify.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Map inotify watch descriptor -> directory path */
typedef struct wd_entry {
  int wd;
  char *dir;
  struct wd_entry *next;
} wd_entry_t;

struct fswatch {
  int ifd;            /* inotify file descriptor */
  fswatch_cb cb;      /* user callback */
  void *userdata;     /* callback userdata */
  wd_entry_t *wds;    /* linked list of wd -> path mappings */
  int watch_count;    /* number of active watches */
  int limit_warned;   /* 1 if we already logged the watch-limit warning */
};

/* inotify event mask for directories */
#define WATCH_MASK (IN_MODIFY | IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM)

/* Hard ceiling on inotify watches.  Prevents thrashing when the workspace
 * is a large tree (e.g. $HOME with millions of files).  Each watch costs
 * roughly 1 KB of kernel memory, so 65536 watches use about 64 MB - negligible
 * on modern systems where max_user_watches is typically 500k+. */
#define WATCH_MAX_WATCHES 65536

/* Look up the directory path for a watch descriptor */
static const char *wd_lookup(fswatch_t *w, int wd) {
  for (wd_entry_t *e = w->wds; e; e = e->next)
    if (e->wd == wd)
      return e->dir;
  return NULL;
}

/* Record a wd -> dir mapping */
static void wd_add(fswatch_t *w, int wd, const char *dir) {
  /* Update existing entry if wd reused (kernel recycles) */
  for (wd_entry_t *e = w->wds; e; e = e->next) {
    if (e->wd == wd) {
      free(e->dir);
      e->dir = strdup(dir);
      return;
    }
  }
  wd_entry_t *e = malloc(sizeof(*e));
  if (!e) return;
  e->wd = wd;
  e->dir = strdup(dir);
  e->next = w->wds;
  w->wds = e;
  w->watch_count++;
}

/* Remove a wd entry (called on IN_IGNORED / dir removal) */
static void wd_remove(fswatch_t *w, int wd) {
  wd_entry_t **pp = &w->wds;
  while (*pp) {
    if ((*pp)->wd == wd) {
      wd_entry_t *victim = *pp;
      *pp = victim->next;
      free(victim->dir);
      free(victim);
      w->watch_count--;
      return;
    }
    pp = &(*pp)->next;
  }
}

/* Add an inotify watch for a single directory.
 * Returns 0 on success, -1 on error, 1 if the watch limit was reached. */
static int watch_dir(fswatch_t *w, const char *dir) {
  if (w->watch_count >= WATCH_MAX_WATCHES) {
    if (!w->limit_warned) {
      fprintf(stderr, "[fswatch] watch limit reached (%d); "
              "deeper directories will not be monitored\n",
              WATCH_MAX_WATCHES);
      w->limit_warned = 1;
    }
    return 1;
  }
  int wd = inotify_add_watch(w->ifd, dir, WATCH_MASK);
  if (wd < 0) return -1;
  wd_add(w, wd, dir);
  return 0;
}

/* Max recursion depth.  Keeps the initial scan bounded even when the
 * tree is very deep (e.g. workspace set to $HOME). */
#define WATCH_MAX_DEPTH 8

/* Recursively add watches for dir and all subdirectories.
 * Skips hidden directories (starting with '.') to avoid watching
 * .git, .cache, and other dot-prefixed trees.
 * Uses lstat() to avoid following symlinks.  Bounded by both a
 * depth limit (WATCH_MAX_DEPTH) and a total watch count ceiling
 * (WATCH_MAX_WATCHES) so that large workspaces like $HOME do not
 * cause scan thrashing or exhaust inotify resources. */
static int watch_recursive(fswatch_t *w, const char *dir, int depth) {
  if (depth > WATCH_MAX_DEPTH)
    return 0;
  if (w->watch_count >= WATCH_MAX_WATCHES)
    return 0;

  if (watch_dir(w, dir) < 0)
    return -1;

  DIR *d = opendir(dir);
  if (!d) return -1;

  struct dirent *ent;
  while ((ent = readdir(d)) != NULL) {
    /* Skip hidden directories (. / .. and dot-prefixed) */
    if (ent->d_name[0] == '.') continue;

    /* Stop scanning if we have hit the watch ceiling */
    if (w->watch_count >= WATCH_MAX_WATCHES) break;

    char path[PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
    if (n < 0 || (size_t)n >= sizeof(path)) continue;

    struct stat st;
    if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
      watch_recursive(w, path, depth + 1);
    }
  }
  closedir(d);
  return 0;
}

/* Convert inotify mask to FSW_* event flags */
static int mask_to_fsw(uint32_t mask) {
  int ev = 0;
  if (mask & IN_MODIFY)    ev |= FSW_MODIFY;
  if (mask & IN_CREATE)    ev |= FSW_CREATE;
  if (mask & IN_DELETE)    ev |= FSW_DELETE;
  if (mask & (IN_MOVED_TO | IN_MOVED_FROM)) ev |= FSW_RENAME;
  return ev;
}

fswatch_t *fswatch_init(fswatch_cb cb, void *userdata) {
  if (!cb) return NULL;

  int ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (ifd < 0) return NULL;

  fswatch_t *w = calloc(1, sizeof(*w));
  if (!w) { close(ifd); return NULL; }

  w->ifd = ifd;
  w->cb = cb;
  w->userdata = userdata;
  w->wds = NULL;
  return w;
}

int fswatch_add(fswatch_t *w, const char *path, int recursive) {
  if (!w || !path) return -1;

  /* Resolve to absolute path */
  char resolved[PATH_MAX];
  if (!realpath(path, resolved)) return -1;

  struct stat st;
  if (stat(resolved, &st) < 0) return -1;

  if (S_ISDIR(st.st_mode) && recursive)
    return watch_recursive(w, resolved, 0);
  else
    return watch_dir(w, resolved);
}

int fswatch_fd(fswatch_t *w) {
  return w ? w->ifd : -1;
}

int fswatch_drain(fswatch_t *w) {
  if (!w) return -1;

  /* Read all available inotify events.
   * Buffer sized for ~32 events with 256-byte names. */
  char buf[4096]
    __attribute__((aligned(__alignof__(struct inotify_event))));

  int count = 0;

  for (;;) {
    ssize_t len = read(w->ifd, buf, sizeof(buf));
    if (len <= 0) {
      if (len < 0 && errno != EAGAIN)
        return -1;
      break; /* no more events */
    }

    const char *ptr = buf;
    while (ptr < buf + len) {
      const struct inotify_event *ev =
        (const struct inotify_event *)ptr;

      if (ev->mask & IN_Q_OVERFLOW) {
        /* Queue overflow - events were lost.  Nothing we can do
         * except continue processing what we have. */
        ptr += sizeof(*ev) + ev->len;
        continue;
      }

      if (ev->mask & IN_IGNORED) {
        /* Watch was removed (directory deleted, unmounted, etc.) */
        wd_remove(w, ev->wd);
        ptr += sizeof(*ev) + ev->len;
        continue;
      }

      const char *dir = wd_lookup(w, ev->wd);
      if (dir && ev->len > 0) {
        char fullpath[PATH_MAX];
        int n = snprintf(fullpath, sizeof(fullpath), "%s/%s",
                         dir, ev->name);
        if (n > 0 && (size_t)n < sizeof(fullpath)) {
          int fsw_ev = mask_to_fsw(ev->mask);

          /* If a new subdirectory was created, add a recursive watch.
           * Hidden dirs are skipped; the watch count ceiling and depth
           * limit inside watch_recursive prevent runaway growth. */
          if ((ev->mask & IN_CREATE) && (ev->mask & IN_ISDIR)) {
            if (ev->name[0] != '.') {
              watch_recursive(w, fullpath, 0);
            }
          }

          w->cb(fullpath, fsw_ev, w->userdata);
          count++;
        }
      }

      ptr += sizeof(*ev) + ev->len;
    }
  }

  return count;
}

void fswatch_free(fswatch_t *w) {
  if (!w) return;

  /* Close inotify fd (automatically removes all watches) */
  if (w->ifd >= 0)
    close(w->ifd);

  /* Free wd map */
  wd_entry_t *e = w->wds;
  while (e) {
    wd_entry_t *next = e->next;
    free(e->dir);
    free(e);
    e = next;
  }

  free(w);
}

#endif /* __linux__ */
