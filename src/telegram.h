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
 * Uses HTML parse_mode for formatting (simpler escaping than MarkdownV2).
 * Messages >4096 chars are sent as document files (.md) with caption preview.
 * No new dependencies — uses existing libcurl (http_post/http_get) and cJSON.
 */

typedef struct {
    char     *bot_token;         /* Telegram bot token from @BotFather */
    long long chat_id;           /* Authorized chat ID */
    long long update_offset;     /* getUpdates offset (last_update_id + 1) */
    char     *mailbox_dir;       /* path to ~/.nash/mailbox */
    char     *config_path;       /* path to config.toml (for saving setup) */
    volatile sig_atomic_t *shutdown;  /* pointer to shutdown_requested flag */
} telegram_ctx_t;

/* Initialize telegram context from config.
 * Loads bot_token and chat_id from config.toml [telegram] section.
 * Returns 0 on success, -1 on error. */
int telegram_init(telegram_ctx_t *ctx, const char *config_path,
                  const char *mailbox_dir,
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
