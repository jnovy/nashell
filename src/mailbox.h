#ifndef MAILBOX_H
#define MAILBOX_H

#include "react.h"
#include "react_event.h"

/*
 * File-based mailbox for headless nash communication.
 * All messages are plain text — no JSON, no ceremony.
 *
 * Directory layout:
 *   ~/.nash/mailbox/
 *     outbox/   ← agent writes questions, status, results here
 *     inbox/    ← human/bridge writes answers, tasks here
 *
 * Message types (outbox → human reads these):
 *   ask_{id}    — plain text question from user_ask
 *   status_{id} — plain text status/error notification
 *   result_{id} — plain text task result
 *
 * Message types (inbox → human writes these):
 *   ask_{id}    — plain text answer (entire file = the answer)
 *   task_{id}   — plain text query (entire file = the query)
 *
 * Answering a question is just:
 *   echo "Yes, go ahead" > ~/.nash/mailbox/inbox/ask_684abc120042
 *
 * Submitting a task is just:
 *   echo "List files in /tmp" > ~/.nash/mailbox/inbox/task_myquery
 *
 * Any external tool (bash script, Python bridge, Telegram bot, etc.)
 * can watch outbox/ and write to inbox/ to communicate with nash.
 */

/* Mailbox context — passed as userdata to the event handler */
typedef struct {
    react_ctx_t *react_ctx;     /* react context for setting user_ask_answer */
    const char  *mailbox_dir;   /* ~/.nash/mailbox */
    const char  *session_dir;   /* for reading store files (may be NULL) */
    int          timeout_sec;   /* max seconds to wait for user_ask answer (0=forever) */
} mailbox_ctx_t;

/* Initialize mailbox directories. Returns 0 on success, -1 on error. */
int mailbox_init(const char *nash_dir, char *mailbox_dir_out, size_t out_size);

/* Event handler for headless mode with mailbox support.
 * Wraps tui_on_event but intercepts USER_ASK to use the mailbox.
 * userdata must be a mailbox_ctx_t*. */
void mailbox_on_event(const react_event_t *ev, void *userdata);

/* Write a question to outbox and block until answer appears in inbox.
 * Returns answer string (caller must free), or NULL on timeout/error. */
char *mailbox_ask(const char *mailbox_dir, const char *question, int timeout_sec);

/* Write a status/notification to outbox (non-blocking). */
void mailbox_notify(const char *mailbox_dir, const char *event_type,
                    const char *message);

/* Write task result to outbox. */
void mailbox_write_result(const char *mailbox_dir, const char *task_id,
                          const char *result);

/* Wait for next task file in inbox. Returns query string (caller frees).
 * task_id_out receives the task ID (caller frees). Blocks until task appears. */
char *mailbox_wait_task(const char *mailbox_dir, char **task_id_out,
                        int timeout_sec);

/* Generate a unique message ID. Returns static buffer (not thread-safe). */
const char *mailbox_gen_id(void);

/* ── Extended mailbox protocol with workspace routing ────── */

/*
 * Task files can carry optional metadata headers for workspace routing:
 *
 *   X-Workspace: rh
 *   X-Route-Token: 12345
 *   ---
 *   actual query text
 *
 * Files without headers are treated as global workspace (backward compat).
 * The route_token is opaque — bridges use it to route replies back to the
 * correct Telegram topic / Matrix room.
 */

/* Parsed task with optional workspace routing metadata */
typedef struct {
    char *task_id;      /* from filename: task_{id} */
    char *workspace;    /* X-Workspace header value (NULL = global) */
    char *route_token;  /* X-Route-Token value (NULL = none) */
    char *query;        /* actual query text (after --- separator) */
} mailbox_task_t;

/* Free a mailbox_task_t (all fields + struct itself). */
void mailbox_task_free(mailbox_task_t *task);

/* Wait for next task, parsing metadata headers.
 * Returns NULL on timeout/error/cmd_* wakeup (same as mailbox_wait_task). */
mailbox_task_t *mailbox_wait_task_ex(const char *mailbox_dir, int timeout_sec);

/* Write task result with route_token header for bridge reply routing.
 * If route_token is NULL, writes plain result (same as mailbox_write_result).
 * workspace and user_query are optional (NULL = omitted) -- they are written
 * as X-Workspace and X-User-Query headers so the bridge can recreate rooms
 * and echo the original query. */
void mailbox_write_result_routed(const char *mailbox_dir, const char *task_id,
                                 const char *result, const char *route_token,
                                 const char *workspace,
                                 const char *user_query);

/* Parse metadata headers from task file content.
 * Modifies content in-place: returns pointer to query text (after ---).
 * Fills workspace_out, route_token_out, and user_query_out (caller must free).
 * If no headers found, returns content unchanged with NULLs. */
char *mailbox_parse_headers(char *content, char **workspace_out,
                            char **route_token_out,
                            char **user_query_out);

#endif /* MAILBOX_H */
