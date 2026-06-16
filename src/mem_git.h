#ifndef MEM_GIT_H
#define MEM_GIT_H

#include "memory.h"

/* Internal git operations for memory version control.
 * Used by memory.c — not part of the public API. */

/* Run a git command in the memory directory. Returns 0 on success. */
int memory_git_run(memory_t *m, const char *const argv[]);

/* Initialize git repo in .memory/ if not already initialized.
 * Called on first memory_store — lazy init. */
void memory_git_init(memory_t *m);

/* Stage all changes and commit with a descriptive message.
 * Appends "Stored-by: <model>" signoff when model is known.
 * In deferred mode, skips commit and increments counter. */
void memory_git_commit(memory_t *m, const char *msg);

#endif
