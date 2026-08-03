/* todo_core.c - Shared todo.md operations
 *
 * Pure-data functions for todo list manipulation.
 * Used by both cmd_todo.c (TUI layer) and tool_todo.c (AI tool layer). */

#include "todo_core.h"
#include "nash_limits.h"
#include "str.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

/* Resolve the todo.md path from workspace/memory context.
 * Prefers workspace memory if active, else global, else fallback memory. */
int todo_resolve_path(workspace_t *ws, memory_t *memory,
                      char *buf, size_t sz) {
    const char *mdir = NULL;

    /* Prefer workspace memory if active, else global */
    if (ws && ws->workspace)
        mdir = memory_dir(ws->workspace);
    else if (ws && ws->global)
        mdir = memory_dir(ws->global);
    else if (memory)
        mdir = memory_dir(memory);

    if (!mdir) return -1;

    /* mdir is "{root}/memory" - strip the "/memory" suffix to get root */
    size_t mlen = strlen(mdir);
    const char *suffix = "/memory";
    size_t slen = strlen(suffix);

    if (mlen > slen && strcmp(mdir + mlen - slen, suffix) == 0) {
        /* Build "{root}/todo.md" */
        size_t rlen = mlen - slen;
        if (rlen + sizeof("/todo.md") > sz) return -1;
        memcpy(buf, mdir, rlen);
        memcpy(buf + rlen, "/todo.md", sizeof("/todo.md")); /* includes NUL */
    } else {
        /* Fallback: just put todo.md next to the memory dir */
        snprintf(buf, sz, "%s/../todo.md", mdir);
    }
    return 0;
}

/* Load all non-empty lines from todo.md into a dynamic array.
 * Returns line count (0 if file missing); *lines_out is heap-allocated.
 * Returns -1 on error. */
int todo_load(const char *path, char ***lines_out) {
    *lines_out = NULL;
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char **lines = NULL;
    int count = 0, cap = 0;
    char linebuf[4096];

    while (fgets(linebuf, sizeof(linebuf), f)) {
        /* Strip trailing newline */
        size_t len = strlen(linebuf);
        while (len > 0 && (linebuf[len-1] == '\n' || linebuf[len-1] == '\r'))
            linebuf[--len] = '\0';

        /* Skip empty lines */
        if (len == 0) continue;

        if (count >= cap) {
            cap = cap ? cap * 2 : 16;
            if (safe_realloc((void **)&lines, sizeof(char *) * (size_t)cap)) {
                todo_free_lines(lines, count);
                fclose(f);
                return -1;
            }
        }
        char *dup = xstrdup(linebuf);
        lines[count++] = dup;
    }
    fclose(f);
    *lines_out = lines;
    return count;
}

/* Write lines back to file atomically (.tmp + rename). */
int todo_save(const char *path, char **lines, int count) {
    char tmp[NASH_PATH_MAX + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *f = fopen(tmp, "w");
    if (!f) return -1;

    for (int i = 0; i < count; i++)
        fprintf(f, "%s\n", lines[i]);

    fclose(f);
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* Free a lines array. */
void todo_free_lines(char **lines, int count) {
    free_string_array(lines, count);
}

/* Append a new line to the lines array.
 * Returns new count, or -1 on alloc failure. */
int todo_add(char ***lines_ptr, int count, const char *line) {
    char **lines = *lines_ptr;
    if (safe_realloc((void **)&lines, sizeof(char *) * (size_t)(count + 1))) return -1;

    char *dup = xstrdup(line);
    lines[count] = dup;
    *lines_ptr = lines;
    return count + 1;
}

/* Mark item at 1-based index as done (flip "- [ ]" to "- [x]").
 * Returns 0 on success, -1 on error. */
int todo_mark_done(char **lines, int count, int index, const char **err_msg) {
    if (index < 1 || index > count) {
        if (err_msg) *err_msg = "index out of range";
        return -1;
    }
    char *check = strstr(lines[index - 1], "- [ ]");
    if (!check) {
        if (err_msg) *err_msg = "item is not an open checkbox";
        return -1;
    }
    check[3] = 'x';  /* "- [ ]" -> "- [x]" */
    return 0;
}

/* Remove item at 1-based index (shift remaining items down).
 * Returns new count, or -1 on error. */
int todo_remove(char **lines, int count, int index,
                char **removed_out, const char **err_msg) {
    if (index < 1 || index > count) {
        if (err_msg) *err_msg = "index out of range";
        return -1;
    }
    if (removed_out)
        *removed_out = xstrdup(lines[index - 1]);

    free(lines[index - 1]);
    for (int i = index - 1; i < count - 1; i++)
        lines[i] = lines[i + 1];

    return count - 1;
}

/* Purge all completed ("- [x]") items. Returns new (kept) count. */
int todo_purge(char **lines, int count, int *purged_out) {
    int kept = 0, purged = 0;
    for (int i = 0; i < count; i++) {
        if (strstr(lines[i], "- [x]")) {
            free(lines[i]);
            lines[i] = NULL;
            purged++;
        }
    }
    /* Compact */
    for (int i = 0; i < count; i++) {
        if (lines[i])
            lines[kept++] = lines[i];
    }
    if (purged_out) *purged_out = purged;
    return kept;
}
