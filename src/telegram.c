/*
 * telegram.c — Native Telegram Bot bridge for nash daemon mode.
 *
 * Bridges the mailbox system to Telegram: incoming messages become tasks,
 * outbox results/questions are sent as formatted Telegram messages.
 *
 * Uses HTML parse_mode (not MarkdownV2) for simplicity:
 *   - Only need to escape <, >, &
 *   - <b>, <i>, <code>, <pre> for formatting
 *
 * Threading model:
 *   The telegram_run() function runs in its own pthread, started by main.c.
 *   It alternates between short getUpdates polls and inotify-based outbox
 *   watching, using poll() to multiplex both file descriptors.
 */

#include "telegram.h"
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
static int  tg_api_send_message(telegram_ctx_t *ctx, const char *text,
                                const char *parse_mode);
static int  tg_send_long(telegram_ctx_t *ctx, const char *text,
                         const char *parse_mode);
static int  tg_api_send_document(telegram_ctx_t *ctx, const char *text,
                                 const char *caption);
static char *md_to_html(const char *md);
static void tg_process_outbox_file(telegram_ctx_t *ctx, const char *filename);
static int  tg_config_save(telegram_ctx_t *ctx);
static int  tg_config_load(telegram_ctx_t *ctx);
static void tg_write_task(const char *mailbox_dir, const char *task_id,
                          const char *text);
static void tg_write_answer(const char *mailbox_dir, const char *ask_id,
                            const char *text);
static int  tg_download_photo(telegram_ctx_t *ctx, const char *file_id,
                              char *out_path, size_t out_sz);
static int  tg_is_reply_to_bot(cJSON *msg);
static void tg_write_cmd_new(const char *mailbox_dir);


/* ── Initialization ──────────────────────────────────────── */

int telegram_init(telegram_ctx_t *ctx, const char *config_path,
                  const char *mailbox_dir,
                  volatile sig_atomic_t *shutdown) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->config_path = strdup(config_path);
    ctx->mailbox_dir = strdup(mailbox_dir);
    ctx->shutdown = shutdown;
    ctx->update_offset = 0;

    /* Try loading existing config */
    tg_config_load(ctx);
    return 0;
}

void telegram_free(telegram_ctx_t *ctx) {
    free(ctx->bot_token);
    free(ctx->mailbox_dir);
    free(ctx->config_path);
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
    tg_api_send_message(ctx, "🤖 Nash bot connected! Send me queries.", NULL);
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

static int tg_api_send_raw(telegram_ctx_t *ctx, const char *text,
                           const char *parse_mode) {
    char url[TG_URL_MAX];
    snprintf(url, sizeof(url), "%s%s/sendMessage",
             TG_API_BASE, ctx->bot_token);

    /* Build JSON body */
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "chat_id", (double)ctx->chat_id);
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

    /* Check for API-level errors */
    if (rc == 0 && resp.len > 0) {
        cJSON *rjson = cJSON_Parse(resp.data);
        if (rjson) {
            cJSON *ok = cJSON_GetObjectItem(rjson, "ok");
            if (!ok || !cJSON_IsTrue(ok)) {
                cJSON *desc = cJSON_GetObjectItem(rjson, "description");
                fprintf(stderr, "[telegram] sendMessage error: %s\n",
                        desc ? desc->valuestring : "unknown");
                rc = -1;
            }
            cJSON_Delete(rjson);
        }
    }

    str_free(&resp);
    curl_slist_free_all(headers);
    free(body_str);
    return rc;
}

/* Send message with HTML fallback: if HTML parse fails, strip tags and retry
 * as plain text so the message is never lost */
static int tg_api_send_message(telegram_ctx_t *ctx, const char *text,
                               const char *parse_mode) {
    int rc = tg_api_send_raw(ctx, text, parse_mode);

    /* If HTML parse failed, retry without formatting */
    if (rc == -1 && parse_mode && strcmp(parse_mode, "HTML") == 0) {
        fprintf(stderr, "[telegram] HTML parse failed, retrying as plain text\n");
        rc = tg_api_send_raw(ctx, text, NULL);
    }
    return rc;
}

/* Send text as a document file (.md) via sendDocument API.
 * Used for messages that exceed TG_MSG_MAX to avoid splitting into
 * multiple chunks where the user only sees the last part. */
static int tg_api_send_document(telegram_ctx_t *ctx, const char *text,
                                const char *caption) {
    char url[TG_URL_MAX];
    snprintf(url, sizeof(url), "%s%s/sendDocument",
             TG_API_BASE, ctx->bot_token);

    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    /* Build multipart form */
    curl_mime *mime = curl_mime_init(curl);

    /* chat_id field */
    curl_mimepart *part = curl_mime_addpart(mime);
    curl_mime_name(part, "chat_id");
    char chat_id_str[32];
    snprintf(chat_id_str, sizeof(chat_id_str), "%lld", ctx->chat_id);
    curl_mime_data(part, chat_id_str, CURL_ZERO_TERMINATED);

    /* document field — send text content as "result.md" */
    part = curl_mime_addpart(mime);
    curl_mime_name(part, "document");
    curl_mime_data(part, text, CURL_ZERO_TERMINATED);
    curl_mime_filename(part, "result.md");
    curl_mime_type(part, "text/markdown");

    /* caption (short preview, max 1024 chars for Telegram) */
    if (caption && caption[0]) {
        part = curl_mime_addpart(mime);
        curl_mime_name(part, "caption");
        curl_mime_data(part, caption, CURL_ZERO_TERMINATED);
    }

    str_t resp = str_new(1024);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, str_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);

    CURLcode res = curl_easy_perform(curl);
    int rc = (res == CURLE_OK) ? 0 : -1;

    /* Check API response */
    if (rc == 0 && resp.len > 0) {
        cJSON *rjson = cJSON_Parse(resp.data);
        if (rjson) {
            cJSON *ok = cJSON_GetObjectItem(rjson, "ok");
            if (!ok || !cJSON_IsTrue(ok)) {
                cJSON *desc = cJSON_GetObjectItem(rjson, "description");
                fprintf(stderr, "[telegram] sendDocument error: %s\n",
                        desc ? desc->valuestring : "unknown");
                rc = -1;
            }
            cJSON_Delete(rjson);
        }
    }

    str_free(&resp);
    curl_mime_free(mime);
    curl_easy_cleanup(curl);
    return rc;
}

/* Send a long message, splitting at ~4096 chars on paragraph boundaries. */
/* Send a long message: if it fits in TG_MSG_MAX, send as a regular message.
 * If it's longer, send the full text as a document file (.md) so the user
 * gets one message instead of having to scroll through multiple chunks. */
static int tg_send_long(telegram_ctx_t *ctx, const char *text,
                        const char *parse_mode) {
    size_t len = strlen(text);
    if (len <= TG_MSG_MAX) {
        return tg_api_send_message(ctx, text, parse_mode);
    }

    /* For long messages, send as a document with a short caption preview.
     * This avoids the problem where multi-part messages show only the
     * last chunk and the user has to scroll up to read from the beginning. */

    /* Extract first paragraph as caption preview (max ~900 chars to stay
     * under Telegram's 1024-char caption limit with some margin) */
    const size_t caption_max = 900;
    char caption[1024];
    size_t cap_len = 0;

    /* Find end of first paragraph (double newline) or use caption_max */
    const char *para_end = strstr(text, "\n\n");
    if (para_end && (size_t)(para_end - text) <= caption_max) {
        cap_len = (size_t)(para_end - text);
    } else {
        /* Find last newline before caption_max */
        cap_len = len < caption_max ? len : caption_max;
        for (size_t i = cap_len; i > cap_len / 2; i--) {
            if (text[i] == '\n') {
                cap_len = i;
                break;
            }
        }
    }

    /* Copy caption, stripping any HTML tags for clean preview */
    size_t j = 0;
    for (size_t i = 0; i < cap_len && j < sizeof(caption) - 4; i++) {
        if (text[i] == '<') {
            /* Skip to closing > */
            while (i < cap_len && text[i] != '>') i++;
            continue;
        }
        caption[j++] = text[i];
    }
    /* Add ellipsis if truncated */
    if (cap_len < len && j + 3 < sizeof(caption)) {
        caption[j++] = '.';
        caption[j++] = '.';
        caption[j++] = '.';
    }
    caption[j] = '\0';

    return tg_api_send_document(ctx, text, caption);
}


/* ── Markdown → Telegram HTML conversion ─────────────────── */

/* Escape HTML special characters: < > & */
static void html_escape_append(str_t *out, const char *text, size_t len) {
    for (size_t i = 0; i < len; i++) {
        switch (text[i]) {
            case '<': str_append_cstr(out, "&lt;"); break;
            case '>': str_append_cstr(out, "&gt;"); break;
            case '&': str_append_cstr(out, "&amp;"); break;
            default:  str_append(out, &text[i], 1); break;
        }
    }
}

/*
 * Convert nash's markdown output to Telegram HTML.
 *
 * Supported conversions:
 *   **bold**       → <b>bold</b>
 *   _italic_       → <i>italic</i>  (word-boundary only, not file_name)
 *   `code`         → <code>code</code>
 *   ```lang\n...\n```  → <pre><code class="language-lang">...</code></pre>
 *   | table |      → bold header + plain rows  (separator lines stripped)
 *   ## Header      → <b>Header</b>
 *   - bullet       → • bullet
 *   [text](url)    → <a href="url">text</a>
 *
 * Returns heap-allocated string. Caller frees.
 */
/* Check if a character is a word boundary for italic detection */
static int is_word_boundary(char c) {
    return c == '\0' || c == ' ' || c == '\t' || c == '\n' || c == '\r'
        || c == '.' || c == ',' || c == ':' || c == ';' || c == '!'
        || c == '?' || c == ')' || c == ']' || c == '}' || c == '"'
        || c == '\'';
}

/* Check if line at position i is a table line (starts with |) */
static int is_table_line(const char *md, int i) {
    /* Skip leading whitespace */
    while (md[i] == ' ' || md[i] == '\t') i++;
    return md[i] == '|';
}

/* Check if line is a table separator (|---|---| or | --- | --- |) */
static int is_table_separator(const char *md, int i) {
    if (!is_table_line(md, i)) return 0;
    /* Must contain at least one - and no alphabetic chars */
    int has_dash = 0;
    while (md[i] && md[i] != '\n') {
        if (md[i] == '-' || md[i] == ':') has_dash = 1;
        else if ((md[i] >= 'a' && md[i] <= 'z') || (md[i] >= 'A' && md[i] <= 'Z'))
            return 0;
        i++;
    }
    return has_dash;
}

static char *md_to_html(const char *md) {
    if (!md) return strdup("");

    size_t len = strlen(md);
    str_t out = str_new(len + len / 4 + 64);

    int i = 0;

    while (md[i]) {
        /* Fenced code block: ```lang ... ``` */
        if (md[i] == '`' && md[i+1] == '`' && md[i+2] == '`') {
            i += 3;
            /* Extract optional language */
            int lang_start = i;
            while (md[i] && md[i] != '\n') i++;
            int lang_len = i - lang_start;
            if (md[i] == '\n') i++;

            /* Find closing ``` */
            const char *close = strstr(&md[i], "```");
            size_t block_len = close ? (size_t)(close - &md[i]) : strlen(&md[i]);

            if (lang_len > 0) {
                str_append_cstr(&out, "<pre><code class=\"language-");
                str_append(&out, &md[lang_start], (size_t)lang_len);
                str_append_cstr(&out, "\">");
            } else {
                str_append_cstr(&out, "<pre><code>");
            }
            /* Code block content: escape HTML but preserve whitespace */
            html_escape_append(&out, &md[i], block_len);
            str_append_cstr(&out, "</code></pre>");

            i += (int)block_len;
            if (close) i += 3;  /* skip closing ``` */
            if (md[i] == '\n') i++;  /* skip trailing newline */
            continue;
        }

        /* Line-level patterns (only at start of line or start of string) */
        if (i == 0 || md[i-1] == '\n') {

            /* Markdown table: bold header, skip separators, plain rows */
            if (is_table_line(md, i)) {
                int is_header = 1;  /* first non-separator row is header */
                while (md[i] && is_table_line(md, i)) {
                    /* Skip separator lines (|---|---|) */
                    if (is_table_separator(md, i)) {
                        while (md[i] && md[i] != '\n') i++;
                        if (md[i] == '\n') i++;
                        continue;
                    }
                    /* Emit table row — header gets <b> wrapping */
                    if (is_header)
                        str_append_cstr(&out, "<b>");
                    while (md[i] && md[i] != '\n') {
                        /* Handle **bold** inside data cells */
                        if (!is_header && md[i] == '*' && md[i+1] == '*') {
                            i += 2;
                            str_append_cstr(&out, "<b>");
                            while (md[i] && md[i] != '\n'
                                   && !(md[i] == '*' && md[i+1] == '*')) {
                                html_escape_append(&out, &md[i], 1);
                                i++;
                            }
                            str_append_cstr(&out, "</b>");
                            if (md[i] == '*' && md[i+1] == '*') i += 2;
                            continue;
                        }
                        html_escape_append(&out, &md[i], 1);
                        i++;
                    }
                    if (is_header) {
                        str_append_cstr(&out, "</b>");
                        is_header = 0;
                    }
                    if (md[i] == '\n') {
                        str_append_cstr(&out, "\n");
                        i++;
                    }
                }
                continue;
            }

            /* Headers: ## Text → <b>Text</b> */
            if (md[i] == '#') {
                int hashes = 0;
                while (md[i + hashes] == '#') hashes++;
                if (md[i + hashes] == ' ') {
                    i += hashes + 1;
                    str_append_cstr(&out, "<b>");
                    while (md[i] && md[i] != '\n') {
                        html_escape_append(&out, &md[i], 1);
                        i++;
                    }
                    str_append_cstr(&out, "</b>");
                    if (md[i] == '\n') {
                        str_append_cstr(&out, "\n");
                        i++;
                    }
                    continue;
                }
            }

            /* Bullet lists: - item → • item */
            if (md[i] == '-' && md[i+1] == ' ') {
                str_append_cstr(&out, "• ");
                i += 2;
                continue;
            }
            /* Also handle * bullets (but not ** which is bold) */
            if (md[i] == '*' && md[i+1] == ' ') {
                str_append_cstr(&out, "• ");
                i += 2;
                continue;
            }
        }

        /* Inline code: `code` */
        if (md[i] == '`' && md[i+1] != '`') {
            i++;
            str_append_cstr(&out, "<code>");
            while (md[i] && md[i] != '`' && md[i] != '\n') {
                html_escape_append(&out, &md[i], 1);
                i++;
            }
            str_append_cstr(&out, "</code>");
            if (md[i] == '`') i++;
            continue;
        }

        /* Bold: **text** */
        if (md[i] == '*' && md[i+1] == '*') {
            i += 2;
            str_append_cstr(&out, "<b>");
            while (md[i] && !(md[i] == '*' && md[i+1] == '*')) {
                html_escape_append(&out, &md[i], 1);
                i++;
            }
            str_append_cstr(&out, "</b>");
            if (md[i] == '*' && md[i+1] == '*') i += 2;
            continue;
        }

        /* Italic: _text_ — only at word boundaries to avoid mangling
         * identifiers like file_name or my_var */
        if (md[i] == '_' && md[i+1] != '_' && md[i+1] != ' '
            && md[i+1] != '\0'
            && (i == 0 || is_word_boundary(md[i-1]))) {
            /* Scan for closing _ at a word boundary */
            int j = i + 1;
            while (md[j] && md[j] != '\n') {
                if (md[j] == '_' && is_word_boundary(md[j+1])) {
                    /* Found valid closing _ */
                    i++;
                    str_append_cstr(&out, "<i>");
                    while (i < j) {
                        html_escape_append(&out, &md[i], 1);
                        i++;
                    }
                    str_append_cstr(&out, "</i>");
                    i++;  /* skip closing _ */
                    goto next_char;
                }
                j++;
            }
            /* No valid closing _ found, output literally */
            html_escape_append(&out, &md[i], 1);
            i++;
            continue;
        next_char:
            continue;
        }

        /* Links: [text](url) */
        if (md[i] == '[') {
            int start = i + 1;
            int j = start;
            while (md[j] && md[j] != ']' && md[j] != '\n') j++;
            if (md[j] == ']' && md[j+1] == '(') {
                int url_start = j + 2;
                int k = url_start;
                while (md[k] && md[k] != ')' && md[k] != '\n') k++;
                if (md[k] == ')') {
                    str_append_cstr(&out, "<a href=\"");
                    str_append(&out, &md[url_start], (size_t)(k - url_start));
                    str_append_cstr(&out, "\">");
                    html_escape_append(&out, &md[start], (size_t)(j - start));
                    str_append_cstr(&out, "</a>");
                    i = k + 1;
                    continue;
                }
            }
        }

        /* Default: escape and append */
        html_escape_append(&out, &md[i], 1);
        i++;
    }

    return str_steal(&out);
}


/* ── Mailbox interaction ─────────────────────────────────── */

/* Write a task file to mailbox inbox (atomic via .tmp + rename) */
static void tg_write_task(const char *mailbox_dir, const char *task_id,
                          const char *text) {
    char tmp_path[512], final_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s/inbox/task_%s.tmp",
             mailbox_dir, task_id);
    snprintf(final_path, sizeof(final_path), "%s/inbox/task_%s",
             mailbox_dir, task_id);

    FILE *f = fopen(tmp_path, "w");
    if (!f) return;
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

    if (strncmp(filename, "result_", 7) == 0) {
        /* Task result → convert markdown and send.
         * For long results, send the original markdown as a document file
         * so the user can read it top-to-bottom instead of scrolling. */
        size_t content_len = strlen(content);
        if (content_len > TG_MSG_MAX) {
            /* Long: send original markdown as .md document */
            tg_send_long(ctx, content, NULL);
        } else {
            /* Short: send as formatted HTML message */
            char *html = md_to_html(content);
            tg_send_long(ctx, html, "HTML");
            free(html);
        }
    } else if (strncmp(filename, "ask_", 4) == 0) {
        /* user_ask question → send and note the ask ID for reply matching */
        str_t msg = str_new(strlen(content) + 64);
        str_append_cstr(&msg, "❓ ");
        str_append_cstr(&msg, content);
        str_append_cstr(&msg, "\n\n<i>(Reply to this message to answer)</i>");
        tg_api_send_message(ctx, msg.data, "HTML");
        str_free(&msg);
    } else if (strncmp(filename, "status_", 7) == 0) {
        /* Status notification → send as-is */
        str_t msg = str_new(strlen(content) + 16);
        str_append_cstr(&msg, "📋 ");
        str_append_cstr(&msg, content);
        tg_api_send_message(ctx, msg.data, NULL);
        str_free(&msg);
    }

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

    /* Pending ask ID: when nash writes ask_*, we track it so the next
     * Telegram reply is routed as an answer rather than a new task. */
    char pending_ask_id[128] = {0};

    /* Process any existing outbox files */
    tg_scan_outbox(ctx);

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
                            "🔄 Starting new session — context cleared.", NULL);
                        fprintf(stderr, "[telegram] /new command → session reset\n");
                        continue;
                    }
                    if (strcmp(msg_text, "/help") == 0) {
                        tg_api_send_message(ctx,
                            "🤖 <b>Nash Bot Commands</b>\n\n"
                            "/new or /clear — Start a new session (clear context)\n"
                            "/help — Show this help\n\n"
                            "<b>Session behavior:</b>\n"
                            "• New message → starts a fresh session\n"
                            "• Reply to a bot message → continues that session\n"
                            "• /new or /clear → explicitly resets the session",
                            "HTML");
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

                if (pending_ask_id[0]) {
                    /* This is an answer to a user_ask question */
                    const char *answer = msg_text ? msg_text : "(photo)";
                    fprintf(stderr, "[telegram] routing as answer to ask_%s\n",
                            pending_ask_id);
                    tg_write_answer(ctx->mailbox_dir, pending_ask_id, answer);
                    pending_ask_id[0] = 0;
                    tg_api_send_message(ctx, "✓ Answer received", NULL);
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
                        tg_write_task(ctx->mailbox_dir, task_id, task_text.data);
                        str_free(&task_text);
                        tg_api_send_message(ctx, "📷 Analyzing image...", NULL);
                    } else {
                        tg_api_send_message(ctx,
                            "⚠️ Failed to download image. "
                            "Note: Telegram limits bot file downloads to 20 MB.",
                            NULL);
                    }
                } else {
                    /* Plain text task query */
                    if (!is_reply) {
                        /* New standalone message → reset session first */
                        tg_write_cmd_new(ctx->mailbox_dir);
                        fprintf(stderr, "[telegram] new message → session reset\n");
                        /* Small delay so daemon processes cmd_new before task */
                        usleep(100000);  /* 100ms */
                    } else {
                        fprintf(stderr, "[telegram] reply → continuing session\n");
                    }
                    char task_id[64];
                    struct timespec ts;
                    clock_gettime(CLOCK_REALTIME, &ts);
                    snprintf(task_id, sizeof(task_id), "tg%lx%04lx",
                             (long)ts.tv_sec, ts.tv_nsec / 100000L);

                    fprintf(stderr, "[telegram] creating task_%s\n", task_id);
                    tg_write_task(ctx->mailbox_dir, task_id, msg_text);
                    tg_api_send_message(ctx, is_reply
                        ? "⏳ Continuing..." : "⏳ Processing...", NULL);
                }
            }
            cJSON_Delete(updates);
        }

        if (*ctx->shutdown) break;

        /* ── Phase 2: Check outbox for results/questions ─────────── */
        if (ifd >= 0) {
            /* Read inotify events (non-blocking) */
            char evbuf[4096]
                __attribute__((aligned(__alignof__(struct inotify_event))));
            ssize_t nread = read(ifd, evbuf, sizeof(evbuf));
            if (nread > 0) {
                /* Small delay to let atomic writes complete */
                usleep(50000);  /* 50ms */
                char *ptr = evbuf;
                while (ptr < evbuf + nread) {
                    struct inotify_event *ev = (struct inotify_event *)ptr;
                    if (ev->len > 0 && ev->name[0] != '.') {
                        /* Check if this is an ask_* file → set pending ask */
                        if (strncmp(ev->name, "ask_", 4) == 0) {
                            /* Extract ask ID */
                            size_t nlen = strlen(ev->name);
                            if (nlen < sizeof(pending_ask_id)) {
                                snprintf(pending_ask_id, sizeof(pending_ask_id),
                                         "%s", ev->name + 4);
                            }
                        }
                        tg_process_outbox_file(ctx, ev->name);
                    }
                    ptr += sizeof(struct inotify_event) + ev->len;
                }
            }
        } else {
            /* Fallback: poll-based outbox scan */
            tg_scan_outbox(ctx);
        }
    }

    /* Cleanup */
    if (iwd >= 0) inotify_rm_watch(ifd, iwd);
    if (ifd >= 0) close(ifd);

    fprintf(stderr, "[telegram] bridge thread stopped\n");
    return NULL;
}
