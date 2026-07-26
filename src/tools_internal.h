/* tools_internal.h — shared helpers for tool implementation files.
 * This is NOT a public API header; only tool_*.c files should include it. */
#ifndef TOOLS_INTERNAL_H
#define TOOLS_INTERNAL_H

#include "tools.h"
#include "nash_limits.h"
#include "str.h"
#include "nash_log.h"
#include "journal.h"

/* ── Shared helper functions (defined in tools.c) ────────────────── */

/* Inject the current step's thought into params cJSON before journal_append. */
void tools_inject_thought(tool_ctx_t *ctx, cJSON *params);

/* Journal a tool result and mark the step as journaled.
 * Wraps journal_append with the common ctx->journal/react_loop/step prefix
 * and sets ctx->journal_done = 1 so tool_execute() knows not to add a
 * fallback entry.  All tool handlers should use this instead of calling
 * journal_append directly. */
static inline void tool_journal(tool_ctx_t *ctx, const char *tool,
                                cJSON *params, const char *ref,
                                size_t size, int lines,
                                const char *error, const char *tool_call_id) {
    ctx->journal_done = 1;
    journal_append(ctx->journal, ctx->react_loop, ctx->step,
                   tool, params, ref, size, lines, error, tool_call_id,
                   ctx->start_ts);
}

/* Construct a tool_result_t from components. */
tool_result_t tools_make_result(int success, cJSON *meta, char *ref);

/* Construct an error tool_result_t with the given message. */
tool_result_t tools_make_error(const char *msg);

/* Resolve a tool path: step alias → store path, store/ prefix → session-relative.
 * Writes resolved path into resolved_buf (size NASH_PATH_MAX).
 * Returns the path to use (may be the original, resolved alias, or resolved_buf).
 * *resolved_out is set to the alias resolution (caller must free if non-NULL). */
const char *tools_resolve_path(tool_ctx_t *ctx, const char *path,
                               char *resolved_buf, char **resolved_out);

/* Unified memory key operation for pin/unpin/delete. */
typedef int (*ws_key_fn)(workspace_t *, const char *);
typedef int (*mem_key_fn)(memory_t *, const char *);
tool_result_t tools_memory_key_op(tool_ctx_t *ctx, cJSON *params,
                                  const char *tool_name, const char *err_prefix,
                                  const char *status_str, const char *harness_note,
                                  ws_key_fn ws_fn, mem_key_fn mem_fn);

/* ── Cross-file declarations (only functions called outside their own TU) ── */

/* tool_memory.c: LLM-based consolidation -- called by
 * tool_flush_deferred_consolidations() in tools.c.
 * target: the memory_t instance the new entry was stored in. */
char *tools_memory_try_consolidate(tool_ctx_t *ctx, const char *new_key,
                                   const char *new_value, memory_t *target);

/* tool_device.c: Stop the device stream and save the capture to the session
 * directory.  Called from react.c when `done` is produced.  Safe to call
 * when no device session exists (no-op). */
void tool_device_cleanup(const char *session_dir);

/* Tool handler declarations are no longer needed here -- each tool_*.c file
 * self-registers its handlers via TOOL_PLUGIN_REGISTER() constructors.
 * Only cross-file helpers (above) remain. */

#endif /* TOOLS_INTERNAL_H */
