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
static char *md_to_html(const char *md);
static void tg_process_outbox_file(telegram_ctx_t *ctx, const char *filename);
static int  tg_config_save(telegram_ctx_t *ctx);
static int  tg_config_load(telegram_ctx_t *ctx);
static void tg_write_task(const char *mailbox_dir, const char *task_id,
                          const char *text);
static void tg_write_answer(const char *mailbox_dir, const char *ask_id,
                            const char *text);


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

/* Send a long message, splitting at ~4096 chars on paragraph boundaries. */
/* Track which HTML tags are open at a given position in the text.
 * Returns a bitmask: bit 0 = <pre>, bit 1 = <code>, bit 2 = <b>, bit 3 = <i> */
#define TAG_PRE  1
#define TAG_CODE 2
#define TAG_B    4
#define TAG_I    8

static int html_open_tags(const char *text, size_t len) {
    int tags = 0;
    for (size_t i = 0; i < len; i++) {
        if (text[i] != '<') continue;
        if (i + 4 <= len && strncmp(&text[i], "<pre", 4) == 0) tags |= TAG_PRE;
        else if (i + 6 <= len && strncmp(&text[i], "</pre>", 6) == 0) tags &= ~TAG_PRE;
        else if (i + 5 <= len && strncmp(&text[i], "<code", 5) == 0) tags |= TAG_CODE;
        else if (i + 7 <= len && strncmp(&text[i], "</code>", 7) == 0) tags &= ~TAG_CODE;
        else if (i + 3 <= len && strncmp(&text[i], "<b>", 3) == 0) tags |= TAG_B;
        else if (i + 4 <= len && strncmp(&text[i], "</b>", 4) == 0) tags &= ~TAG_B;
        else if (i + 3 <= len && strncmp(&text[i], "<i>", 3) == 0) tags |= TAG_I;
        else if (i + 4 <= len && strncmp(&text[i], "</i>", 4) == 0) tags &= ~TAG_I;
    }
    return tags;
}

/* Append closing tags for any open tags (inner → outer order) */
static void html_close_tags(str_t *out, int tags) {
    if (tags & TAG_I)    str_append_cstr(out, "</i>");
    if (tags & TAG_B)    str_append_cstr(out, "</b>");
    if (tags & TAG_CODE) str_append_cstr(out, "</code>");
    if (tags & TAG_PRE)  str_append_cstr(out, "</pre>");
}

/* Append opening tags for any that need to continue (outer → inner order) */
static void html_reopen_tags(str_t *out, int tags) {
    if (tags & TAG_PRE)  str_append_cstr(out, "<pre>");
    if (tags & TAG_CODE) str_append_cstr(out, "<code>");
    if (tags & TAG_B)    str_append_cstr(out, "<b>");
    if (tags & TAG_I)    str_append_cstr(out, "<i>");
}

/* Send a long message, splitting at ~4096 chars on paragraph boundaries.
 * When using HTML parse_mode, properly closes and reopens tags across splits. */
static int tg_send_long(telegram_ctx_t *ctx, const char *text,
                        const char *parse_mode) {
    size_t len = strlen(text);
    if (len <= TG_MSG_MAX) {
        return tg_api_send_message(ctx, text, parse_mode);
    }

    int is_html = parse_mode && strcmp(parse_mode, "HTML") == 0;

    /* Split into chunks */
    const char *p = text;
    size_t remaining = len;
    int part = 1;
    int total_parts = (int)((len + TG_MSG_MAX - 1) / TG_MSG_MAX);
    int carry_tags = 0;  /* tags open from previous chunk */

    while (remaining > 0) {
        /* Reserve space for part indicator + possible tag close/reopen */
        size_t reserve = 120;
        size_t chunk = remaining > TG_MSG_MAX - reserve
                       ? TG_MSG_MAX - reserve : remaining;

        /* Find a good split point (paragraph boundary) */
        if (chunk < remaining) {
            /* Look for double newline first */
            size_t best = 0;
            for (size_t i = chunk; i > chunk / 2; i--) {
                if (p[i] == '\n' && i > 0 && p[i-1] == '\n') {
                    best = i + 1;
                    break;
                }
            }
            /* Fall back to single newline */
            if (!best) {
                for (size_t i = chunk; i > chunk / 2; i--) {
                    if (p[i] == '\n') {
                        best = i + 1;
                        break;
                    }
                }
            }
            if (best) chunk = best;
        }

        /* Determine which HTML tags are open at this split point
         * by scanning from the very beginning of the text. */
        int open_tags = 0;
        if (is_html) {
            open_tags = html_open_tags(text, (size_t)(p - text) + chunk);
        }

        /* Build chunk with part indicator and tag continuity */
        str_t msg = str_new(chunk + 128);
        if (total_parts > 1) {
            str_appendf(&msg, "[%d/%d]\n", part, total_parts);
        }

        /* Reopen tags that were open at end of previous chunk */
        if (carry_tags) {
            html_reopen_tags(&msg, carry_tags);
        }

        str_append(&msg, p, chunk);

        /* Close any tags that are still open at the split point */
        if (open_tags && chunk < remaining) {
            html_close_tags(&msg, open_tags);
        }

        tg_api_send_message(ctx, msg.data, parse_mode);
        str_free(&msg);

        /* Carry open tags to next iteration */
        carry_tags = (chunk < remaining) ? open_tags : 0;

        p += chunk;
        remaining -= chunk;
        part++;

        /* Small delay between parts to avoid rate limits */
        if (remaining > 0) usleep(200000); /* 200ms */
    }

    return 0;
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
 *   | table |      → <pre>table rows</pre>  (separator lines stripped)
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

            /* Markdown table: consecutive lines starting with | → <pre> */
            if (is_table_line(md, i)) {
                str_append_cstr(&out, "<pre>");
                while (md[i] && is_table_line(md, i)) {
                    /* Skip separator lines (|---|---|) */
                    if (is_table_separator(md, i)) {
                        while (md[i] && md[i] != '\n') i++;
                        if (md[i] == '\n') i++;
                        continue;
                    }
                    /* Emit table row with HTML escaping */
                    while (md[i] && md[i] != '\n') {
                        html_escape_append(&out, &md[i], 1);
                        i++;
                    }
                    if (md[i] == '\n') {
                        str_append_cstr(&out, "\n");
                        i++;
                    }
                }
                str_append_cstr(&out, "</pre>");
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

/* Read and remove a file from the outbox. Caller frees result. */
static char *tg_read_outbox(const char *path) {
    size_t len = 0;
    char *data = slurp_file(path, &len);
    if (data) unlink(path);
    return data;
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
        /* Task result → convert markdown and send */
        char *html = md_to_html(content);
        tg_send_long(ctx, html, "HTML");
        free(html);
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

                cJSON *text = cJSON_GetObjectItem(msg, "text");
                if (!text || !text->valuestring) continue;

                const char *msg_text = text->valuestring;
                fprintf(stderr, "[telegram] received: %.100s%s\n",
                        msg_text, strlen(msg_text) > 100 ? "..." : "");

                /* Route message: answer to pending ask, or new task */
                if (pending_ask_id[0]) {
                    /* This is an answer to a user_ask question */
                    fprintf(stderr, "[telegram] routing as answer to ask_%s\n",
                            pending_ask_id);
                    tg_write_answer(ctx->mailbox_dir, pending_ask_id, msg_text);
                    pending_ask_id[0] = 0;
                    tg_api_send_message(ctx, "✓ Answer received", NULL);
                } else {
                    /* New task query */
                    char task_id[64];
                    struct timespec ts;
                    clock_gettime(CLOCK_REALTIME, &ts);
                    snprintf(task_id, sizeof(task_id), "tg%lx%04lx",
                             (long)ts.tv_sec, ts.tv_nsec / 100000L);

                    fprintf(stderr, "[telegram] creating task_%s\n", task_id);
                    tg_write_task(ctx->mailbox_dir, task_id, msg_text);
                    tg_api_send_message(ctx, "⏳ Processing...", NULL);
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
