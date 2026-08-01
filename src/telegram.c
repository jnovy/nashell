/*
 * telegram.c — Native Telegram Bot bridge for nash daemon mode.
 *
 * Bridges the mailbox system to Telegram: incoming messages become tasks,
 * outbox results/questions are sent as formatted Telegram messages.
 *
 * Formatting strategy (Bot API 10.1+ sendRichMessage with RichMarkdown):
 *   - Agent markdown output sent directly — native tables, headings, code
 *   - Falls back to HTML parse_mode (md_to_html) on older Bot API servers
 *
 * Threading model:
 *   The telegram_run() function runs in its own pthread, started by main.c.
 *   It alternates between short getUpdates polls and inotify-based outbox
 *   watching, using poll() to multiplex both file descriptors.
 */

#include "telegram.h"
#include "md_html.h"
#include "mailbox.h"
#include "nash_limits.h"
#include "str.h"
#include "cJSON.h"
#include "toml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <dirent.h>
#include <poll.h>
#include <curl/curl.h>
#include <pthread.h>

/* Telegram API limits */
#define TG_MSG_MAX      4096    /* max chars per message */
#define TG_API_BASE     "https://api.telegram.org/bot"
#define TG_POLL_TIMEOUT 30      /* long-poll timeout for getUpdates (seconds) */
#define TG_RETRY_DELAY  5       /* seconds to wait after API error */
#define TG_URL_MAX      512

/* ── Forward declarations ────────────────────────────────── */

static int  tg_api_get_me(telegram_ctx_t *ctx, char *bot_name, size_t name_sz);
static int  tg_api_get_updates(telegram_ctx_t *ctx, cJSON **out);
static long long tg_api_send_message(telegram_ctx_t *ctx, const char *text,
                                     const char *parse_mode, long long thread_id);
static long long tg_api_send_message_reply(telegram_ctx_t *ctx, const char *text,
                                           const char *parse_mode,
                                           long long thread_id,
                                           long long reply_to_message_id);
static long long tg_send_long(telegram_ctx_t *ctx, const char *text,
                              const char *parse_mode, long long thread_id);
static int  tg_api_send_rich(telegram_ctx_t *ctx, const char *md_text,
                             long long thread_id);
static int  tg_send_rich_long(telegram_ctx_t *ctx, const char *md_text,
                              long long thread_id);
static void tg_process_outbox_file(telegram_ctx_t *ctx, const char *filename);
static int  tg_config_save(telegram_ctx_t *ctx);
static int  tg_config_load(telegram_ctx_t *ctx);
static void tg_write_task(const char *mailbox_dir, const char *task_id,
                          const char *text, const char *workspace,
                          long long thread_id);
static void tg_write_answer(const char *mailbox_dir, const char *ask_id,
                            const char *text);
static int  tg_download_photo(telegram_ctx_t *ctx, const char *file_id,
                              char *out_path, size_t out_sz);
static int  tg_is_reply_to_bot(cJSON *msg);
static void tg_write_cmd_new(const char *mailbox_dir);

/* ── Route map helpers ───────────────────────────────────── */

static void tg_route_map_add(telegram_ctx_t *ctx, const char *task_id,
                             long long thread_id) {
    int idx = ctx->route_map_next;
    snprintf(ctx->route_map[idx].task_id,
             sizeof(ctx->route_map[idx].task_id), "%s", task_id);
    ctx->route_map[idx].thread_id = thread_id;
    ctx->route_map_next = (idx + 1) % TG_MAX_ROUTE_MAP;
}

/* ── Session thread tracking ─────────────────────────────── */

/* Store the thread root message_id for a topic */
static void tg_session_thread_set(telegram_ctx_t *ctx, long long thread_id,
                                   long long root_message_id) {
    for (int i = 0; i < ctx->session_thread_count; i++) {
        if (ctx->session_threads[i].thread_id == thread_id) {
            ctx->session_threads[i].root_message_id = root_message_id;
            return;
        }
    }
    if (ctx->session_thread_count < TG_MAX_TOPIC_MAP) {
        int idx = ctx->session_thread_count++;
        ctx->session_threads[idx].thread_id = thread_id;
        ctx->session_threads[idx].root_message_id = root_message_id;
    }
}

/* Get the thread root message_id for a topic (0 if none) */
static long long tg_session_thread_get(telegram_ctx_t *ctx, long long thread_id) {
    for (int i = 0; i < ctx->session_thread_count; i++) {
        if (ctx->session_threads[i].thread_id == thread_id)
            return ctx->session_threads[i].root_message_id;
    }
    return 0;
}

/* Clear the session thread for a specific topic */
static void tg_session_thread_clear(telegram_ctx_t *ctx, long long thread_id) {
    for (int i = 0; i < ctx->session_thread_count; i++) {
        if (ctx->session_threads[i].thread_id == thread_id) {
            for (int j = i; j < ctx->session_thread_count - 1; j++)
                ctx->session_threads[j] = ctx->session_threads[j + 1];
            ctx->session_thread_count--;
            return;
        }
    }
}

/* Clear all session threads */
static void tg_session_thread_clear_all(telegram_ctx_t *ctx) {
    ctx->session_thread_count = 0;
}

/* Look up workspace name for a given thread_id from topic_map config */
static const char *tg_workspace_for_thread(telegram_ctx_t *ctx,
                                           long long thread_id) {
    if (thread_id == 0) return NULL;  /* general topic = global workspace */
    for (int i = 0; i < ctx->topic_map_count; i++) {
        if (ctx->topic_map[i].thread_id == thread_id)
            return ctx->topic_map[i].workspace;
    }
    return NULL;  /* unmapped topic = global workspace */
}

/* Dynamically add a thread_id -> workspace mapping (auto-discovery).
 * Returns the stored workspace name, or NULL if map is full. */
static const char *tg_topic_map_add(telegram_ctx_t *ctx,
                                    long long thread_id,
                                    const char *workspace) {
    /* Check for duplicates */
    for (int i = 0; i < ctx->topic_map_count; i++) {
        if (ctx->topic_map[i].thread_id == thread_id)
            return ctx->topic_map[i].workspace;
    }
    if (ctx->topic_map_count >= TG_MAX_TOPIC_MAP) {
        fprintf(stderr, "[telegram] topic_map full (%d), cannot auto-map "
                "thread %lld\n", TG_MAX_TOPIC_MAP, thread_id);
        return NULL;
    }
    int idx = ctx->topic_map_count++;
    ctx->topic_map[idx].thread_id = thread_id;
    ctx->topic_map[idx].workspace = strdup(workspace);
    fprintf(stderr, "[telegram] auto-discovered: thread %lld -> workspace '%s'\n",
            thread_id, workspace);
    return ctx->topic_map[idx].workspace;
}

/* Create a forum topic in the configured Telegram group.
 * Returns the message_thread_id of the new topic, or -1 on failure.
 * Also auto-maps the new topic to the given workspace name. */
static long long tg_api_create_forum_topic(telegram_ctx_t *ctx,
                                           const char *name) {
    char url[TG_URL_MAX];
    snprintf(url, sizeof(url), "%s%s/createForumTopic",
             TG_API_BASE, ctx->bot_token);

    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "chat_id", (double)ctx->chat_id);
    cJSON_AddStringToObject(body, "name", name);

    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    str_t resp = str_new(1024);
    int rc = http_post(url, body_str, headers, 30, &resp);
    long long thread_id = -1;

    if (rc == 0 && resp.len > 0) {
        cJSON *rjson = cJSON_Parse(resp.data);
        if (rjson) {
            cJSON *ok = cJSON_GetObjectItem(rjson, "ok");
            if (ok && cJSON_IsTrue(ok)) {
                cJSON *result = cJSON_GetObjectItem(rjson, "result");
                if (result) {
                    cJSON *tid = cJSON_GetObjectItem(result,
                                                     "message_thread_id");
                    if (tid && cJSON_IsNumber(tid))
                        thread_id = (long long)tid->valuedouble;
                }
            } else {
                cJSON *desc = cJSON_GetObjectItem(rjson, "description");
                fprintf(stderr, "[telegram] createForumTopic '%s' failed: %s\n",
                        name, desc ? desc->valuestring : "unknown");
            }
            cJSON_Delete(rjson);
        }
    }

    str_free(&resp);
    curl_slist_free_all(headers);
    free(body_str);
    return thread_id;
}

/* Scan ~/.nash/workspaces/ and create Telegram forum topics for any
 * workspace that doesn't already have a topic mapping.
 * Sends a welcome message to each newly created topic. */
static void tg_sync_workspaces(telegram_ctx_t *ctx) {
    if (!ctx->nash_dir || !ctx->bot_token || !ctx->chat_id) return;

    char ws_dir[512];
    snprintf(ws_dir, sizeof(ws_dir), "%s/workspaces", ctx->nash_dir);

    DIR *d = opendir(ws_dir);
    if (!d) return;  /* no workspaces dir yet */

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (ent->d_type != DT_DIR && ent->d_type != DT_UNKNOWN) continue;

        /* For DT_UNKNOWN, stat to confirm directory */
        if (ent->d_type == DT_UNKNOWN) {
            char full[1024];
            snprintf(full, sizeof(full), "%s/%s", ws_dir, ent->d_name);
            struct stat st;
            if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        }

        const char *ws_name = ent->d_name;

        /* Check if this workspace already has a topic mapping */
        int found = 0;
        for (int i = 0; i < ctx->topic_map_count; i++) {
            if (strcmp(ctx->topic_map[i].workspace, ws_name) == 0) {
                found = 1;
                break;
            }
        }
        if (found) continue;

        /* Check capacity */
        if (ctx->topic_map_count >= TG_MAX_TOPIC_MAP) {
            fprintf(stderr, "[telegram] topic_map full, cannot sync "
                    "workspace '%s'\n", ws_name);
            break;
        }

        /* Create a forum topic for this workspace */
        fprintf(stderr, "[telegram] creating topic for workspace '%s'\n",
                ws_name);
        long long tid = tg_api_create_forum_topic(ctx, ws_name);
        if (tid < 0) {
            fprintf(stderr, "[telegram] failed to create topic for '%s'\n",
                    ws_name);
            continue;
        }

        /* Add to topic map */
        tg_topic_map_add(ctx, tid, ws_name);

        /* Send a welcome message to the new topic */
        char welcome[256];
        snprintf(welcome, sizeof(welcome),
                 "\xf0\x9f\x93\x82 Workspace **%s** linked to this topic.",
                 ws_name);
        tg_api_send_message(ctx, welcome, NULL, tid);
    }
    closedir(d);
}


/* ── Initialization ──────────────────────────────────────── */

int telegram_init(telegram_ctx_t *ctx, const char *config_path,
                  const char *nash_dir, const char *mailbox_dir,
                  volatile sig_atomic_t *shutdown) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->config_path = strdup(config_path);
    ctx->nash_dir = strdup(nash_dir);
    ctx->mailbox_dir = strdup(mailbox_dir);
    ctx->shutdown = shutdown;
    ctx->update_offset = 0;
    ctx->rich_supported = 1;  /* optimistic; downgraded on first 404 */

    /* Try loading existing config */
    tg_config_load(ctx);
    return 0;
}

void telegram_free(telegram_ctx_t *ctx) {
    free(ctx->bot_token);
    free(ctx->nash_dir);
    free(ctx->mailbox_dir);
    free(ctx->config_path);
    for (int i = 0; i < ctx->topic_map_count; i++)
        free(ctx->topic_map[i].workspace);
    memset(ctx, 0, sizeof(*ctx));
}


/* ── Config load/save ────────────────────────────────────── */

/* Parse a simple key=value config file (e.g. ~/.nash/telegram.conf).
 * Lines: "key = value" or "key=value", # comments, blank lines ignored.
 * String values may or may not be quoted. */
static int tg_config_load_simple(telegram_ctx_t *ctx, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        /* Strip newline */
        line[strcspn(line, "\r\n")] = 0;
        /* Skip comments and blank lines */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\0' || *p == '[') continue;

        /* Split on '=' */
        char *eq = strchr(p, '=');
        if (!eq) continue;

        /* Extract key (trim trailing spaces) */
        char *kend = eq - 1;
        while (kend > p && (*kend == ' ' || *kend == '\t')) kend--;
        size_t klen = (size_t)(kend - p + 1);

        /* Extract value (trim leading spaces, strip quotes) */
        char *val = eq + 1;
        while (*val == ' ' || *val == '\t') val++;
        size_t vlen = strlen(val);
        /* Strip surrounding quotes if present */
        if (vlen >= 2 && ((val[0] == '"' && val[vlen-1] == '"') ||
                          (val[0] == '\'' && val[vlen-1] == '\''))) {
            val++;
            vlen -= 2;
        }
        /* Trim trailing spaces from value */
        while (vlen > 0 && (val[vlen-1] == ' ' || val[vlen-1] == '\t')) vlen--;

        if (klen == 9 && strncmp(p, "bot_token", 9) == 0) {
            free(ctx->bot_token);
            ctx->bot_token = strndup(val, vlen);
        } else if (klen == 7 && strncmp(p, "chat_id", 7) == 0) {
            char tmp[64];
            size_t cplen = vlen < sizeof(tmp)-1 ? vlen : sizeof(tmp)-1;
            memcpy(tmp, val, cplen);
            tmp[cplen] = 0;
            ctx->chat_id = atoll(tmp);
        }
    }
    fclose(f);
    return (ctx->bot_token && ctx->chat_id) ? 0 : -1;
}

static int tg_config_load(telegram_ctx_t *ctx) {
    /* Try 1: [telegram] section in config.toml */
    FILE *f = fopen(ctx->config_path, "r");
    if (f) {
        char errbuf[256];
        toml_table_t *root = toml_parse_file(f, errbuf, sizeof(errbuf));
        fclose(f);
        if (root) {
            toml_table_t *tg = toml_table_in(root, "telegram");
            if (tg) {
                toml_datum_t d;
                d = toml_string_in(tg, "bot_token");
                if (d.ok) ctx->bot_token = d.u.s;

                d = toml_int_in(tg, "chat_id");
                if (d.ok) ctx->chat_id = (long long)d.u.i;

                /* Parse [telegram.topics] — topic-to-workspace mapping */
                toml_table_t *topics = toml_table_in(tg, "topics");
                if (topics) {
                    ctx->topic_map_count = 0;
                    for (int ti = 0; ; ti++) {
                        const char *key = toml_key_in(topics, ti);
                        if (!key) break;
                        if (ctx->topic_map_count >= TG_MAX_TOPIC_MAP) break;
                        toml_datum_t td = toml_string_in(topics, key);
                        if (td.ok) {
                            int idx = ctx->topic_map_count++;
                            ctx->topic_map[idx].thread_id = atoll(key);
                            ctx->topic_map[idx].workspace = td.u.s;
                            fprintf(stderr, "[telegram] topic %lld -> workspace '%s'\n",
                                    ctx->topic_map[idx].thread_id,
                                    ctx->topic_map[idx].workspace);
                        }
                    }
                }
            }
            toml_free(root);
            if (ctx->bot_token && ctx->chat_id) return 0;
        }
    }

    /* Try 2: standalone ~/.nash/telegram.conf (key=value format) */
    char tg_conf[512];
    /* Derive directory from config_path (e.g. ~/.nash/config.toml → ~/.nash/) */
    const char *slash = strrchr(ctx->config_path, '/');
    if (slash) {
        size_t dirlen = (size_t)(slash - ctx->config_path);
        snprintf(tg_conf, sizeof(tg_conf), "%.*s/telegram.conf", (int)dirlen,
                 ctx->config_path);
    } else {
        snprintf(tg_conf, sizeof(tg_conf), "telegram.conf");
    }

    int rc = tg_config_load_simple(ctx, tg_conf);
    if (rc == 0) {
        fprintf(stderr, "[telegram] loaded config from %s\n", tg_conf);
        /* Migrate: save to config.toml [telegram] section for future use */
        tg_config_save(ctx);
        fprintf(stderr, "[telegram] migrated to %s [telegram] section\n",
                ctx->config_path);
    }
    return rc;
}

static int tg_config_save(telegram_ctx_t *ctx) {
    /* Read existing config, append [telegram] section */
    char *existing = slurp_file(ctx->config_path, NULL);

    FILE *f = fopen(ctx->config_path, "w");
    if (!f) {
        free(existing);
        return -1;
    }

    /* Write back existing content */
    if (existing) {
        /* Remove any existing [telegram] section first */
        char *tg_start = strstr(existing, "\n[telegram]");
        if (tg_start) {
            /* Find next section or EOF */
            char *next = strstr(tg_start + 1, "\n[");
            if (next) {
                /* Write before [telegram] and after next section */
                fwrite(existing, 1, (size_t)(tg_start - existing), f);
                fputs(next, f);
            } else {
                /* [telegram] is last section — truncate */
                fwrite(existing, 1, (size_t)(tg_start - existing), f);
            }
        } else {
            fputs(existing, f);
        }
        free(existing);
    }

    /* Append [telegram] section */
    fprintf(f, "\n[telegram]\nbot_token = \"%s\"\nchat_id = %lld\n",
            ctx->bot_token, ctx->chat_id);

    fclose(f);
    return 0;
}


/* ── Interactive setup ───────────────────────────────────── */

int telegram_setup(telegram_ctx_t *ctx) {
    char buf[256];

    fprintf(stderr, "\n╔══════════════════════════════════════════════╗\n");
    fprintf(stderr, "║       Nash Telegram Bot Setup                ║\n");
    fprintf(stderr, "╚══════════════════════════════════════════════╝\n\n");

    /* Step 1: Get bot token */
    fprintf(stderr, "1. Open Telegram and message @BotFather\n");
    fprintf(stderr, "2. Send /newbot and follow the prompts\n");
    fprintf(stderr, "3. Copy the bot token\n\n");
    fprintf(stderr, "Paste your bot token: ");
    fflush(stderr);

    if (!fgets(buf, sizeof(buf), stdin) || *ctx->shutdown) {
        fprintf(stderr, "\n[telegram] aborted\n");
        return -1;
    }
    /* Strip newline */
    buf[strcspn(buf, "\r\n")] = 0;
    if (!buf[0]) {
        fprintf(stderr, "[telegram] empty token\n");
        return -1;
    }
    ctx->bot_token = strdup(buf);

    /* Step 2: Validate with getMe */
    char bot_name[128] = {0};
    if (tg_api_get_me(ctx, bot_name, sizeof(bot_name)) != 0) {
        fprintf(stderr, "[telegram] ✗ Invalid token — getMe failed\n");
        free(ctx->bot_token);
        ctx->bot_token = NULL;
        return -1;
    }
    fprintf(stderr, "[telegram] ✓ Bot: @%s\n\n", bot_name);

    /* Step 3: Get chat_id */
    fprintf(stderr, "Now send any message to @%s in Telegram,\n", bot_name);
    fprintf(stderr, "then press Enter here...");
    fflush(stderr);
    if (!fgets(buf, sizeof(buf), stdin) || *ctx->shutdown) {
        fprintf(stderr, "\n[telegram] aborted\n");
        return -1;
    }

    /* Poll for updates to capture chat_id */
    fprintf(stderr, "[telegram] Looking for your message...\n");
    cJSON *updates = NULL;
    int tries = 0;
    while (tries++ < 3 && !ctx->chat_id && !*ctx->shutdown) {
        if (tg_api_get_updates(ctx, &updates) == 0 && updates) {
            int n = cJSON_GetArraySize(updates);
            for (int i = 0; i < n; i++) {
                cJSON *upd = cJSON_GetArrayItem(updates, i);
                cJSON *msg = cJSON_GetObjectItem(upd, "message");
                if (!msg) continue;
                cJSON *chat = cJSON_GetObjectItem(msg, "chat");
                if (!chat) continue;
                cJSON *cid = cJSON_GetObjectItem(chat, "id");
                if (cid) {
                    ctx->chat_id = (long long)cid->valuedouble;
                    /* Update offset past this update */
                    cJSON *uid = cJSON_GetObjectItem(upd, "update_id");
                    if (uid) ctx->update_offset = (long long)uid->valuedouble + 1;
                    break;
                }
            }
            cJSON_Delete(updates);
            updates = NULL;
        }
        if (!ctx->chat_id) sleep(2);
    }

    if (!ctx->chat_id) {
        fprintf(stderr, "[telegram] ✗ Could not detect chat_id. "
                "Did you send a message to the bot?\n");
        return -1;
    }
    fprintf(stderr, "[telegram] ✓ Chat ID: %lld\n", ctx->chat_id);

    /* Step 4: Save config */
    if (tg_config_save(ctx) == 0) {
        fprintf(stderr, "[telegram] ✓ Saved to %s\n", ctx->config_path);
    } else {
        fprintf(stderr, "[telegram] ✗ Failed to save config\n");
        return -1;
    }

    /* Step 5: Send greeting */
    tg_api_send_message(ctx, "🤖 Nash bot connected! Send me queries.", NULL, 0);
    fprintf(stderr, "[telegram] ✓ Setup complete — bot is ready\n\n");

    return 0;
}


/* ── Telegram API calls ──────────────────────────────────── */

static int tg_api_get_me(telegram_ctx_t *ctx, char *bot_name, size_t name_sz) {
    char url[TG_URL_MAX];
    snprintf(url, sizeof(url), "%s%s/getMe", TG_API_BASE, ctx->bot_token);

    str_t resp = str_new(1024);
    if (http_get(url, 10, &resp) != 0 || resp.len == 0) {
        str_free(&resp);
        return -1;
    }

    cJSON *root = cJSON_Parse(resp.data);
    str_free(&resp);
    if (!root) return -1;

    cJSON *ok = cJSON_GetObjectItem(root, "ok");
    if (!ok || !cJSON_IsTrue(ok)) {
        cJSON_Delete(root);
        return -1;
    }

    cJSON *result = cJSON_GetObjectItem(root, "result");
    if (result && bot_name) {
        cJSON *uname = cJSON_GetObjectItem(result, "username");
        if (uname && uname->valuestring)
            snprintf(bot_name, name_sz, "%s", uname->valuestring);
    }

    cJSON_Delete(root);
    return 0;
}

static int tg_api_get_updates(telegram_ctx_t *ctx, cJSON **out) {
    char url[TG_URL_MAX];
    snprintf(url, sizeof(url),
             "%s%s/getUpdates?timeout=%d&offset=%lld&allowed_updates=[\"message\"]",
             TG_API_BASE, ctx->bot_token, TG_POLL_TIMEOUT, ctx->update_offset);

    str_t resp = str_new(4096);
    /* timeout = poll timeout + extra for network */
    if (http_get(url, TG_POLL_TIMEOUT + 10, &resp) != 0) {
        str_free(&resp);
        return -1;
    }

    cJSON *root = cJSON_Parse(resp.data);
    str_free(&resp);
    if (!root) return -1;

    cJSON *ok = cJSON_GetObjectItem(root, "ok");
    if (!ok || !cJSON_IsTrue(ok)) {
        cJSON_Delete(root);
        return -1;
    }

    /* Return the "result" array; caller must cJSON_Delete the root */
    *out = cJSON_DetachItemFromObject(root, "result");
    cJSON_Delete(root);
    return *out ? 0 : -1;
}

static long long tg_api_send_raw(telegram_ctx_t *ctx, const char *text,
                                 const char *parse_mode, long long thread_id,
                                 long long reply_to_message_id) {
    char url[TG_URL_MAX];
    snprintf(url, sizeof(url), "%s%s/sendMessage",
             TG_API_BASE, ctx->bot_token);

    /* Build JSON body */
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "chat_id", (double)ctx->chat_id);
    if (thread_id != 0)
        cJSON_AddNumberToObject(body, "message_thread_id", (double)thread_id);
    if (reply_to_message_id != 0) {
        cJSON *reply_params = cJSON_CreateObject();
        cJSON_AddNumberToObject(reply_params, "message_id",
                                (double)reply_to_message_id);
        cJSON_AddItemToObject(body, "reply_parameters", reply_params);
    }
    cJSON_AddStringToObject(body, "text", text);
    if (parse_mode)
        cJSON_AddStringToObject(body, "parse_mode", parse_mode);
    /* Disable link previews to avoid clutter */
    cJSON *link_opts = cJSON_CreateObject();
    cJSON_AddBoolToObject(link_opts, "is_disabled", 1);
    cJSON_AddItemToObject(body, "link_preview_options", link_opts);

    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    str_t resp = str_new(1024);
    int rc = http_post(url, body_str, headers, 30, &resp);
    long long result_msg_id = -1;

    /* Check for API-level errors and extract message_id */
    if (rc == 0 && resp.len > 0) {
        cJSON *rjson = cJSON_Parse(resp.data);
        if (rjson) {
            cJSON *ok = cJSON_GetObjectItem(rjson, "ok");
            if (!ok || !cJSON_IsTrue(ok)) {
                cJSON *desc = cJSON_GetObjectItem(rjson, "description");
                fprintf(stderr, "[telegram] sendMessage error: %s\n",
                        desc ? desc->valuestring : "unknown");
            } else {
                /* Extract message_id from result */
                cJSON *result = cJSON_GetObjectItem(rjson, "result");
                if (result) {
                    cJSON *mid = cJSON_GetObjectItem(result, "message_id");
                    if (mid)
                        result_msg_id = (long long)mid->valuedouble;
                }
            }
            cJSON_Delete(rjson);
        }
    }

    str_free(&resp);
    curl_slist_free_all(headers);
    free(body_str);
    return result_msg_id;  /* -1 on error, message_id on success */
}

/* Send message with HTML fallback: if HTML parse fails, strip tags and retry
 * as plain text so the message is never lost.
 * Returns message_id on success, -1 on error. */
static long long tg_api_send_message(telegram_ctx_t *ctx, const char *text,
                                     const char *parse_mode, long long thread_id) {
    long long mid = tg_api_send_raw(ctx, text, parse_mode, thread_id, 0);

    /* If HTML parse failed, retry without formatting */
    if (mid == -1 && parse_mode && strcmp(parse_mode, "HTML") == 0) {
        fprintf(stderr, "[telegram] HTML parse failed, retrying as plain text\n");
        mid = tg_api_send_raw(ctx, text, NULL, thread_id, 0);
    }
    return mid;
}

/* Send message as a reply to a specific message.
 * Returns message_id on success, -1 on error. */
static long long tg_api_send_message_reply(telegram_ctx_t *ctx, const char *text,
                                           const char *parse_mode,
                                           long long thread_id,
                                           long long reply_to_message_id) {
    long long mid = tg_api_send_raw(ctx, text, parse_mode, thread_id,
                                    reply_to_message_id);

    if (mid == -1 && parse_mode && strcmp(parse_mode, "HTML") == 0) {
        fprintf(stderr, "[telegram] HTML parse failed, retrying as plain text\n");
        mid = tg_api_send_raw(ctx, text, NULL, thread_id, reply_to_message_id);
    }
    return mid;
}

/* Send a long message: if it fits in TG_MSG_MAX, send as a regular message.
 * If it's longer, split into multiple messages at paragraph/line boundaries. */
static long long tg_send_long(telegram_ctx_t *ctx, const char *text,
                              const char *parse_mode, long long thread_id) {
    size_t len = strlen(text);
    if (len <= TG_MSG_MAX) {
        return tg_api_send_message(ctx, text, parse_mode, thread_id);
    }

    /* Split long messages into multiple chunks */
    const char *pos = text;
    size_t remaining = len;
    long long first_msg_id = -1;

    while (remaining > 0) {
        size_t chunk_len;
        if (remaining <= TG_MSG_MAX) {
            chunk_len = remaining;
        } else {
            chunk_len = TG_MSG_MAX;
            /* Try to find last paragraph break (\n\n) within the chunk */
            size_t best = 0;
            for (size_t i = 0; i + 1 < chunk_len; i++) {
                if (pos[i] == '\n' && pos[i + 1] == '\n')
                    best = i + 2;  /* split after both newlines */
            }
            if (best > chunk_len / 4) {
                chunk_len = best;
            } else {
                /* No paragraph break -- try last line break */
                best = 0;
                for (size_t i = 0; i < chunk_len; i++) {
                    if (pos[i] == '\n')
                        best = i + 1;  /* split after the newline */
                }
                if (best > chunk_len / 4) {
                    chunk_len = best;
                }
                /* else: hard split at TG_MSG_MAX -- clamp to UTF-8 boundary */
                else {
                    while (chunk_len > 0 && ((unsigned char)pos[chunk_len] & 0xC0) == 0x80)
                        chunk_len--;
                    /* If all bytes were continuation bytes (malformed UTF-8),
                     * skip at least 1 byte to avoid an infinite loop. */
                    if (chunk_len == 0)
                        chunk_len = 1;
                }
            }
        }

        /* Send this chunk */
        char *chunk = malloc(chunk_len + 1);
        if (!chunk) return -1;
        memcpy(chunk, pos, chunk_len);
        chunk[chunk_len] = '\0';

        long long mid = tg_api_send_message(ctx, chunk, parse_mode, thread_id);
        free(chunk);
        if (mid == -1) break;
        if (first_msg_id == -1) first_msg_id = mid;

        pos += chunk_len;
        remaining -= chunk_len;

        /* Small delay between chunks to maintain message order */
        if (remaining > 0) usleep(300000);  /* 300ms */
    }

    return first_msg_id;
}


/* ── Rich Message support (Bot API 10.1+) ────────────────── */

/*
 * Send a message using the Bot API 10.1 sendRichMessage endpoint.
 * Sends the raw markdown directly with parse_mode "RichMarkdown".
 *
 * Returns 0 on success, -1 on error.
 * On 404 (method not found), sets ctx->rich_supported = 0 so we
 * never retry on older Bot API servers.
 */
static int tg_api_send_rich(telegram_ctx_t *ctx, const char *md_text,
                            long long thread_id) {
    char url[TG_URL_MAX];
    snprintf(url, sizeof(url), "%s%s/sendRichMessage",
             TG_API_BASE, ctx->bot_token);

    /* Build JSON body */
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "chat_id", (double)ctx->chat_id);
    if (thread_id != 0)
        cJSON_AddNumberToObject(body, "message_thread_id", (double)thread_id);
    cJSON_AddStringToObject(body, "rich_text", md_text);
    cJSON_AddStringToObject(body, "parse_mode", "RichMarkdown");
    /* Disable link previews */
    cJSON *link_opts = cJSON_CreateObject();
    cJSON_AddBoolToObject(link_opts, "is_disabled", 1);
    cJSON_AddItemToObject(body, "link_preview_options", link_opts);

    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    str_t resp = str_new(1024);
    int rc = http_post(url, body_str, headers, 30, &resp);

    if (rc == 0 && resp.len > 0) {
        cJSON *rjson = cJSON_Parse(resp.data);
        if (rjson) {
            cJSON *ok = cJSON_GetObjectItem(rjson, "ok");
            if (!ok || !cJSON_IsTrue(ok)) {
                cJSON *desc = cJSON_GetObjectItem(rjson, "description");
                cJSON *errcode = cJSON_GetObjectItem(rjson, "error_code");
                int code = errcode ? (int)errcode->valuedouble : 0;
                fprintf(stderr, "[telegram] sendRichMessage error %d: %s\n",
                        code, desc ? desc->valuestring : "unknown");

                /* 404 = method not found → Bot API server doesn't support
                 * Rich Messages (pre-10.1). Disable permanently. */
                if (code == 404 || code == 400) {
                    fprintf(stderr, "[telegram] Rich Messages not supported, "
                            "falling back to HTML\n");
                    ctx->rich_supported = 0;
                }
                rc = -1;
            } else {
                fprintf(stderr, "[telegram] sent via sendRichMessage\n");
            }
            cJSON_Delete(rjson);
        }
    } else {
        rc = -1;
    }

    str_free(&resp);
    curl_slist_free_all(headers);
    free(body_str);
    return rc;
}

/* Send a (possibly long) message via Rich Messages.
 * If the text fits in TG_MSG_MAX, sends via sendRichMessage.
 * If longer, splits into multiple Rich Messages at paragraph/line boundaries. */
static int tg_send_rich_long(telegram_ctx_t *ctx, const char *md_text,
                             long long thread_id) {
    size_t len = strlen(md_text);
    if (len <= TG_MSG_MAX) {
        return tg_api_send_rich(ctx, md_text, thread_id);
    }

    /* Split long messages into multiple chunks */
    const char *pos = md_text;
    size_t remaining = len;
    int rc = 0;

    while (remaining > 0) {
        size_t chunk_len;
        if (remaining <= TG_MSG_MAX) {
            chunk_len = remaining;
        } else {
            chunk_len = TG_MSG_MAX;
            /* Try to find last paragraph break (\n\n) within the chunk */
            size_t best = 0;
            for (size_t i = 0; i + 1 < chunk_len; i++) {
                if (pos[i] == '\n' && pos[i + 1] == '\n')
                    best = i + 2;  /* split after both newlines */
            }
            if (best > chunk_len / 4) {
                chunk_len = best;
            } else {
                /* No paragraph break — try last line break */
                best = 0;
                for (size_t i = 0; i < chunk_len; i++) {
                    if (pos[i] == '\n')
                        best = i + 1;  /* split after the newline */
                }
                if (best > chunk_len / 4) {
                    chunk_len = best;
                }
                /* else: hard split at TG_MSG_MAX -- clamp to UTF-8 boundary */
                else {
                    while (chunk_len > 0 && ((unsigned char)pos[chunk_len] & 0xC0) == 0x80)
                        chunk_len--;
                    /* If all bytes were continuation bytes (malformed UTF-8),
                     * skip at least 1 byte to avoid an infinite loop. */
                    if (chunk_len == 0)
                        chunk_len = 1;
                }
            }
        }

        /* Send this chunk */
        char *chunk = malloc(chunk_len + 1);
        if (!chunk) return -1;
        memcpy(chunk, pos, chunk_len);
        chunk[chunk_len] = '\0';

        rc = tg_api_send_rich(ctx, chunk, thread_id);
        free(chunk);
        if (rc != 0) break;

        pos += chunk_len;
        remaining -= chunk_len;

        /* Small delay between chunks to maintain message order */
        if (remaining > 0) usleep(300000);  /* 300ms */
    }

    return rc;
}


/* ── Mailbox interaction ─────────────────────────────────── */

/* Write a task file to mailbox inbox (atomic via .tmp + rename) */
static void tg_write_task(const char *mailbox_dir, const char *task_id,
                          const char *text, const char *workspace,
                          long long thread_id) {
    char tmp_path[512], final_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s/inbox/task_%s.tmp",
             mailbox_dir, task_id);
    snprintf(final_path, sizeof(final_path), "%s/inbox/task_%s",
             mailbox_dir, task_id);

    FILE *f = fopen(tmp_path, "w");
    if (!f) return;
    /* Write workspace routing metadata headers if applicable */
    if (workspace && workspace[0])
        fprintf(f, "X-Workspace: %s\n", workspace);
    if (thread_id != 0)
        fprintf(f, "X-Route-Token: %lld\n", thread_id);
    if ((workspace && workspace[0]) || thread_id != 0)
        fputs("---\n", f);
    fputs(text, f);
    fclose(f);
    rename(tmp_path, final_path);
}

/* Write an answer file to mailbox inbox (for user_ask responses) */
static void tg_write_answer(const char *mailbox_dir, const char *ask_id,
                            const char *text) {
    char tmp_path[512], final_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s/inbox/ask_%s.tmp",
             mailbox_dir, ask_id);
    snprintf(final_path, sizeof(final_path), "%s/inbox/ask_%s",
             mailbox_dir, ask_id);

    FILE *f = fopen(tmp_path, "w");
    if (!f) return;
    fputs(text, f);
    fclose(f);
    rename(tmp_path, final_path);
}

/* Write cmd_new to mailbox inbox to trigger session reset in the daemon. */
static void tg_write_cmd_new(const char *mailbox_dir) {
    char cmd_tmp[512], cmd_path[512];
    snprintf(cmd_tmp, sizeof(cmd_tmp), "%s/inbox/cmd_new.tmp", mailbox_dir);
    snprintf(cmd_path, sizeof(cmd_path), "%s/inbox/cmd_new", mailbox_dir);
    FILE *f = fopen(cmd_tmp, "w");
    if (f) {
        fputs("new_session", f);
        fclose(f);
        rename(cmd_tmp, cmd_path);
    }
}

/*
 * Check if a Telegram message is a reply to one of our bot's messages.
 * Returns 1 if the message is a reply to a message sent by a bot
 * (reply_to_message.from.is_bot == true), 0 otherwise.
 *
 * This is used for session routing: replies continue the current session,
 * while new standalone messages start a fresh session.
 */
static int tg_is_reply_to_bot(cJSON *msg) {
    cJSON *reply = cJSON_GetObjectItem(msg, "reply_to_message");
    if (!reply) return 0;

    cJSON *from = cJSON_GetObjectItem(reply, "from");
    if (!from) return 0;

    cJSON *is_bot = cJSON_GetObjectItem(from, "is_bot");
    return (is_bot && cJSON_IsTrue(is_bot)) ? 1 : 0;
}

/* Read and remove a file from the outbox. Caller frees result. */
static char *tg_read_outbox(const char *path) {
    size_t len = 0;
    char *data = slurp_file(path, &len);
    if (data) unlink(path);
    return data;
}


/* ── Photo/image download ────────────────────────────────── */

/*
 * Download a photo from Telegram by file_id.
 *
 * Steps:
 *   1. Call getFile API with file_id → get file_path
 *   2. Download from https://api.telegram.org/file/bot<token>/<file_path>
 *   3. Save to ~/.nash/images/<file_path basename>
 *
 * On success, writes the local path to out_path and returns 0.
 * On failure, returns -1.
 */
static int tg_download_photo(telegram_ctx_t *ctx, const char *file_id,
                             char *out_path, size_t out_sz) {
    /* Step 1: Call getFile to get the file_path */
    char url[TG_URL_MAX];
    snprintf(url, sizeof(url), "%s%s/getFile?file_id=%s",
             TG_API_BASE, ctx->bot_token, file_id);

    str_t resp = str_new(1024);
    if (http_get(url, 15, &resp) != 0 || resp.len == 0) {
        fprintf(stderr, "[telegram] getFile request failed\n");
        str_free(&resp);
        return -1;
    }

    cJSON *root = cJSON_Parse(resp.data);
    str_free(&resp);
    if (!root) {
        fprintf(stderr, "[telegram] getFile: invalid JSON response\n");
        return -1;
    }

    cJSON *ok = cJSON_GetObjectItem(root, "ok");
    if (!ok || !cJSON_IsTrue(ok)) {
        cJSON *desc = cJSON_GetObjectItem(root, "description");
        fprintf(stderr, "[telegram] getFile error: %s\n",
                desc ? desc->valuestring : "unknown");
        cJSON_Delete(root);
        return -1;
    }

    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *fp = result ? cJSON_GetObjectItem(result, "file_path") : NULL;
    if (!fp || !fp->valuestring || !fp->valuestring[0]) {
        fprintf(stderr, "[telegram] getFile: no file_path in response\n");
        cJSON_Delete(root);
        return -1;
    }

    const char *file_path = fp->valuestring;

    /* Step 2: Download the file */
    char file_url[TG_URL_MAX * 2];
    snprintf(file_url, sizeof(file_url), "https://api.telegram.org/file/bot%s/%s",
             ctx->bot_token, file_path);

    str_t file_data = str_new(256 * 1024);  /* 256KB initial */
    if (http_get(file_url, 60, &file_data) != 0 || file_data.len == 0) {
        fprintf(stderr, "[telegram] failed to download file: %s\n", file_path);
        str_free(&file_data);
        cJSON_Delete(root);
        return -1;
    }

    /* Step 3: Save to ~/.nash/images/ */
    /* Derive nash_dir from mailbox_dir (e.g. ~/.nash/mailbox → ~/.nash) */
    char images_dir[512];
    const char *mbox_suffix = strstr(ctx->mailbox_dir, "/mailbox");
    if (mbox_suffix) {
        snprintf(images_dir, sizeof(images_dir), "%.*s/images",
                 (int)(mbox_suffix - ctx->mailbox_dir), ctx->mailbox_dir);
    } else {
        snprintf(images_dir, sizeof(images_dir), "%s/../images",
                 ctx->mailbox_dir);
    }
    mkdir_p(images_dir, 0755);

    /* Extract filename from file_path (e.g. "photos/file_123.jpg" → "file_123.jpg") */
    const char *basename = strrchr(file_path, '/');
    basename = basename ? basename + 1 : file_path;

    /* Add timestamp prefix to avoid collisions */
    char local_name[256];
    snprintf(local_name, sizeof(local_name), "%ld_%s", (long)time(NULL), basename);

    char local_path[768];
    snprintf(local_path, sizeof(local_path), "%s/%s", images_dir, local_name);

    /* Write file atomically */
    char tmp_path[776];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", local_path);

    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        fprintf(stderr, "[telegram] failed to create %s: %s\n",
                tmp_path, strerror(errno));
        str_free(&file_data);
        cJSON_Delete(root);
        return -1;
    }
    size_t file_sz = file_data.len;
    size_t written = fwrite(file_data.data, 1, file_sz, f);
    fclose(f);
    str_free(&file_data);
    cJSON_Delete(root);

    if (written != file_sz) {
        fprintf(stderr, "[telegram] short write to %s\n", tmp_path);
        unlink(tmp_path);
        return -1;
    }

    if (rename(tmp_path, local_path) != 0) {
        fprintf(stderr, "[telegram] rename failed: %s\n", strerror(errno));
        unlink(tmp_path);
        return -1;
    }

    fprintf(stderr, "[telegram] saved photo: %s (%.1f KB)\n",
            local_path, (double)written / 1024.0);

    snprintf(out_path, out_sz, "%s", local_path);
    return 0;
}

/*
 * Extract the best file_id from a Telegram message containing a photo or
 * image document.
 *
 * For photos: picks the largest PhotoSize (last in the array).
 * For documents with image/ mime_type: uses the document's file_id.
 *
 * Returns the file_id string (owned by cJSON, valid while msg lives),
 * or NULL if the message has no image.
 */
static const char *tg_extract_image_file_id(cJSON *msg) {
    /* Check for photo array (sent as photo) */
    cJSON *photo = cJSON_GetObjectItem(msg, "photo");
    if (photo && cJSON_IsArray(photo)) {
        int n = cJSON_GetArraySize(photo);
        if (n > 0) {
            /* Last element is the largest resolution */
            cJSON *largest = cJSON_GetArrayItem(photo, n - 1);
            cJSON *fid = largest ? cJSON_GetObjectItem(largest, "file_id") : NULL;
            if (fid && fid->valuestring)
                return fid->valuestring;
        }
    }

    /* Check for document with image mime type */
    cJSON *doc = cJSON_GetObjectItem(msg, "document");
    if (doc) {
        cJSON *mime = cJSON_GetObjectItem(doc, "mime_type");
        if (mime && mime->valuestring &&
            strncmp(mime->valuestring, "image/", 6) == 0) {
            cJSON *fid = cJSON_GetObjectItem(doc, "file_id");
            if (fid && fid->valuestring)
                return fid->valuestring;
        }
    }

    return NULL;
}


/* ── Outbox file processing ──────────────────────────────── */

static void tg_process_outbox_file(telegram_ctx_t *ctx, const char *filename) {
    char path[512];
    snprintf(path, sizeof(path), "%s/outbox/%s", ctx->mailbox_dir, filename);

    /* Skip .tmp files (in-progress writes) */
    size_t flen = strlen(filename);
    if (flen > 4 && strcmp(filename + flen - 4, ".tmp") == 0) return;

    char *content = tg_read_outbox(path);
    if (!content) return;

    /* Parse all metadata headers including threading fields */
    char *route_token = NULL, *workspace = NULL, *user_query = NULL;
    char *thread_action = NULL, *source = NULL, *agent_name = NULL;
    char *actual_content = mailbox_parse_headers_full(content, &workspace,
                                                      &route_token, &user_query,
                                                      &thread_action, &source,
                                                      &agent_name);
    long long thread_id = route_token ? atoll(route_token) : 0;

    /* Look up existing session thread for reply threading */
    long long reply_to = tg_session_thread_get(ctx, thread_id);

    if (strncmp(filename, "query_", 6) == 0) {
        /* Query notification from TUI/agent -- becomes thread root or reply.
         * Skip sending if the query text is empty (e.g. scheduled agents). */
        if (thread_action && strcmp(thread_action, "root") == 0) {
            tg_session_thread_clear(ctx, thread_id);
            if (actual_content && actual_content[0]) {
                str_t qmsg = str_new(strlen(actual_content) + 48);
                str_appendf(&qmsg, "\xf0\x9f\x92\xac Query: %s", actual_content);
                long long mid = tg_api_send_message(ctx, qmsg.data, NULL, thread_id);
                if (mid > 0) {
                    tg_session_thread_set(ctx, thread_id, mid);
                    fprintf(stderr, "[telegram] thread root set: %lld\n", mid);
                }
                str_free(&qmsg);
            }
        } else if (actual_content && actual_content[0]) {
            /* Reply in existing thread */
            str_t qmsg = str_new(strlen(actual_content) + 48);
            str_appendf(&qmsg, "\xf0\x9f\x92\xac Query: %s", actual_content);
            tg_api_send_message_reply(ctx, qmsg.data, NULL, thread_id, reply_to);
            str_free(&qmsg);
        }
    } else if (strncmp(filename, "result_", 7) == 0) {
        /* Task result -> convert any markdown tables to bullet-point lists
         * for inline display, then send as threaded reply. */
        char *display = md_has_table(actual_content)
                        ? md_tables_to_bullets(actual_content) : NULL;
        const char *text = display ? display : actual_content;

        int sent = 0;
        if (ctx->rich_supported) {
            if (tg_send_rich_long(ctx, text, thread_id) == 0) {
                sent = 1;
            }
        }
        if (!sent) {
            char *html = md_to_html(text);
            if (reply_to > 0) {
                tg_api_send_message_reply(ctx, html, "HTML", thread_id, reply_to);
            } else {
                tg_send_long(ctx, html, "HTML", thread_id);
            }
            free(html);
        }
        free(display);
    } else if (strncmp(filename, "ask_", 4) == 0) {
        /* user_ask question -> send as threaded reply */
        int ask_sent = 0;
        if (ctx->rich_supported) {
            str_t msg = str_new(strlen(actual_content) + 64);
            str_append_cstr(&msg, "\xe2\x9d\x93 ");
            str_append_cstr(&msg, actual_content);
            str_append_cstr(&msg, "\n\n_(Reply to this message to answer)_");
            if (tg_api_send_rich(ctx, msg.data, thread_id) == 0)
                ask_sent = 1;
            str_free(&msg);
        }
        if (!ask_sent) {
            str_t msg = str_new(strlen(actual_content) + 64);
            str_append_cstr(&msg, "\xe2\x9d\x93 ");
            str_append_cstr(&msg, actual_content);
            str_append_cstr(&msg, "\n\\n<i>(Reply to this message to answer)</i>");
            if (reply_to > 0) {
                tg_api_send_message_reply(ctx, msg.data, "HTML", thread_id, reply_to);
            } else {
                tg_api_send_message(ctx, msg.data, "HTML", thread_id);
            }
            str_free(&msg);
        }
        /* Record which topic this ask was sent to, so only a reply
         * from the same topic is accepted as the answer (bug #32). */
        snprintf(ctx->pending_ask_id, sizeof(ctx->pending_ask_id),
                 "%s", filename + 4);  /* skip "ask_" prefix */
        ctx->pending_ask_thread_id = thread_id;
    } else if (strncmp(filename, "status_", 7) == 0) {
        if (strncmp(actual_content, "[done]", 6) == 0) {
            free(route_token); free(workspace); free(user_query);
            free(thread_action); free(source); free(agent_name);
            free(content);
            return;
        }
        str_t msg = str_new(strlen(actual_content) + 16);
        str_append_cstr(&msg, "\xf0\x9f\x93\x8b ");
        str_append_cstr(&msg, actual_content);
        if (reply_to > 0) {
            tg_api_send_message_reply(ctx, msg.data, NULL, thread_id, reply_to);
        } else {
            tg_api_send_message(ctx, msg.data, NULL, thread_id);
        }
        str_free(&msg);
    }

    free(route_token); free(workspace); free(user_query);
    free(thread_action); free(source); free(agent_name);
    free(content);
}

/* Scan outbox for any existing files and process them */
static void tg_scan_outbox(telegram_ctx_t *ctx) {
    char outbox_path[512];
    snprintf(outbox_path, sizeof(outbox_path), "%s/outbox", ctx->mailbox_dir);

    DIR *dir = opendir(outbox_path);
    if (!dir) return;

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;
        tg_process_outbox_file(ctx, de->d_name);
    }
    closedir(dir);
}


/* ── Main bridge loop ────────────────────────────────────── */

/*
 * The telegram thread runs two activities concurrently:
 *
 * 1. getUpdates long-poll: receives messages from Telegram users.
 *    User messages become either:
 *    - task_* files (new queries) written to mailbox inbox
 *    - ask_* files (answers to user_ask questions) written to mailbox inbox
 *
 * 2. outbox inotify watch: detects when nash writes result/ask/status files.
 *    These are read, formatted, and sent via Telegram sendMessage.
 *
 * We use a simple alternating approach: short getUpdates polls (2-5 seconds)
 * interleaved with inotify checks. This avoids the complexity of multiplexing
 * curl sockets with inotify fds.
 */

void *telegram_run(void *arg) {
    telegram_ctx_t *ctx = (telegram_ctx_t *)arg;

    fprintf(stderr, "[telegram] bridge thread started\n");

    /* Set up inotify on outbox */
    char outbox_path[512];
    snprintf(outbox_path, sizeof(outbox_path), "%s/outbox", ctx->mailbox_dir);

    int ifd = inotify_init1(IN_NONBLOCK);
    int iwd = -1;
    if (ifd >= 0) {
        iwd = inotify_add_watch(ifd, outbox_path, IN_CREATE | IN_MOVED_TO);
        if (iwd < 0) {
            fprintf(stderr, "[telegram] inotify_add_watch failed: %s\n",
                    strerror(errno));
        }
    } else {
        fprintf(stderr, "[telegram] inotify_init failed: %s (will use polling)\n",
                strerror(errno));
    }

    /* Process any existing outbox files */
    tg_scan_outbox(ctx);

    /* Sync workspaces → create forum topics for unmapped workspaces */
    tg_sync_workspaces(ctx);
    int ws_sync_counter = 0;

    /* Main loop */
    while (!*ctx->shutdown) {
        /* ── Phase 1: Poll Telegram for updates (short timeout) ──── */
        /* Save the old timeout for short polls */
        long long old_offset = ctx->update_offset;
        (void)old_offset;

        cJSON *updates = NULL;
        /* Use a short poll (2s) so we can check outbox frequently */
        char url[TG_URL_MAX];
        snprintf(url, sizeof(url),
                 "%s%s/getUpdates?timeout=2&offset=%lld&allowed_updates=[\"message\"]",
                 TG_API_BASE, ctx->bot_token, ctx->update_offset);

        str_t resp = str_new(4096);
        int rc = http_get(url, 5, &resp);

        if (rc == 0 && resp.len > 0) {
            cJSON *root = cJSON_Parse(resp.data);
            if (root) {
                cJSON *ok = cJSON_GetObjectItem(root, "ok");
                if (ok && cJSON_IsTrue(ok)) {
                    updates = cJSON_DetachItemFromObject(root, "result");
                }
                cJSON_Delete(root);
            }
        }
        str_free(&resp);

        /* Process incoming messages */
        if (updates) {
            int n = cJSON_GetArraySize(updates);
            for (int i = 0; i < n; i++) {
                cJSON *upd = cJSON_GetArrayItem(updates, i);
                cJSON *uid = cJSON_GetObjectItem(upd, "update_id");
                if (uid) {
                    long long id = (long long)uid->valuedouble;
                    if (id >= ctx->update_offset)
                        ctx->update_offset = id + 1;
                }

                cJSON *msg = cJSON_GetObjectItem(upd, "message");
                if (!msg) continue;

                /* Verify chat_id matches */
                cJSON *chat = cJSON_GetObjectItem(msg, "chat");
                if (!chat) continue;
                cJSON *cid = cJSON_GetObjectItem(chat, "id");
                if (!cid || (long long)cid->valuedouble != ctx->chat_id) {
                    fprintf(stderr, "[telegram] ignoring message from "
                            "unauthorized chat %lld\n",
                            (long long)cid->valuedouble);
                    continue;
                }

                /* Extract message_thread_id for forum topic routing */
                cJSON *thread_j = cJSON_GetObjectItem(msg, "message_thread_id");
                long long msg_thread_id = (thread_j && cJSON_IsNumber(thread_j))
                                          ? (long long)thread_j->valuedouble : 0;
                const char *msg_workspace = tg_workspace_for_thread(ctx, msg_thread_id);

                /* Auto-discover topic names from service messages.
                 * forum_topic_created/edited arrive as service messages
                 * (no text/image), so we capture the name before the
                 * content check that would skip them. */
                char *auto_ws = NULL;  /* freed at end if not cached */
                cJSON *ftc = cJSON_GetObjectItem(msg, "forum_topic_created");
                if (ftc && msg_thread_id != 0) {
                    cJSON *ftc_name = cJSON_GetObjectItem(ftc, "name");
                    if (ftc_name && ftc_name->valuestring) {
                        auto_ws = sanitize_workspace_name(ftc_name->valuestring);
                        if (auto_ws) {
                            msg_workspace = tg_topic_map_add(ctx, msg_thread_id,
                                                             auto_ws);
                        }
                    }
                }
                cJSON *fte = cJSON_GetObjectItem(msg, "forum_topic_edited");
                if (fte && msg_thread_id != 0) {
                    cJSON *fte_name = cJSON_GetObjectItem(fte, "name");
                    if (fte_name && fte_name->valuestring) {
                        char *edited_ws = sanitize_workspace_name(fte_name->valuestring);
                        if (edited_ws) {
                            /* Update existing mapping if present */
                            for (int ti = 0; ti < ctx->topic_map_count; ti++) {
                                if (ctx->topic_map[ti].thread_id == msg_thread_id) {
                                    free(ctx->topic_map[ti].workspace);
                                    ctx->topic_map[ti].workspace = edited_ws;
                                    msg_workspace = edited_ws;
                                    edited_ws = NULL;
                                    fprintf(stderr, "[telegram] topic renamed: "
                                            "thread %lld -> '%s'\n",
                                            msg_thread_id,
                                            ctx->topic_map[ti].workspace);
                                    break;
                                }
                            }
                            free(edited_ws);  /* NULL if consumed above */
                        }
                    }
                }

                /* Auto-discover: unmapped non-zero thread -> fallback name */
                if (!msg_workspace && msg_thread_id != 0) {
                    char fallback[80];
                    snprintf(fallback, sizeof(fallback), "topic-%lld",
                             msg_thread_id);
                    msg_workspace = tg_topic_map_add(ctx, msg_thread_id,
                                                     fallback);
                }
                free(auto_ws);

                /* Extract text content: from text field or caption (for photos) */
                cJSON *text = cJSON_GetObjectItem(msg, "text");
                cJSON *caption_j = cJSON_GetObjectItem(msg, "caption");
                const char *msg_text = NULL;
                if (text && text->valuestring)
                    msg_text = text->valuestring;
                else if (caption_j && caption_j->valuestring)
                    msg_text = caption_j->valuestring;

                /* Check for photo or image document */
                const char *image_file_id = tg_extract_image_file_id(msg);

                /* Must have either text or an image */
                if (!msg_text && !image_file_id) continue;

                if (msg_text) {
                    fprintf(stderr, "[telegram] received: %.100s%s\n",
                            msg_text, strlen(msg_text) > 100 ? "..." : "");
                }
                if (image_file_id) {
                    fprintf(stderr, "[telegram] received photo (file_id: %.40s...)\n",
                            image_file_id);
                }

                /* Handle bot commands before routing */
                if (msg_text && msg_text[0] == '/') {
                    if (strcmp(msg_text, "/new") == 0 ||
                        strcmp(msg_text, "/clear") == 0) {
                        /* Session reset command */
                        tg_write_cmd_new(ctx->mailbox_dir);
                        tg_api_send_message(ctx,
                            "\xf0\x9f\x94\x84 Starting new session -- context cleared.",
                            NULL, msg_thread_id);
                        fprintf(stderr, "[telegram] /new command -> session reset\n");
                        continue;
                    }
                    if (strcmp(msg_text, "/help") == 0) {
                        tg_api_send_message(ctx,
                            "\xf0\x9f\xa4\x96 <b>Nash Bot Commands</b>\n\n"
                            "/new or /clear -- Start a new session (clear context)\n"
                            "/help -- Show this help\n\n"
                            "<b>Session behavior:</b>\n"
                            "* New message -> starts a fresh session\n"
                            "* Reply to a bot message -> continues that session\n"
                            "* /new or /clear -> explicitly resets the session",
                            "HTML", msg_thread_id);
                        continue;
                    }
                    /* Other /commands: strip the slash and treat as a query */
                }

                /* Route message: answer to pending ask, or new task.
                 *
                 * Session routing:
                 *   - Reply to a bot message → continue current session
                 *   - New standalone message  → start fresh session (cmd_new)
                 *   - Reply to a pending ask   → route as answer (no session change)
                 */
                int is_reply = tg_is_reply_to_bot(msg);

                if (ctx->pending_ask_id[0] &&
                    msg_thread_id == ctx->pending_ask_thread_id) {
                    /* This is an answer to a user_ask question from the correct topic */
                    const char *answer = msg_text ? msg_text : "(photo)";
                    fprintf(stderr, "[telegram] routing as answer to ask_%s\n",
                            ctx->pending_ask_id);
                    tg_write_answer(ctx->mailbox_dir, ctx->pending_ask_id, answer);
                    ctx->pending_ask_id[0] = 0;
                    tg_api_send_message(ctx, "\xe2\x9c\x93 Answer received", NULL, msg_thread_id);
                } else if (image_file_id) {
                    /* Photo/image message → download and create image task */
                    if (!is_reply) {
                        /* New standalone message → reset session first */
                        tg_write_cmd_new(ctx->mailbox_dir);
                        fprintf(stderr, "[telegram] new message → session reset\n");
                        /* Small delay so daemon processes cmd_new before task */
                        usleep(100000);  /* 100ms */
                    } else {
                        fprintf(stderr, "[telegram] reply → continuing session\n");
                    }
                    char image_path[768];
                    if (tg_download_photo(ctx, image_file_id,
                                          image_path, sizeof(image_path)) == 0) {
                        /* Build task text that tells the agent to analyze the image */
                        str_t task_text = str_new(1024);
                        if (msg_text && msg_text[0]) {
                            /* User provided a caption — use it as the question */
                            str_appendf(&task_text,
                                "Analyze this image using image_analyze tool "
                                "(path: %s): %s", image_path, msg_text);
                        } else {
                            /* No caption — use default prompt */
                            str_appendf(&task_text,
                                "Analyze this image using image_analyze tool "
                                "(path: %s). Describe what you see in detail.",
                                image_path);
                        }

                        char task_id[64];
                        struct timespec ts;
                        clock_gettime(CLOCK_REALTIME, &ts);
                        snprintf(task_id, sizeof(task_id), "tg%lx%04lx",
                                 (long)ts.tv_sec, ts.tv_nsec / 100000L);

                        fprintf(stderr, "[telegram] creating image task_%s\n",
                                task_id);
                        tg_route_map_add(ctx, task_id, msg_thread_id);
                        tg_write_task(ctx->mailbox_dir, task_id, task_text.data,
                                      msg_workspace, msg_thread_id);
                        str_free(&task_text);
                        tg_api_send_message(ctx, "\xf0\x9f\x93\xb7 Analyzing image...",
                                            NULL, msg_thread_id);
                    } else {
                        tg_api_send_message(ctx,
                            "\xe2\x9a\xa0\xef\xb8\x8f Failed to download image. "
                            "Note: Telegram limits bot file downloads to 20 MB.",
                            NULL, msg_thread_id);
                    }
                } else {
                    /* Plain text task query */
                    if (!is_reply) {
                        /* New standalone message -> reset session first */
                        tg_write_cmd_new(ctx->mailbox_dir);
                        tg_session_thread_clear_all(ctx);
                        fprintf(stderr, "[telegram] new message -> session reset\n");
                        /* Small delay so daemon processes cmd_new before task */
                        usleep(100000);  /* 100ms */
                    } else {
                        fprintf(stderr, "[telegram] reply -> continuing session\n");
                    }
                    char task_id[64];
                    struct timespec ts;
                    clock_gettime(CLOCK_REALTIME, &ts);
                    snprintf(task_id, sizeof(task_id), "tg%lx%04lx",
                             (long)ts.tv_sec, ts.tv_nsec / 100000L);

                    fprintf(stderr, "[telegram] creating task_%s\n", task_id);
                    tg_route_map_add(ctx, task_id, msg_thread_id);
                    tg_write_task(ctx->mailbox_dir, task_id, msg_text,
                                  msg_workspace, msg_thread_id);

                    /* Send ack -- for new sessions this becomes the thread root */
                    {
                        long long reply_root = tg_session_thread_get(ctx, msg_thread_id);
                        long long ack_mid;
                        if (is_reply && reply_root > 0) {
                            ack_mid = tg_api_send_message_reply(ctx,
                                "\xe2\x8f\xb3 Continuing...",
                                NULL, msg_thread_id, reply_root);
                        } else {
                            ack_mid = tg_api_send_message(ctx,
                                "\xe2\x8f\xb3 Processing...",
                                NULL, msg_thread_id);
                        }
                        if (ack_mid > 0 && !is_reply) {
                            tg_session_thread_set(ctx, msg_thread_id, ack_mid);
                            fprintf(stderr, "[telegram] thread root set: %lld\n",
                                    ack_mid);
                        }
                    }
                }
            }
            cJSON_Delete(updates);
        }

        if (*ctx->shutdown) break;

        /* ── Phase 2: Check outbox for results/questions ─────────── */
        if (ifd >= 0) {
            /* Read inotify events (non-blocking) */
            char evbuf[NASH_PATH_MAX]
                __attribute__((aligned(__alignof__(struct inotify_event))));
            ssize_t nread = read(ifd, evbuf, sizeof(evbuf));
            if (nread > 0) {
                /* Small delay to let atomic writes complete */
                usleep(50000);  /* 50ms */
                char *ptr = evbuf;
                while (ptr < evbuf + nread) {
                    struct inotify_event *ev = (struct inotify_event *)ptr;
                    if (ev->len > 0 && ev->name[0] != '.') {
                        /* tg_process_outbox_file handles ask_* detection
                         * and sets ctx->pending_ask_id with the correct
                         * thread_id for per-topic routing. */
                        tg_process_outbox_file(ctx, ev->name);
                    }
                    ptr += sizeof(struct inotify_event) + ev->len;
                }
            }
        } else {
            /* Fallback: poll-based outbox scan */
            tg_scan_outbox(ctx);
        }

        /* Periodically re-sync workspaces (every ~30 iterations ≈ 60s) */
        if (++ws_sync_counter >= 30) {
            tg_sync_workspaces(ctx);
            ws_sync_counter = 0;
        }
    }

    /* Cleanup */
    if (iwd >= 0) inotify_rm_watch(ifd, iwd);
    if (ifd >= 0) close(ifd);

    fprintf(stderr, "[telegram] bridge thread stopped\n");
    return NULL;
}
