/*
 * completion.h — Tab-completion engine for slash commands.
 *
 * Provides context-aware completion for the TUI input bar when the user
 * is typing a /command.  Supports static subcommand trees and dynamic
 * completions (playbook names, agent IDs, run IDs, filesystem paths).
 */

#ifndef COMPLETION_H
#define COMPLETION_H

#include "ui_state.h"

/* ── Completion result ───────────────────────────────────── */

typedef struct {
  char **candidates;   /* array of candidate strings */
  int count;           /* number of candidates */
  char *common_prefix; /* longest common prefix of all candidates */
  int replace_start;   /* byte offset in input where replacement begins */
  int replace_len;     /* length of text being replaced */
} completion_result_t;

/* ── API ─────────────────────────────────────────────────── */

/* Compute completions for the current input buffer.
 * `buf` is the full input string (NUL-terminated), `len` is its length,
 * `cursor` is the cursor position (byte offset).
 * `nash_dir` is ~/.nash path for dynamic completions (may be NULL).
 *
 * Returns a heap-allocated result.  Caller must free with completion_result_free().
 * Returns NULL if no completions are possible. */
completion_result_t *completion_complete(const char *buf, int len, int cursor,
                                         const char *nash_dir);

/* Free a completion result. */
void completion_result_free(completion_result_t *r);

/* ── UI integration ──────────────────────────────────────── */

/* Handle a Tab keypress in the input bar.
 * `direction` is +1 for Tab (forward), -1 for Shift-Tab (backward cycling).
 * Updates ui->input_buffer, cursor_pos, input_len as needed.
 * Shows candidates in status bar when ambiguous.
 * Caller must hold ui->mtx. */
void ui_state_complete_tab(ui_state_t *ui, int direction);

/* Reset/invalidate active completion state.
 * Called whenever the input buffer changes from a non-Tab keystroke.
 * Caller must hold ui->mtx (or be in single-threaded context). */
void ui_state_completion_reset(ui_state_t *ui);

#endif /* COMPLETION_H */
