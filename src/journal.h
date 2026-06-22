#ifndef JOURNAL_H
#define JOURNAL_H

#include "cJSON.h"
#include <stddef.h>
#include <pthread.h>

/* Journal handle — wraps the path to journal.jsonl */
typedef struct {
    char *path;       /* full path to journal.jsonl (NULL until created) */
    char *session_dir;/* resolved session directory (NULL until created) */
    char *nash_dir;   /* base dir for lazy session creation (NULL if not lazy) */
    int   lazy_created;/* 0=directory not yet created, 1=created */
    pthread_mutex_t mtx;  /* FIX CRIT2: thread-safe append/read */
} journal_t;

journal_t *journal_new(const char *session_dir);
/* Lazy: directory is not created until the first journal_append().
 * If the program exits without any append, no session directory exists. */
journal_t *journal_new_lazy(const char *nash_dir);
void       journal_free(journal_t *j);
/* Returns session_dir once created, NULL if not yet created */
const char *journal_session_dir(journal_t *j);

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

/* Recursively unwrap nested JSON in a thought string.
 * Returns a heap-allocated clean thought, or NULL if no unwrapping was needed.
 * Caller must free() the result. */
char *unwrap_thought(const char *thought);

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

/* ── B1 FIX: Shared compaction parameter extraction ──── */

/* Parsed compaction statistics from a journal compaction event. */
typedef struct {
    int before_msgs;
    int after_msgs;
    int before_pct;
    int after_pct;
} journal_compaction_stats_t;

/* Parse compaction stats from a cJSON params object.
 * Safely handles NULL params (all fields zeroed). */
void journal_parse_compaction_stats(cJSON *params, journal_compaction_stats_t *s);

/* ── B3 FIX: Shared structural tool skip list ────────── */

/* Returns 1 if the tool name is a structural/internal journal entry type
 * that should be skipped during manifest/RAG/session-grep rendering.
 * Structural tools: system, query, context, spec, memory_context, log, compaction.
 * Consolidates 3 near-identical skip lists that were diverging. */
int journal_is_structural_tool(const char *tool);

/* Reduced structural filter for lexical search.
 * Keeps "query" and "memory_context" searchable since they contain
 * user input and matched skills/lessons — the most useful content
 * for pattern-based search. */
int journal_is_structural_tool_search(const char *tool);

/* ── Chunk extraction for session-level RAG ─────────── */

/* Extracted semantic chunks from a journal for embedding.
 * Each chunk is a text segment (~800-1000 chars) containing agent thoughts,
 * tool outputs, and reasoning from consecutive steps. */
typedef struct {
    char **texts;       /* chunk text strings (caller must free each + array) */
    int    n_chunks;    /* number of chunks */
} journal_chunks_t;

/* Extract semantic text chunks from a journal session.
 * Parses journal.jsonl, extracts thoughts/tool outputs from each step,
 * groups consecutive steps into chunks of ~max_chars_per_chunk characters,
 * prefixed with query context.
 * Skips: system, query, context, spec, memory_context, log entries.
 * max_chunks: cap on number of chunks (0 = default 50).
 * Returns journal_chunks_t with n_chunks=0 on failure.
 * Caller must free with journal_chunks_free(). */
journal_chunks_t journal_extract_chunks(const char *session_dir,
                                        int max_chars_per_chunk,
                                        int max_chunks);

/* Free journal chunks returned by journal_extract_chunks(). */
void journal_chunks_free(journal_chunks_t *jc);

#endif
