#ifndef TELEGRAM_H
#define TELEGRAM_H

#include <signal.h>

/*
 * Native Telegram Bot bridge for nash daemon mode.
 *
 * Architecture:
 *   nash --telegram
 *     ├── implies --daemon (mailbox_mode + daemon_mode)
 *     ├── telegram_thread (pthread):
 *     │   ├── polls getUpdates → writes task_* to mailbox inbox
 *     │   ├── watches outbox (inotify) → sends results via sendMessage
 *     │   └── handles ask_* → sends question, waits for reply → writes answer
 *     └── daemon loop (main thread):
 *         └── existing mailbox_wait_task → react_run → mailbox_write_result
 *
 * Uses Bot API 10.1 sendRichMessage with RichMarkdown parse_mode when
 * available — sends agent markdown output directly with full formatting
 * (native tables, headings, code highlighting, math).
 * Falls back to HTML parse_mode via md_to_html() on older Bot API servers.
 * Messages >4096 chars are sent as document files (.md) with caption preview.
 * No new dependencies — uses existing libcurl (http_post/http_get) and cJSON.
 */

/* Topic-to-workspace mapping entry (from [telegram.topics] config) */
#define TG_MAX_TOPIC_MAP 32
typedef struct {
    long long thread_id;         /* Telegram message_thread_id */
    char     *workspace;         /* workspace name */
} tg_topic_map_entry_t;

/* Route map: maps task_id -> thread_id for reply routing */
#define TG_MAX_ROUTE_MAP 64
typedef struct {
    char      task_id[64];       /* task ID (e.g. "tg684abc12") */
    long long thread_id;         /* source topic's thread_id (0 = general) */
} tg_route_entry_t;

typedef struct {
    char     *bot_token;         /* Telegram bot token from @BotFather */
    long long chat_id;           /* Authorized chat ID */
    long long update_offset;     /* getUpdates offset (last_update_id + 1) */
    char     *mailbox_dir;       /* path to ~/.nash/mailbox */
    char     *config_path;       /* path to config.toml (for saving setup) */
    volatile sig_atomic_t *shutdown;  /* pointer to shutdown_requested flag */
    int       rich_supported;    /* 1 = sendRichMessage available (Bot API 10.1+) */
    char     *nash_dir;          /* path to ~/.nash (for workspace scanning) */

    /* Topic-to-workspace mapping (from [telegram.topics] in config.toml) */
    tg_topic_map_entry_t topic_map[TG_MAX_TOPIC_MAP];
    int       topic_map_count;

    /* Reply routing: task_id -> thread_id (circular buffer) */
    tg_route_entry_t route_map[TG_MAX_ROUTE_MAP];
    int       route_map_next;    /* next write index (circular) */

    /* Session threading: tracks reply thread root message_id per topic */
    struct {
        long long thread_id;         /* forum topic (0 = general) */
        long long root_message_id;   /* message_id of the thread root */
    } session_threads[TG_MAX_TOPIC_MAP];
    int       session_thread_count;

    /* Pending user_ask: tracks which topic the ask was sent to so that
     * only a reply from the correct topic is routed as the answer. */
    char      pending_ask_id[128];
    long long pending_ask_thread_id;
} telegram_ctx_t;

/* Initialize telegram context from config.
 * Loads bot_token and chat_id from config.toml [telegram] section.
 * Returns 0 on success, -1 on error. */
int telegram_init(telegram_ctx_t *ctx, const char *config_path,
                  const char *nash_dir, const char *mailbox_dir,
                  volatile sig_atomic_t *shutdown);

/* Interactive first-run setup.
 * Prompts for bot token, validates with getMe, captures chat_id,
 * and saves [telegram] section to config.toml.
 * Returns 0 on success, -1 on error. */
int telegram_setup(telegram_ctx_t *ctx);

/* Main telegram bridge loop (runs in a pthread).
 * arg must be a telegram_ctx_t*.
 * Concurrently: polls getUpdates for incoming messages,
 * watches outbox via inotify for results/questions to send. */
void *telegram_run(void *arg);

/* Free telegram context resources. */
void telegram_free(telegram_ctx_t *ctx);

#endif /* TELEGRAM_H */
