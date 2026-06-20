/* tools_internal.h — shared helpers for tool implementation files.
 * This is NOT a public API header; only tool_*.c files should include it. */
#ifndef TOOLS_INTERNAL_H
#define TOOLS_INTERNAL_H

#include "tools.h"
#include "tools_registry.h"
#include "nash_limits.h"
#include "str.h"
#include "nash_log.h"

/* ── Shared helper functions (defined in tools.c) ────────────────── */

/* Inject the current step's thought into params cJSON before journal_append. */
void tools_inject_thought(tool_ctx_t *ctx, cJSON *params);

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

/* ── Tool handler declarations (defined in tool_*.c files) ───────── */

/* tool_web.c */
tool_result_t tool_web_fetch(tool_ctx_t *ctx, cJSON *params);
tool_result_t tool_web_search(tool_ctx_t *ctx, cJSON *params);

/* tool_memory.c */
tool_result_t tool_memory_store(tool_ctx_t *ctx, cJSON *params);
tool_result_t tool_memory_recall(tool_ctx_t *ctx, cJSON *params);
tool_result_t tool_memory_pin(tool_ctx_t *ctx, cJSON *params);
tool_result_t tool_memory_unpin(tool_ctx_t *ctx, cJSON *params);
tool_result_t tool_memory_delete(tool_ctx_t *ctx, cJSON *params);
tool_result_t tool_memory_list(tool_ctx_t *ctx, cJSON *params);
/* LLM-based consolidation — called by tool_flush_deferred_consolidations */
char *tools_memory_try_consolidate(tool_ctx_t *ctx, const char *new_key,
                                   const char *new_value);

/* tool_file.c */
tool_result_t tool_file_read(tool_ctx_t *ctx, cJSON *params);
tool_result_t tool_file_write(tool_ctx_t *ctx, cJSON *params);
tool_result_t tool_file_edit(tool_ctx_t *ctx, cJSON *params);

/* tool_search.c */
tool_result_t tool_grep_search(tool_ctx_t *ctx, cJSON *params);
tool_result_t tool_glob_search(tool_ctx_t *ctx, cJSON *params);

/* tool_notes.c */
tool_result_t tool_notes(tool_ctx_t *ctx, cJSON *params);

/* tool_image.c */
tool_result_t tool_image_analyze(tool_ctx_t *ctx, cJSON *params);

/* tool_session.c */
tool_result_t tool_session_grep(tool_ctx_t *ctx, cJSON *params);

#endif /* TOOLS_INTERNAL_H */
