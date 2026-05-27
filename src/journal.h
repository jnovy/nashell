#ifndef JOURNAL_H
#define JOURNAL_H

#include "cJSON.h"
#include <stddef.h>

/* Journal handle — wraps the path to journal.jsonl */
typedef struct {
    char *path;       /* full path to journal.jsonl */
    char *session_dir;
} journal_t;

journal_t *journal_new(const char *session_dir);
void       journal_free(journal_t *j);

/* Append a step entry to journal.jsonl
 * params: cJSON object of tool parameters (borrowed, not consumed)
 * ref: store/ reference path (or NULL)
 * size: raw output size in bytes
 * lines: line count of output
 * error: error string (or NULL for success)
 * tool_call_id: LLM-generated tool_call ID for API threading (or NULL)
 */
int journal_append(journal_t *j, int react_loop, int step, const char *tool,
                   cJSON *params, const char *ref,
                   size_t size, int lines, const char *error,
                   const char *tool_call_id);

/* Build a compact manifest string for context injection.
 * Caller must free returned string. */
char *journal_manifest(journal_t *j, int max_steps);

/* FIX D8: Build a manifest filtered to only show steps from `min_step` onwards
 * within the current react loop `react_loop`. Earlier steps get a compact
 * summary line instead of full entries. This prevents the model from seeing
 * detailed references to evicted context it can no longer access.
 * Caller must free returned string. */
char *journal_manifest_filtered(journal_t *j, int max_steps,
                                 int react_loop, int min_step);

/* Scan journal.jsonl and return the highest react_loop value found.
 * Returns -1 if the journal is empty or doesn't exist.
 * Used to continue react_loop numbering when reopening an existing session. */
int journal_max_react_loop(journal_t *j);

#endif
