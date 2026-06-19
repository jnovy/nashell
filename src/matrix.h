#ifndef MATRIX_H
#define MATRIX_H

#include <signal.h>

/*
 * Native Matrix bridge for nash daemon mode.
 *
 * Architecture:
 *   nash --matrix
 *     ├── implies --daemon (mailbox_mode + daemon_mode)
 *     ├── matrix_thread (pthread):
 *     │   ├── polls /sync → writes task_* to mailbox inbox
 *     │   ├── watches outbox (inotify) → sends results via sendMessage
 *     │   └── handles ask_* → sends question, waits for reply → writes answer
 *     └── daemon loop (main thread):
 *         └── existing mailbox_wait_task → react_run → mailbox_write_result
 *
 * Uses Matrix Client-Server API v3 over HTTP (Tailscale network).
 * Sends agent markdown output as org.matrix.custom.html formatted messages.
 * No new dependencies — uses existing libcurl (http_post/http_get) and cJSON.
 */

typedef struct {
    char     *homeserver;        /* Matrix homeserver URL (e.g. http://100.118.224.104:8008) */
    char     *access_token;      /* Matrix access token */
    char     *user_id;           /* Bot's user ID (e.g. @nash:localhost) */
    char     *room_id;           /* Room to bridge (e.g. !xxx:localhost) */
    char     *since_token;       /* /sync pagination token */
    char     *mailbox_dir;       /* path to ~/.nash/mailbox */
    char     *config_path;       /* path to config.toml (for saving setup) */
    volatile sig_atomic_t *shutdown;  /* pointer to shutdown_requested flag */
    long long txn_counter;       /* incrementing txn ID for idempotent sends */
} matrix_ctx_t;

/* Initialize matrix context from config.
 * Loads access_token, room_id, homeserver from config.toml [matrix] section.
 * Returns 0 on success, -1 on error. */
int matrix_init(matrix_ctx_t *ctx, const char *config_path,
                const char *mailbox_dir,
                volatile sig_atomic_t *shutdown);

/* Interactive first-run setup.
 * Prompts for homeserver, username, password; logs in to get access_token;
 * creates or joins a room; saves [matrix] section to config.toml.
 * Returns 0 on success, -1 on error. */
int matrix_setup(matrix_ctx_t *ctx);

/* Main matrix bridge loop (runs in a pthread).
 * arg must be a matrix_ctx_t*.
 * Concurrently: polls /sync for incoming messages,
 * watches outbox via inotify for results/questions to send. */
void *matrix_run(void *arg);

/* Free matrix context resources. */
void matrix_free(matrix_ctx_t *ctx);

#endif /* MATRIX_H */
