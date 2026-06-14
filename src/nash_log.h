#ifndef NASH_LOG_H
#define NASH_LOG_H

#include "journal.h"
#include "store.h"
#include "tools.h"

/* Forward declaration to avoid circular include with ui_state.h */
struct ui_state_t_tag;

/* Initialize the logging subsystem with journal + store handles.
 * Must be called before any nash_log() calls during TUI mode. */
void nash_log_init(journal_t *journal, store_t *store);

/* Set the tool context for alias registration.
 * When set, nash_log() creates RXSX symlinks instead of passing
 * raw SHA256 hashes as journal refs (which break TUI hyperlinks). */
void nash_log_set_tools(tool_ctx_t *tools);

/* Set the current react loop + step context.
 * Called by react.c before each inference step so log entries
 * are associated with the correct step in journal.jsonl. */
void nash_log_set_context(int react_loop, int step);

/* Set the UI state pointer for TUI refresh after logging.
 * Called from main.c after ui_state_new(). Pass NULL to clear. */
void nash_log_set_ui(void *ui);

/* Log a message.
 * - If TUI is NOT active: prints to stderr (like before).
 * - If TUI IS active: stores message in .store/, appends a journal
 *   entry with tool="log", and triggers reactRx.md regeneration
 *   so the error is visible in the TUI and persisted for audit. */
void nash_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
