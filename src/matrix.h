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

/* Room-to-workspace mapping entry (from [matrix.rooms] config) */
#define MX_MAX_ROOM_MAP 32
typedef struct {
    char *room_id;               /* Matrix room ID (e.g. !xxx:localhost) */
    char *workspace;             /* workspace name */
} mx_room_map_entry_t;

/* Route map: maps task_id -> source room_id for reply routing */
#define MX_MAX_ROUTE_MAP 64
typedef struct {
    char task_id[64];            /* task ID (e.g. "mx684abc12") */
    char room_id[256];           /* source room_id */
} mx_route_entry_t;

typedef struct {
    char     *homeserver;        /* Matrix homeserver URL (e.g. http://100.118.224.104:8008) */
    char     *access_token;      /* Matrix access token */
    char     *user_id;           /* Bot's user ID (e.g. @nash:localhost) */
    char     *room_id;           /* Default room to bridge (e.g. !xxx:localhost) */
    char     *since_token;       /* /sync pagination token */
    char     *mailbox_dir;       /* path to ~/.nash/mailbox */
    char     *config_path;       /* path to config.toml (for saving setup) */
    volatile sig_atomic_t *shutdown;  /* pointer to shutdown_requested flag */
    long long txn_counter;       /* incrementing txn ID for idempotent sends */
    char     *nash_dir;          /* path to ~/.nash (for workspace scanning) */

    /* Room-to-workspace mapping (from [matrix.rooms] in config.toml) */
    mx_room_map_entry_t room_map[MX_MAX_ROOM_MAP];
    int       room_map_count;

    /* Reply routing: task_id -> source room_id (circular buffer) */
    mx_route_entry_t route_map[MX_MAX_ROUTE_MAP];
    int       route_map_next;    /* next write index (circular) */
} matrix_ctx_t;

/* Initialize matrix context from config.
 * Loads access_token, room_id, homeserver from config.toml [matrix] section.
 * Returns 0 on success, -1 on error. */
int matrix_init(matrix_ctx_t *ctx, const char *config_path,
                const char *nash_dir, const char *mailbox_dir,
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
