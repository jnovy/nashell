/* Recursive kqueue watcher for macOS.
 *
 * kqueue only reports changes to an open vnode.  To preserve fswatch's
 * changed-file callback contract, we watch files as well as directories;
 * directory notifications discover and register newly-created children. */
#ifdef __APPLE__

#include "fswatch.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

typedef struct watch_entry {
  int fd;
  int is_dir;
  char *path;
  struct watch_entry *next;
} watch_entry_t;

struct fswatch {
  int kqfd;
  fswatch_cb cb;
  void *userdata;
  watch_entry_t *watches;
};

static watch_entry_t *find_watch(fswatch_t *w, const char *path) {
  for (watch_entry_t *e = w->watches; e; e = e->next)
    if (strcmp(e->path, path) == 0) return e;
  return NULL;
}

static int add_watch(fswatch_t *w, const char *path, int is_dir) {
  if (find_watch(w, path)) return 0;
  int fd = open(path, O_RDONLY | O_EVTONLY);
  if (fd < 0) return -1;
  struct kevent change;
  EV_SET(&change, (uintptr_t)fd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
         NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB | NOTE_DELETE | NOTE_RENAME,
         0, NULL);
  if (kevent(w->kqfd, &change, 1, NULL, 0, NULL) < 0) {
    close(fd);
    return -1;
  }
  watch_entry_t *entry = calloc(1, sizeof(*entry));
  if (!entry) { close(fd); return -1; }
  entry->path = strdup(path);
  if (!entry->path) { free(entry); close(fd); return -1; }
  entry->fd = fd;
  entry->is_dir = is_dir;
  entry->next = w->watches;
  w->watches = entry;
  /* Store the entry for O(1) event-to-path lookup. */
  EV_SET(&change, (uintptr_t)fd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
         NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB | NOTE_DELETE | NOTE_RENAME,
         0, entry);
  if (kevent(w->kqfd, &change, 1, NULL, 0, NULL) < 0) return -1;
  return 0;
}

static int watch_tree(fswatch_t *w, const char *path, int notify_new) {
  struct stat st;
  if (lstat(path, &st) != 0 || S_ISLNK(st.st_mode)) return 0;
  int is_dir = S_ISDIR(st.st_mode);
  int was_known = find_watch(w, path) != NULL;
  if (add_watch(w, path, is_dir) != 0) return 0;
  int added = 0;
  if (notify_new && !was_known)
    w->cb(path, FSW_CREATE, w->userdata), added++;
  if (!is_dir) return added;
  DIR *dir = opendir(path);
  if (!dir) return added;
  struct dirent *de;
  while ((de = readdir(dir)) != NULL) {
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
      continue;
    size_t n = strlen(path) + strlen(de->d_name) + 2;
    char *child = malloc(n);
    if (!child) continue;
    snprintf(child, n, "%s/%s", path, de->d_name);
    added += watch_tree(w, child, notify_new);
    free(child);
  }
  closedir(dir);
  return added;
}

fswatch_t *fswatch_init(fswatch_cb cb, void *userdata) {
  if (!cb) return NULL;
  int kqfd = kqueue();
  if (kqfd < 0) return NULL;
  fswatch_t *w = calloc(1, sizeof(*w));
  if (!w) { close(kqfd); return NULL; }
  w->kqfd = kqfd;
  w->cb = cb;
  w->userdata = userdata;
  return w;
}

int fswatch_add(fswatch_t *w, const char *path, int recursive) {
  if (!w || !path) return -1;
  struct stat st;
  if (lstat(path, &st) != 0) return -1;
  if (recursive && S_ISDIR(st.st_mode)) (void)watch_tree(w, path, 0);
  else if (add_watch(w, path, S_ISDIR(st.st_mode)) != 0) return -1;
  return find_watch(w, path) ? 0 : -1;
}

int fswatch_fd(fswatch_t *w) { return w ? w->kqfd : -1; }

int fswatch_drain(fswatch_t *w) {
  if (!w) return -1;
  struct timespec timeout = {0, 0};
  struct kevent events[32];
  int count = 0, n;
  while ((n = kevent(w->kqfd, NULL, 0, events, 32, &timeout)) > 0) {
    for (int i = 0; i < n; i++) {
      watch_entry_t *entry = events[i].udata;
      if (!entry) continue;
      int flags = 0;
      if (events[i].fflags & (NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB))
        flags |= FSW_MODIFY;
      if (events[i].fflags & NOTE_DELETE) flags |= FSW_DELETE;
      if (events[i].fflags & NOTE_RENAME) flags |= FSW_RENAME;
      if (!flags) continue;
      if (entry->is_dir && (flags & FSW_MODIFY)) {
        /* kqueue supplies no child name: scan only to register new paths. */
        count += watch_tree(w, entry->path, 1);
      } else {
        w->cb(entry->path, flags, w->userdata);
        count++;
      }
    }
  }
  return n < 0 && errno != EAGAIN ? -1 : count;
}

void fswatch_free(fswatch_t *w) {
  if (!w) return;
  watch_entry_t *entry = w->watches;
  while (entry) {
    watch_entry_t *next = entry->next;
    close(entry->fd);
    free(entry->path);
    free(entry);
    entry = next;
  }
  close(w->kqfd);
  free(w);
}

#endif /* __APPLE__ */
