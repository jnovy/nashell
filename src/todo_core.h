#ifndef TODO_CORE_H
#define TODO_CORE_H

#include <stddef.h>
#include "workspace.h"
#include "memory.h"

/* ── Shared todo.md operations ───────────────────────────────────────
 * Pure-data functions for todo list manipulation.
 * Used by both cmd_todo.c (TUI layer) and tool_todo.c (AI tool layer). */

/* Resolve the todo.md path from workspace/memory context.
 * Prefers workspace memory if active, else global, else fallback memory.
 * Returns 0 on success, -1 on failure. */
int todo_resolve_path(workspace_t *ws, memory_t *memory,
                      char *buf, size_t sz);

/* Load all non-empty lines from todo.md into a dynamic array.
 * Returns line count (0 if file missing); *lines_out is heap-allocated.
 * Returns -1 on error. Caller must call todo_free_lines(). */
int todo_load(const char *path, char ***lines_out);

/* Write lines back to file atomically (.tmp + rename).
 * Returns 0 on success, -1 on failure. */
int todo_save(const char *path, char **lines, int count);

/* Free a lines array returned by todo_load(). */
void todo_free_lines(char **lines, int count);

/* Append a new line to the lines array.
 * Grows *lines_ptr as needed. Returns new count, or -1 on alloc failure. */
int todo_add(char ***lines_ptr, int count, const char *line);

/* Mark item at 1-based index as done (flip "- [ ]" to "- [x]").
 * Returns 0 on success, -1 on error (sets *err_msg to a static string). */
int todo_mark_done(char **lines, int count, int index, const char **err_msg);

/* Remove item at 1-based index (shift remaining items down).
 * Returns new count, or -1 on error (sets *err_msg).
 * If removed_out is non-NULL, stores a strdup of the removed line (caller frees). */
int todo_remove(char **lines, int count, int index,
                char **removed_out, const char **err_msg);

/* Purge all completed ("- [x]") items. Returns new (kept) count.
 * If purged_out is non-NULL, stores the number of items removed. */
int todo_purge(char **lines, int count, int *purged_out);

#endif
