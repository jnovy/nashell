/*
 * matrix.c — Native Matrix bridge for nash daemon mode.
 *
 * Bridges the mailbox system to Matrix: incoming messages become tasks,
 * outbox results/questions are sent as formatted Matrix messages.
 *
 * Formatting: sends markdown as org.matrix.custom.html formatted messages.
 * Reuses md_to_html() from telegram.c for markdown→HTML conversion.
 *
 * Threading model:
 *   The matrix_run() function runs in its own pthread, started by main.c.
 *   It alternates between /sync long-polls and inotify-based outbox
 *   watching, using a short sync timeout to multiplex both.
 */

#include "matrix.h"
#include "str.h"
#include "cJSON.h"
#include "toml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <dirent.h>
#include <poll.h>
#include <curl/curl.h>
#include <pthread.h>

/* Matrix API constants */
#define MX_SYNC_TIMEOUT   5000   /* /sync timeout in ms (5 seconds) */
#define MX_RETRY_DELAY    5      /* seconds to wait after API error */
#define MX_URL_MAX        1024
#define MX_MSG_MAX        32000  /* conservative limit for message body */

/* ── Forward declarations ────────────────────────────────── */

static int  mx_api_login(matrix_ctx_t *ctx, const char *user, const char *pass);
static int  mx_api_whoami(matrix_ctx_t *ctx);
static int  mx_api_sync(matrix_ctx_t *ctx, cJSON **out_rooms);
static int  mx_api_send_message(matrix_ctx_t *ctx, const char *text,
                                const char *html);
static int  mx_api_send_markdown(matrix_ctx_t *ctx, const char *md_text);
static int  mx_api_join_room(matrix_ctx_t *ctx, const char *room_id_or_alias);
static int  mx_api_create_room(matrix_ctx_t *ctx, const char *name,
                               const char *invite_user);
static void mx_process_outbox_file(matrix_ctx_t *ctx, const char *filename);
static int  mx_config_save(matrix_ctx_t *ctx);
static int  mx_config_load(matrix_ctx_t *ctx);
static void mx_write_task(const char *mailbox_dir, const char *task_id,
                          const char *text);
static void mx_write_answer(const char *mailbox_dir, const char *ask_id,
                            const char *text);
static void mx_write_cmd_new(const char *mailbox_dir);
static int  mx_download_image(matrix_ctx_t *ctx, const char *mxc_url,
                              char *out_path, size_t out_sz);
static int  mx_api_upload_media(matrix_ctx_t *ctx, const char *file_path,
                                char *out_mxc, size_t mxc_sz);
static int  mx_api_send_image(matrix_ctx_t *ctx, const char *mxc_url,
                              const char *filename, const char *mimetype,
                              size_t filesize);

/* Defined in telegram.c — shared markdown→HTML converter */
extern char *md_to_html(const char *md);


/* ── URL encoding helper ─────────────────────────────────── */

/* URL-encode a string (for room IDs with ! and : characters).
 * Returns heap-allocated string. Caller frees. */
static char *url_encode(const char *s) {
    if (!s) return strdup("");
    size_t len = strlen(s);
    /* Worst case: every char becomes %XX (3x) */
    char *out = malloc(len * 3 + 1);
    if (!out) return strdup("");
    char *p = out;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            *p++ = c;
        } else {
            p += sprintf(p, "%%%02X", (unsigned char)c);
        }
    }
    *p = '\0';
    return out;
}


/* ── Initialization ──────────────────────────────────────── */

int matrix_init(matrix_ctx_t *ctx, const char *config_path,
                const char *mailbox_dir,
                volatile sig_atomic_t *shutdown) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->config_path = strdup(config_path);
    ctx->mailbox_dir = strdup(mailbox_dir);
    ctx->shutdown = shutdown;
    ctx->txn_counter = (long long)time(NULL) * 1000;  /* unique start */

    /* Try loading existing config */
    mx_config_load(ctx);
    return 0;
}

void matrix_free(matrix_ctx_t *ctx) {
    free(ctx->homeserver);
    free(ctx->access_token);
    free(ctx->user_id);
    free(ctx->room_id);
    free(ctx->since_token);
    free(ctx->mailbox_dir);
    free(ctx->config_path);
    memset(ctx, 0, sizeof(*ctx));
}


/* ── Config load/save ────────────────────────────────────── */

static int mx_config_load(matrix_ctx_t *ctx) {
    FILE *f = fopen(ctx->config_path, "r");
    if (!f) return -1;

    char errbuf[256];
    toml_table_t *root = toml_parse_file(f, errbuf, sizeof(errbuf));
    fclose(f);
    if (!root) return -1;

    toml_table_t *mx = toml_table_in(root, "matrix");
    if (mx) {
        toml_datum_t d;

        d = toml_string_in(mx, "homeserver");
        if (d.ok) ctx->homeserver = d.u.s;

        d = toml_string_in(mx, "access_token");
        if (d.ok) ctx->access_token = d.u.s;

        d = toml_string_in(mx, "user_id");
        if (d.ok) ctx->user_id = d.u.s;

        d = toml_string_in(mx, "room_id");
        if (d.ok) ctx->room_id = d.u.s;

        d = toml_string_in(mx, "since_token");
        if (d.ok) ctx->since_token = d.u.s;
    }
    toml_free(root);
    return (ctx->homeserver && ctx->access_token && ctx->room_id) ? 0 : -1;
}

static int mx_config_save(matrix_ctx_t *ctx) {
    /* Read existing config, replace [matrix] section */
    char *existing = slurp_file(ctx->config_path, NULL);

    FILE *f = fopen(ctx->config_path, "w");
    if (!f) {
        free(existing);
        return -1;
    }

    /* Write back existing content, removing old [matrix] section */
    if (existing) {
        char *mx_start = strstr(existing, "\n[matrix]");
        if (mx_start) {
            char *next = strstr(mx_start + 1, "\n[");
            if (next) {
                fwrite(existing, 1, (size_t)(mx_start - existing), f);
                fputs(next, f);
            } else {
                fwrite(existing, 1, (size_t)(mx_start - existing), f);
            }
        } else {
            fputs(existing, f);
        }
        free(existing);
    }

    /* Append [matrix] section */
    fprintf(f, "\n[matrix]\nhomeserver = \"%s\"\naccess_token = \"%s\"\n",
            ctx->homeserver ? ctx->homeserver : "",
            ctx->access_token ? ctx->access_token : "");
    if (ctx->user_id)
        fprintf(f, "user_id = \"%s\"\n", ctx->user_id);
    if (ctx->room_id)
        fprintf(f, "room_id = \"%s\"\n", ctx->room_id);
    if (ctx->since_token)
        fprintf(f, "since_token = \"%s\"\n", ctx->since_token);

    fclose(f);
    return 0;
}


/* ── Interactive setup ───────────────────────────────────── */

int matrix_setup(matrix_ctx_t *ctx) {
    char buf[512];

    fprintf(stderr, "\n╔══════════════════════════════════════════════╗\n");
    fprintf(stderr, "║       Nash Matrix Bridge Setup               ║\n");
    fprintf(stderr, "╚══════════════════════════════════════════════╝\n\n");

    /* Step 1: Get homeserver URL */
    fprintf(stderr, "Enter Matrix homeserver URL\n");
    fprintf(stderr, "  (e.g. http://100.118.224.104:8008): ");
    fflush(stderr);
    if (!fgets(buf, sizeof(buf), stdin) || *ctx->shutdown) {
        fprintf(stderr, "\n[matrix] aborted\n");
        return -1;
    }
    buf[strcspn(buf, "\r\n")] = 0;
    if (!buf[0]) {
        fprintf(stderr, "[matrix] empty URL\n");
        return -1;
    }
    /* Strip trailing slash */
    size_t blen = strlen(buf);
    if (blen > 0 && buf[blen-1] == '/') buf[blen-1] = 0;
    ctx->homeserver = strdup(buf);

    /* Step 2: Get credentials */
    fprintf(stderr, "Matrix username (e.g. nash): ");
    fflush(stderr);
    if (!fgets(buf, sizeof(buf), stdin) || *ctx->shutdown) {
        fprintf(stderr, "\n[matrix] aborted\n");
        return -1;
    }
    buf[strcspn(buf, "\r\n")] = 0;
    char username[128];
    snprintf(username, sizeof(username), "%s", buf);

    fprintf(stderr, "Matrix password: ");
    fflush(stderr);
    if (!fgets(buf, sizeof(buf), stdin) || *ctx->shutdown) {
        fprintf(stderr, "\n[matrix] aborted\n");
        return -1;
    }
    buf[strcspn(buf, "\r\n")] = 0;

    /* Step 3: Login */
    if (mx_api_login(ctx, username, buf) != 0) {
        fprintf(stderr, "[matrix] ✗ Login failed\n");
        return -1;
    }
    fprintf(stderr, "[matrix] ✓ Logged in as %s\n\n", ctx->user_id);

    /* Step 4: Room setup */
    fprintf(stderr, "Enter room ID or alias to join\n");
    fprintf(stderr, "  (leave empty to create 'Nash Agent' room): ");
    fflush(stderr);
    if (!fgets(buf, sizeof(buf), stdin) || *ctx->shutdown) {
        fprintf(stderr, "\n[matrix] aborted\n");
        return -1;
    }
    buf[strcspn(buf, "\r\n")] = 0;

    if (buf[0]) {
        /* Join existing room */
        if (mx_api_join_room(ctx, buf) != 0) {
            fprintf(stderr, "[matrix] ✗ Failed to join room %s\n", buf);
            return -1;
        }
    } else {
        /* Create new room */
        fprintf(stderr, "User to invite (e.g. @jnovy:localhost, empty to skip): ");
        fflush(stderr);
        char invite[256] = {0};
        if (fgets(invite, sizeof(invite), stdin) && !*ctx->shutdown) {
            invite[strcspn(invite, "\r\n")] = 0;
        }
        if (mx_api_create_room(ctx, "Nash Agent", invite[0] ? invite : NULL) != 0) {
            fprintf(stderr, "[matrix] ✗ Failed to create room\n");
            return -1;
        }
    }
    fprintf(stderr, "[matrix] ✓ Room: %s\n", ctx->room_id);

    /* Step 5: Initial sync to get since_token (skip history) */
    fprintf(stderr, "[matrix] performing initial sync...\n");
    cJSON *dummy = NULL;
    mx_api_sync(ctx, &dummy);
    if (dummy) cJSON_Delete(dummy);

    /* Step 6: Save config */
    if (mx_config_save(ctx) == 0) {
        fprintf(stderr, "[matrix] ✓ Saved to %s\n", ctx->config_path);
    } else {
        fprintf(stderr, "[matrix] ✗ Failed to save config\n");
        return -1;
    }

    /* Step 7: Send greeting */
    mx_api_send_message(ctx, "🤖 Nash bot connected! Send me queries.", NULL);
    fprintf(stderr, "[matrix] ✓ Setup complete — bot is ready\n\n");

    return 0;
}


/* ── Matrix API calls ────────────────────────────────────── */

/* Build an Authorization header. Returns heap-allocated string. Caller frees. */
static char *mx_auth_header(matrix_ctx_t *ctx) {
    char *hdr = malloc(strlen(ctx->access_token) + 32);
    if (hdr) sprintf(hdr, "Authorization: Bearer %s", ctx->access_token);
    return hdr;
}

static int mx_api_login(matrix_ctx_t *ctx, const char *user, const char *pass) {
    char url[MX_URL_MAX];
    snprintf(url, sizeof(url), "%s/_matrix/client/v3/login", ctx->homeserver);

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "type", "m.login.password");
    cJSON *ident = cJSON_CreateObject();
    cJSON_AddStringToObject(ident, "type", "m.id.user");
    cJSON_AddStringToObject(ident, "user", user);
    cJSON_AddItemToObject(body, "identifier", ident);
    cJSON_AddStringToObject(body, "password", pass);
    cJSON_AddStringToObject(body, "initial_device_display_name", "nash-bot");

    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    str_t resp = str_new(1024);
    int rc = http_post(url, body_str, headers, 15, &resp);

    if (rc == 0 && resp.len > 0) {
        cJSON *rjson = cJSON_Parse(resp.data);
        if (rjson) {
            cJSON *token = cJSON_GetObjectItem(rjson, "access_token");
            cJSON *uid = cJSON_GetObjectItem(rjson, "user_id");
            if (token && token->valuestring && uid && uid->valuestring) {
                free(ctx->access_token);
                ctx->access_token = strdup(token->valuestring);
                free(ctx->user_id);
                ctx->user_id = strdup(uid->valuestring);
            } else {
                cJSON *err = cJSON_GetObjectItem(rjson, "error");
                fprintf(stderr, "[matrix] login error: %s\n",
                        err ? err->valuestring : "unknown");
                rc = -1;
            }
            cJSON_Delete(rjson);
        } else {
            rc = -1;
        }
    } else {
        rc = -1;
    }

    str_free(&resp);
    curl_slist_free_all(headers);
    free(body_str);
    return rc;
}

static int mx_api_whoami(matrix_ctx_t *ctx) {
    char url[MX_URL_MAX];
    snprintf(url, sizeof(url), "%s/_matrix/client/v3/account/whoami",
             ctx->homeserver);

    /* Add auth header via curl handle — but we use http_get which doesn't
     * support custom headers. So we'll use http_post with GET method...
     * Actually, let's just build the URL with access_token query param */
    char authed_url[MX_URL_MAX * 2];
    snprintf(authed_url, sizeof(authed_url), "%s?access_token=%s",
             url, ctx->access_token);

    str_t resp = str_new(512);
    int rc = http_get(authed_url, 10, &resp);

    if (rc == 0 && resp.len > 0) {
        cJSON *rjson = cJSON_Parse(resp.data);
        if (rjson) {
            cJSON *uid = cJSON_GetObjectItem(rjson, "user_id");
            if (uid && uid->valuestring) {
                free(ctx->user_id);
                ctx->user_id = strdup(uid->valuestring);
            } else {
                rc = -1;
            }
            cJSON_Delete(rjson);
        } else {
            rc = -1;
        }
    } else {
        rc = -1;
    }

    str_free(&resp);
    return rc;
}

/*
 * Perform a /sync request. Extracts timeline events for our room_id.
 * Updates ctx->since_token for next call.
 * out_events receives a cJSON array of m.room.message events (caller must cJSON_Delete).
 * Returns 0 on success, -1 on error.
 */
static int mx_api_sync(matrix_ctx_t *ctx, cJSON **out_events) {
    *out_events = NULL;

    /* Build /sync URL with since token and timeout */
    char url[MX_URL_MAX * 2];
    if (ctx->since_token) {
        snprintf(url, sizeof(url),
                 "%s/_matrix/client/v3/sync?timeout=%d&since=%s"
                 "&filter={\"room\":{\"timeline\":{\"limit\":50},"
                 "\"ephemeral\":{\"types\":[]}},"
                 "\"presence\":{\"types\":[]}}",
                 ctx->homeserver, MX_SYNC_TIMEOUT, ctx->since_token);
    } else {
        /* Initial sync — minimal data, just get the since token */
        snprintf(url, sizeof(url),
                 "%s/_matrix/client/v3/sync?timeout=0"
                 "&filter={\"room\":{\"timeline\":{\"limit\":0},"
                 "\"ephemeral\":{\"types\":[]}},"
                 "\"presence\":{\"types\":[]}}",
                 ctx->homeserver);
    }

    /* We need to send auth header. http_get doesn't support custom headers,
     * so use a custom curl setup. */
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    str_t resp = str_new(4096);
    char *auth = mx_auth_header(ctx);
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, str_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)(MX_SYNC_TIMEOUT / 1000 + 10));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode crc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    free(auth);

    if (crc != CURLE_OK) {
        str_free(&resp);
        return -1;
    }

    cJSON *root = cJSON_Parse(resp.data);
    str_free(&resp);
    if (!root) return -1;

    /* Update since_token */
    cJSON *next_batch = cJSON_GetObjectItem(root, "next_batch");
    if (next_batch && next_batch->valuestring) {
        free(ctx->since_token);
        ctx->since_token = strdup(next_batch->valuestring);
    }

    /* Extract timeline events from our room */
    if (ctx->room_id) {
        cJSON *rooms = cJSON_GetObjectItem(root, "rooms");
        cJSON *join = rooms ? cJSON_GetObjectItem(rooms, "join") : NULL;
        cJSON *room = join ? cJSON_GetObjectItem(join, ctx->room_id) : NULL;
        cJSON *timeline = room ? cJSON_GetObjectItem(room, "timeline") : NULL;
        cJSON *events = timeline ? cJSON_GetObjectItem(timeline, "events") : NULL;

        if (events && cJSON_IsArray(events) && cJSON_GetArraySize(events) > 0) {
            *out_events = cJSON_DetachItemFromObject(timeline, "events");
        }
    }

    cJSON_Delete(root);
    return 0;
}

/*
 * Send a message to the configured room.
 * If html is provided, sends a formatted message.
 * If html is NULL, sends plain text only.
 */
static int mx_api_send_message_inner(matrix_ctx_t *ctx, const char *text,
                                     const char *html, int retries_left) {
    char *enc_room = url_encode(ctx->room_id);
    char url[MX_URL_MAX * 2];
    long long txn = __sync_fetch_and_add(&ctx->txn_counter, 1);
    snprintf(url, sizeof(url),
             "%s/_matrix/client/v3/rooms/%s/send/m.room.message/m%lld",
             ctx->homeserver, enc_room, txn);
    free(enc_room);

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "msgtype", "m.text");
    cJSON_AddStringToObject(body, "body", text);
    if (html) {
        cJSON_AddStringToObject(body, "format", "org.matrix.custom.html");
        cJSON_AddStringToObject(body, "formatted_body", html);
    }

    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    char *auth = mx_auth_header(ctx);
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth);
    headers = curl_slist_append(headers, "Content-Type: application/json");

    /* Use custom curl for PUT request */
    CURL *curl = curl_easy_init();
    if (!curl) {
        free(auth);
        free(body_str);
        curl_slist_free_all(headers);
        return -1;
    }

    str_t resp = str_new(512);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, str_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode crc = curl_easy_perform(curl);
    long http_code = 0;
    int rc = 0;

    if (crc != CURLE_OK) {
        fprintf(stderr, "[matrix] send error: %s\n", curl_easy_strerror(crc));
        rc = -1;
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (resp.len > 0) {
            cJSON *rjson = cJSON_Parse(resp.data);
            if (rjson) {
                cJSON *event_id = cJSON_GetObjectItem(rjson, "event_id");
                if (!event_id || !event_id->valuestring) {
                    cJSON *err = cJSON_GetObjectItem(rjson, "error");
                    /* On 429 Too Many Requests — retry after backoff */
                    if (http_code == 429 && retries_left > 0) {
                        int wait_ms = 2000;  /* default 2s */
                        cJSON *retry = cJSON_GetObjectItem(rjson, "retry_after_ms");
                        if (retry && retry->valueint > 0)
                            wait_ms = retry->valueint + 100;  /* plus margin */
                        cJSON_Delete(rjson);
                        curl_easy_cleanup(curl);
                        str_free(&resp);
                        curl_slist_free_all(headers);
                        free(auth);
                        free(body_str);
                        fprintf(stderr, "[matrix] rate limited, retrying in %dms\n",
                                wait_ms);
                        usleep((useconds_t)wait_ms * 1000);
                        return mx_api_send_message_inner(ctx, text, html,
                                                        retries_left - 1);
                    }
                    fprintf(stderr, "[matrix] send error: %s\n",
                            err ? err->valuestring : "unknown");
                    rc = -1;
                }
                cJSON_Delete(rjson);
            }
        }
    }

    curl_easy_cleanup(curl);
    str_free(&resp);
    curl_slist_free_all(headers);
    free(auth);
    free(body_str);
    return rc;
}

static int mx_api_send_message(matrix_ctx_t *ctx, const char *text,
                               const char *html) {
    return mx_api_send_message_inner(ctx, text, html, 3);  /* up to 3 retries */
}

/* Send markdown as an HTML-formatted Matrix message */
static int mx_api_send_markdown(matrix_ctx_t *ctx, const char *md_text) {
    /* Convert markdown to HTML */
    char *html = md_to_html(md_text);
    int rc = mx_api_send_message(ctx, md_text, html);
    free(html);
    return rc;
}


/* ── Image support ───────────────────────────────────────── */

/*
 * Guess MIME type from filename extension.
 * Returns a static string (no need to free).
 */
static const char *mx_mime_from_ext(const char *filename) {
    if (!filename) return "application/octet-stream";
    const char *dot = strrchr(filename, '.');
    if (!dot) return "application/octet-stream";
    if (strcasecmp(dot, ".png") == 0)  return "image/png";
    if (strcasecmp(dot, ".jpg") == 0)  return "image/jpeg";
    if (strcasecmp(dot, ".jpeg") == 0) return "image/jpeg";
    if (strcasecmp(dot, ".gif") == 0)  return "image/gif";
    if (strcasecmp(dot, ".webp") == 0) return "image/webp";
    if (strcasecmp(dot, ".bmp") == 0)  return "image/bmp";
    if (strcasecmp(dot, ".svg") == 0)  return "image/svg+xml";
    if (strcasecmp(dot, ".tiff") == 0) return "image/tiff";
    if (strcasecmp(dot, ".tif") == 0)  return "image/tiff";
    return "application/octet-stream";
}

/*
 * Download an image from a Matrix mxc:// URL.
 *
 * Parses mxc://server/mediaId → GET /_matrix/media/v3/download/{server}/{mediaId}
 * Saves the image to ~/.nash/images/<timestamp>_<mediaId>.<ext>
 * Writes the local path to out_path.
 *
 * Returns 0 on success, -1 on error.
 */
static int mx_download_image(matrix_ctx_t *ctx, const char *mxc_url,
                             char *out_path, size_t out_sz) {
    if (!mxc_url || strncmp(mxc_url, "mxc://", 6) != 0) {
        fprintf(stderr, "[matrix] invalid mxc URL: %s\n",
                mxc_url ? mxc_url : "(null)");
        return -1;
    }

    /* Parse mxc://server/mediaId */
    const char *rest = mxc_url + 6;  /* skip "mxc://" */
    const char *slash = strchr(rest, '/');
    if (!slash || !slash[1]) {
        fprintf(stderr, "[matrix] malformed mxc URL: %s\n", mxc_url);
        return -1;
    }

    char server[256], media_id[256];
    size_t slen = (size_t)(slash - rest);
    if (slen >= sizeof(server)) slen = sizeof(server) - 1;
    memcpy(server, rest, slen);
    server[slen] = '\0';
    snprintf(media_id, sizeof(media_id), "%s", slash + 1);

    /* Try two download endpoints:
     * 1. /_matrix/media/v3/download/ (legacy, unauthenticated)
     * 2. /_matrix/client/v1/media/download/ (authenticated, Synapse 1.96+)
     * Some homeservers have disabled the legacy endpoint. */
    const char *endpoints[] = {
        "/_matrix/media/v3/download/%s/%s",
        "/_matrix/client/v1/media/download/%s/%s",
    };
    const int num_endpoints = 2;

    str_t resp = str_new(256 * 1024);  /* 256KB initial */
    char *ct_buf = NULL;
    long http_code = 0;
    CURLcode crc = CURLE_OK;

    for (int ep = 0; ep < num_endpoints; ep++) {
        char url[MX_URL_MAX * 2];
        char fmt[128];
        snprintf(fmt, sizeof(fmt), "%%s%s", endpoints[ep]);
        snprintf(url, sizeof(url), fmt, ctx->homeserver, server, media_id);

        CURL *curl = curl_easy_init();
        if (!curl) { str_free(&resp); return -1; }

        resp.len = 0;  /* reset for retry */
        char *auth = mx_auth_header(ctx);
        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, auth);

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, str_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

        http_code = 0;
        free(ct_buf); ct_buf = NULL;
        crc = curl_easy_perform(curl);

        if (crc == CURLE_OK) {
            char *ct = NULL;
            curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ct);
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            if (ct) ct_buf = strdup(ct);
        }

        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
        free(auth);

        /* Success — break out */
        if (crc == CURLE_OK && resp.len > 0 && http_code < 400)
            break;

        /* On error with first endpoint, try the next */
        if (ep < num_endpoints - 1) {
            fprintf(stderr, "[matrix] download via %s failed (HTTP %ld), "
                    "trying authenticated endpoint\n",
                    ep == 0 ? "media/v3" : "client/v1", http_code);
        }
    }

    if (crc != CURLE_OK || resp.len == 0 || http_code >= 400) {
        fprintf(stderr, "[matrix] image download failed: %s%s",
                crc != CURLE_OK ? curl_easy_strerror(crc) :
                http_code >= 400 ? "HTTP error" : "empty response",
                http_code >= 400 ? "" : "\n");
        if (http_code >= 400)
            fprintf(stderr, " %ld\n", http_code);
        str_free(&resp);
        free(ct_buf);
        return -1;
    }

    /* Determine file extension from Content-Type */
    const char *ext = ".bin";
    if (ct_buf) {
        if (strstr(ct_buf, "image/png"))       ext = ".png";
        else if (strstr(ct_buf, "image/jpeg"))  ext = ".jpg";
        else if (strstr(ct_buf, "image/gif"))   ext = ".gif";
        else if (strstr(ct_buf, "image/webp"))  ext = ".webp";
        else if (strstr(ct_buf, "image/bmp"))   ext = ".bmp";
        else if (strstr(ct_buf, "image/svg"))   ext = ".svg";
        else if (strstr(ct_buf, "image/tiff"))  ext = ".tiff";
    }
    free(ct_buf);

    /* Fallback: detect format from magic bytes when Content-Type was
     * unhelpful (e.g. application/octet-stream).  Without this, the
     * file gets saved as .bin and image_analyze rejects it as
     * "unsupported image format". */
    if (strcmp(ext, ".bin") == 0 && resp.len >= 12) {
        const unsigned char *h = (const unsigned char *)resp.data;
        if (h[0] == 0x89 && h[1] == 'P' && h[2] == 'N' && h[3] == 'G')
            ext = ".png";
        else if (h[0] == 0xFF && h[1] == 0xD8)
            ext = ".jpg";
        else if (h[0] == 'G' && h[1] == 'I' && h[2] == 'F')
            ext = ".gif";
        else if (h[0] == 'R' && h[1] == 'I' && h[2] == 'F' && h[3] == 'F' &&
                 h[8] == 'W' && h[9] == 'E' && h[10] == 'B' && h[11] == 'P')
            ext = ".webp";
        else if (h[0] == 'B' && h[1] == 'M')
            ext = ".bmp";
        else if ((h[0] == 0x49 && h[1] == 0x49) ||
                 (h[0] == 0x4D && h[1] == 0x4D))
            ext = ".tiff";
        /* SVG: starts with '<' (XML) — crude check */
        else if (h[0] == '<')
            ext = ".svg";
    }

    /* Save to ~/.nash/images/ */
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

    /* Build local filename: <timestamp>_<mediaId>.<ext> */
    char local_name[512];
    snprintf(local_name, sizeof(local_name), "%ld_%s%s",
             (long)time(NULL), media_id, ext);

    char local_path[1040];
    snprintf(local_path, sizeof(local_path), "%s/%s", images_dir, local_name);

    /* Write atomically */
    char tmp_path[1048];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", local_path);

    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        fprintf(stderr, "[matrix] failed to create %s: %s\n",
                tmp_path, strerror(errno));
        str_free(&resp);
        return -1;
    }
    size_t written = fwrite(resp.data, 1, resp.len, f);
    fclose(f);

    if (written != resp.len) {
        fprintf(stderr, "[matrix] short write to %s\n", tmp_path);
        unlink(tmp_path);
        str_free(&resp);
        return -1;
    }

    if (rename(tmp_path, local_path) != 0) {
        fprintf(stderr, "[matrix] rename failed: %s\n", strerror(errno));
        unlink(tmp_path);
        str_free(&resp);
        return -1;
    }

    fprintf(stderr, "[matrix] saved image: %s (%.1f KB)\n",
            local_path, (double)written / 1024.0);
    str_free(&resp);

    snprintf(out_path, out_sz, "%s", local_path);
    return 0;
}

/*
 * Upload a local file to the Matrix content repository.
 *
 * POST /_matrix/media/v3/upload
 * Returns the mxc:// content_uri in out_mxc.
 *
 * Returns 0 on success, -1 on error.
 */
static int mx_api_upload_media(matrix_ctx_t *ctx, const char *file_path,
                               char *out_mxc, size_t mxc_sz) {
    /* Read the file */
    struct stat st;
    if (stat(file_path, &st) != 0 || st.st_size == 0) {
        fprintf(stderr, "[matrix] upload: cannot stat %s\n", file_path);
        return -1;
    }

    FILE *f = fopen(file_path, "rb");
    if (!f) {
        fprintf(stderr, "[matrix] upload: cannot open %s\n", file_path);
        return -1;
    }

    char *file_data = malloc((size_t)st.st_size);
    if (!file_data) { fclose(f); return -1; }

    size_t nread = fread(file_data, 1, (size_t)st.st_size, f);
    fclose(f);
    if (nread != (size_t)st.st_size) {
        fprintf(stderr, "[matrix] upload: short read of %s\n", file_path);
        free(file_data);
        return -1;
    }

    /* Extract filename for the upload */
    const char *basename = strrchr(file_path, '/');
    basename = basename ? basename + 1 : file_path;

    /* Determine Content-Type */
    const char *mimetype = mx_mime_from_ext(basename);

    /* Build upload URL with filename query param */
    char *enc_name = url_encode(basename);
    char url[MX_URL_MAX * 2];
    snprintf(url, sizeof(url),
             "%s/_matrix/media/v3/upload?filename=%s",
             ctx->homeserver, enc_name);
    free(enc_name);

    /* Build headers */
    char *auth = mx_auth_header(ctx);
    char ct_header[256];
    snprintf(ct_header, sizeof(ct_header), "Content-Type: %s", mimetype);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth);
    headers = curl_slist_append(headers, ct_header);

    /* POST the binary data */
    CURL *curl = curl_easy_init();
    if (!curl) {
        free(auth);
        free(file_data);
        curl_slist_free_all(headers);
        return -1;
    }

    str_t resp = str_new(512);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, file_data);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)nread);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, str_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode crc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    free(auth);
    free(file_data);

    if (crc != CURLE_OK) {
        fprintf(stderr, "[matrix] upload error: %s\n", curl_easy_strerror(crc));
        str_free(&resp);
        return -1;
    }

    /* Parse response for content_uri */
    int rc = -1;
    cJSON *rjson = cJSON_Parse(resp.data);
    if (rjson) {
        cJSON *uri = cJSON_GetObjectItem(rjson, "content_uri");
        if (uri && uri->valuestring) {
            snprintf(out_mxc, mxc_sz, "%s", uri->valuestring);
            fprintf(stderr, "[matrix] uploaded %s → %s\n", basename, out_mxc);
            rc = 0;
        } else {
            cJSON *err = cJSON_GetObjectItem(rjson, "error");
            fprintf(stderr, "[matrix] upload error: %s\n",
                    err ? err->valuestring : "no content_uri in response");
        }
        cJSON_Delete(rjson);
    }

    str_free(&resp);
    return rc;
}

/*
 * Send an image message (m.image) to the configured room.
 *
 * mxc_url: the mxc:// content URI from upload
 * filename: display filename
 * mimetype: MIME type (e.g. "image/png")
 * filesize: file size in bytes
 *
 * Returns 0 on success, -1 on error.
 */
static int mx_api_send_image(matrix_ctx_t *ctx, const char *mxc_url,
                             const char *filename, const char *mimetype,
                             size_t filesize) {
    char *enc_room = url_encode(ctx->room_id);
    char url[MX_URL_MAX * 2];
    long long txn = __sync_fetch_and_add(&ctx->txn_counter, 1);
    snprintf(url, sizeof(url),
             "%s/_matrix/client/v3/rooms/%s/send/m.room.message/m%lld",
             ctx->homeserver, enc_room, txn);
    free(enc_room);

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "msgtype", "m.image");
    cJSON_AddStringToObject(body, "body", filename ? filename : "image");
    cJSON_AddStringToObject(body, "url", mxc_url);

    /* Add info object with mimetype and size */
    cJSON *info = cJSON_CreateObject();
    if (mimetype)
        cJSON_AddStringToObject(info, "mimetype", mimetype);
    if (filesize > 0)
        cJSON_AddNumberToObject(info, "size", (double)filesize);
    cJSON_AddItemToObject(body, "info", info);

    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    char *auth = mx_auth_header(ctx);
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth);
    headers = curl_slist_append(headers, "Content-Type: application/json");

    /* PUT request (same as send_message) */
    CURL *curl = curl_easy_init();
    if (!curl) {
        free(auth);
        free(body_str);
        curl_slist_free_all(headers);
        return -1;
    }

    str_t resp = str_new(512);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, str_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode crc = curl_easy_perform(curl);
    int rc = 0;

    if (crc != CURLE_OK) {
        fprintf(stderr, "[matrix] send_image error: %s\n",
                curl_easy_strerror(crc));
        rc = -1;
    } else if (resp.len > 0) {
        cJSON *rjson = cJSON_Parse(resp.data);
        if (rjson) {
            cJSON *event_id = cJSON_GetObjectItem(rjson, "event_id");
            if (!event_id || !event_id->valuestring) {
                cJSON *err = cJSON_GetObjectItem(rjson, "error");
                fprintf(stderr, "[matrix] send_image error: %s\n",
                        err ? err->valuestring : "unknown");
                rc = -1;
            }
            cJSON_Delete(rjson);
        }
    }

    curl_easy_cleanup(curl);
    str_free(&resp);
    curl_slist_free_all(headers);
    free(auth);
    free(body_str);
    return rc;
}


static int mx_api_join_room(matrix_ctx_t *ctx, const char *room_id_or_alias) {
    char *enc = url_encode(room_id_or_alias);
    char url[MX_URL_MAX * 2];
    snprintf(url, sizeof(url), "%s/_matrix/client/v3/join/%s",
             ctx->homeserver, enc);
    free(enc);

    char *auth = mx_auth_header(ctx);
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth);
    headers = curl_slist_append(headers, "Content-Type: application/json");

    str_t resp = str_new(512);
    int rc = http_post(url, "{}", headers, 15, &resp);

    if (rc == 0 && resp.len > 0) {
        cJSON *rjson = cJSON_Parse(resp.data);
        if (rjson) {
            cJSON *rid = cJSON_GetObjectItem(rjson, "room_id");
            if (rid && rid->valuestring) {
                free(ctx->room_id);
                ctx->room_id = strdup(rid->valuestring);
            } else {
                cJSON *err = cJSON_GetObjectItem(rjson, "error");
                fprintf(stderr, "[matrix] join error: %s\n",
                        err ? err->valuestring : "unknown");
                rc = -1;
            }
            cJSON_Delete(rjson);
        }
    }

    str_free(&resp);
    curl_slist_free_all(headers);
    free(auth);
    return rc;
}

static int mx_api_create_room(matrix_ctx_t *ctx, const char *name,
                              const char *invite_user) {
    char url[MX_URL_MAX];
    snprintf(url, sizeof(url), "%s/_matrix/client/v3/createRoom",
             ctx->homeserver);

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "name", name);
    cJSON_AddStringToObject(body, "topic", "Nash agent communication channel");
    cJSON_AddStringToObject(body, "preset", "private_chat");
    if (invite_user) {
        cJSON *inv = cJSON_CreateArray();
        cJSON_AddItemToArray(inv, cJSON_CreateString(invite_user));
        cJSON_AddItemToObject(body, "invite", inv);
    }

    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    char *auth = mx_auth_header(ctx);
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth);
    headers = curl_slist_append(headers, "Content-Type: application/json");

    str_t resp = str_new(512);
    int rc = http_post(url, body_str, headers, 15, &resp);

    if (rc == 0 && resp.len > 0) {
        cJSON *rjson = cJSON_Parse(resp.data);
        if (rjson) {
            cJSON *rid = cJSON_GetObjectItem(rjson, "room_id");
            if (rid && rid->valuestring) {
                free(ctx->room_id);
                ctx->room_id = strdup(rid->valuestring);
            } else {
                cJSON *err = cJSON_GetObjectItem(rjson, "error");
                fprintf(stderr, "[matrix] createRoom error: %s\n",
                        err ? err->valuestring : "unknown");
                rc = -1;
            }
            cJSON_Delete(rjson);
        }
    }

    str_free(&resp);
    curl_slist_free_all(headers);
    free(auth);
    free(body_str);
    return rc;
}


/* ── Mailbox interaction ─────────────────────────────────── */

static void mx_write_task(const char *mailbox_dir, const char *task_id,
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

static void mx_write_answer(const char *mailbox_dir, const char *ask_id,
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

static void mx_write_cmd_new(const char *mailbox_dir) {
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


/* ── Outbox file processing ──────────────────────────────── */

static void mx_process_outbox_file(matrix_ctx_t *ctx, const char *filename) {
    char path[512];
    snprintf(path, sizeof(path), "%s/outbox/%s", ctx->mailbox_dir, filename);

    /* Skip .tmp files */
    size_t flen = strlen(filename);
    if (flen > 4 && strcmp(filename + flen - 4, ".tmp") == 0) return;

    /* Read and remove */
    size_t content_len = 0;
    char *content = slurp_file(path, &content_len);
    if (!content) return;
    unlink(path);

    if (strncmp(filename, "result_", 7) == 0) {
        /* Task result → send as formatted markdown */
        mx_api_send_markdown(ctx, content);
    } else if (strncmp(filename, "ask_", 4) == 0) {
        /* user_ask question → send with prompt */
        str_t msg = str_new(strlen(content) + 64);
        str_append_cstr(&msg, "❓ ");
        str_append_cstr(&msg, content);
        str_append_cstr(&msg, "\n\n_(Reply to answer)_");

        str_t html = str_new(strlen(content) + 128);
        str_append_cstr(&html, "❓ ");
        char *html_content = md_to_html(content);
        str_append_cstr(&html, html_content);
        free(html_content);
        str_append_cstr(&html, "<br/><br/><i>(Reply to answer)</i>");

        mx_api_send_message(ctx, msg.data, html.data);
        str_free(&msg);
        str_free(&html);
    } else if (strncmp(filename, "status_", 7) == 0) {
        /* Status notification */
        str_t msg = str_new(strlen(content) + 16);
        str_append_cstr(&msg, "📋 ");
        str_append_cstr(&msg, content);
        mx_api_send_message(ctx, msg.data, NULL);
        str_free(&msg);
    } else if (strncmp(filename, "image_", 6) == 0) {
        /* Image file → upload to Matrix and send as m.image.
         * File format: first line = local file path
         *              optional second line = caption text */
        char *img_path = content;
        char *caption = NULL;

        /* Split at first newline */
        char *nl = strchr(content, '\n');
        if (nl) {
            *nl = '\0';
            caption = nl + 1;
            /* Trim trailing whitespace from caption */
            size_t clen = strlen(caption);
            while (clen > 0 &&
                   (caption[clen-1] == '\n' || caption[clen-1] == '\r' ||
                    caption[clen-1] == ' '))
                caption[--clen] = '\0';
            if (!caption[0]) caption = NULL;
        }
        /* Trim trailing whitespace from path */
        size_t plen = strlen(img_path);
        while (plen > 0 &&
               (img_path[plen-1] == '\n' || img_path[plen-1] == '\r' ||
                img_path[plen-1] == ' '))
            img_path[--plen] = '\0';

        if (img_path[0]) {
            char mxc_url[512];
            if (mx_api_upload_media(ctx, img_path,
                                    mxc_url, sizeof(mxc_url)) == 0) {
                /* Extract filename for display */
                const char *basename = strrchr(img_path, '/');
                basename = basename ? basename + 1 : img_path;

                const char *mimetype = mx_mime_from_ext(basename);

                struct stat img_st;
                size_t fsize = 0;
                if (stat(img_path, &img_st) == 0)
                    fsize = (size_t)img_st.st_size;

                mx_api_send_image(ctx, mxc_url, basename, mimetype, fsize);

                /* Send caption as a follow-up text message if present */
                if (caption && caption[0]) {
                    mx_api_send_markdown(ctx, caption);
                }

                fprintf(stderr, "[matrix] sent image: %s\n", basename);
            } else {
                fprintf(stderr, "[matrix] failed to upload image: %s\n",
                        img_path);
                mx_api_send_message(ctx,
                    "⚠️ Failed to upload image to Matrix server.", NULL);
            }
        }
    }

    free(content);
}

static void mx_scan_outbox(matrix_ctx_t *ctx) {
    char outbox_path[512];
    snprintf(outbox_path, sizeof(outbox_path), "%s/outbox", ctx->mailbox_dir);

    DIR *dir = opendir(outbox_path);
    if (!dir) return;

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;
        mx_process_outbox_file(ctx, de->d_name);
    }
    closedir(dir);
}


/* ── Main bridge loop ────────────────────────────────────── */

void *matrix_run(void *arg) {
    matrix_ctx_t *ctx = (matrix_ctx_t *)arg;

    fprintf(stderr, "[matrix] bridge thread started\n");

    /* Validate token with whoami */
    if (!ctx->user_id) {
        if (mx_api_whoami(ctx) != 0) {
            fprintf(stderr, "[matrix] ✗ access_token invalid (whoami failed)\n");
            return NULL;
        }
    }
    fprintf(stderr, "[matrix] logged in as %s\n", ctx->user_id);
    fprintf(stderr, "[matrix] room: %s\n", ctx->room_id);

    /* Do initial sync if no since_token (to skip old history) */
    if (!ctx->since_token) {
        fprintf(stderr, "[matrix] performing initial sync...\n");
        cJSON *dummy = NULL;
        mx_api_sync(ctx, &dummy);
        if (dummy) cJSON_Delete(dummy);
        if (ctx->since_token) {
            fprintf(stderr, "[matrix] initial sync done, token: %.30s...\n",
                    ctx->since_token);
            /* Save since_token to config */
            mx_config_save(ctx);
        }
    }

    /* Set up inotify on outbox */
    char outbox_path[512];
    snprintf(outbox_path, sizeof(outbox_path), "%s/outbox", ctx->mailbox_dir);

    int ifd = inotify_init1(IN_NONBLOCK);
    int iwd = -1;
    if (ifd >= 0) {
        iwd = inotify_add_watch(ifd, outbox_path, IN_CREATE | IN_MOVED_TO);
        if (iwd < 0) {
            fprintf(stderr, "[matrix] inotify_add_watch failed: %s\n",
                    strerror(errno));
        }
    } else {
        fprintf(stderr, "[matrix] inotify_init failed: %s (will use polling)\n",
                strerror(errno));
    }

    /* Pending ask ID for routing replies as answers */
    char pending_ask_id[128] = {0};

    /* Process any existing outbox files */
    mx_scan_outbox(ctx);

    /* Send startup notification */
    mx_api_send_message(ctx, "🟢 Nash bot online", NULL);

    /* Main loop */
    while (!*ctx->shutdown) {
        /* ── Phase 1: /sync for new messages ──────────────────── */
        cJSON *events = NULL;
        int sync_rc = mx_api_sync(ctx, &events);

        if (sync_rc != 0) {
            fprintf(stderr, "[matrix] sync failed, retrying in %ds\n",
                    MX_RETRY_DELAY);
            sleep(MX_RETRY_DELAY);
            continue;
        }

        /* Process incoming messages */
        if (events) {
            int n = cJSON_GetArraySize(events);
            for (int i = 0; i < n; i++) {
                cJSON *ev = cJSON_GetArrayItem(events, i);

                /* Only process m.room.message events */
                cJSON *type = cJSON_GetObjectItem(ev, "type");
                if (!type || !type->valuestring ||
                    strcmp(type->valuestring, "m.room.message") != 0)
                    continue;

                /* Skip our own messages */
                cJSON *sender = cJSON_GetObjectItem(ev, "sender");
                if (!sender || !sender->valuestring) continue;
                if (ctx->user_id &&
                    strcmp(sender->valuestring, ctx->user_id) == 0)
                    continue;

                /* Extract message content */
                cJSON *content = cJSON_GetObjectItem(ev, "content");
                if (!content) continue;

                cJSON *msgtype = cJSON_GetObjectItem(content, "msgtype");
                if (!msgtype || !msgtype->valuestring) continue;

                /* Handle text messages */
                if (strcmp(msgtype->valuestring, "m.text") == 0) {
                    cJSON *body_j = cJSON_GetObjectItem(content, "body");
                    if (!body_j || !body_j->valuestring) continue;

                    const char *msg_text = body_j->valuestring;
                    fprintf(stderr, "[matrix] received from %s: %.100s%s\n",
                            sender->valuestring, msg_text,
                            strlen(msg_text) > 100 ? "..." : "");

                    /* Handle commands */
                    if (msg_text[0] == '!' || msg_text[0] == '/') {
                        const char *cmd = msg_text + 1;
                        if (strcmp(cmd, "new") == 0 ||
                            strcmp(cmd, "clear") == 0) {
                            mx_write_cmd_new(ctx->mailbox_dir);
                            mx_api_send_message(ctx,
                                "🔄 Starting new session — context cleared.",
                                NULL);
                            fprintf(stderr, "[matrix] !new → session reset\n");
                            continue;
                        }
                        if (strcmp(cmd, "help") == 0) {
                            mx_api_send_message(ctx,
                                "🤖 Nash Bot Commands\n\n"
                                "!new or !clear — Start a new session\n"
                                "!help — Show this help\n\n"
                                "Just type your query to interact with the agent.",
                                "<b>🤖 Nash Bot Commands</b><br/><br/>"
                                "<b>!new</b> or <b>!clear</b> — Start a new session<br/>"
                                "<b>!help</b> — Show this help<br/><br/>"
                                "Just type your query to interact with the agent.");
                            continue;
                        }
                    }

                    /* Check if this is a reply to a pending ask */
                    if (pending_ask_id[0]) {
                        fprintf(stderr, "[matrix] routing as answer to ask_%s\n",
                                pending_ask_id);
                        mx_write_answer(ctx->mailbox_dir, pending_ask_id,
                                       msg_text);
                        pending_ask_id[0] = 0;
                        mx_api_send_message(ctx, "✓ Answer received", NULL);
                        continue;
                    }

                    /* Check if this is a reply to a bot message
                     * (via m.relates_to / m.in_reply_to) */
                    int is_reply = 0;
                    cJSON *relates = cJSON_GetObjectItem(content, "m.relates_to");
                    if (relates) {
                        cJSON *in_reply = cJSON_GetObjectItem(relates, "m.in_reply_to");
                        if (in_reply) is_reply = 1;
                    }

                    if (!is_reply) {
                        /* New standalone message → reset session */
                        mx_write_cmd_new(ctx->mailbox_dir);
                        fprintf(stderr, "[matrix] new message → session reset\n");
                        usleep(100000);  /* 100ms for daemon to process cmd_new */
                    } else {
                        fprintf(stderr, "[matrix] reply → continuing session\n");
                    }

                    /* Create task */
                    char task_id[64];
                    struct timespec ts;
                    clock_gettime(CLOCK_REALTIME, &ts);
                    snprintf(task_id, sizeof(task_id), "mx%lx%04lx",
                             (long)ts.tv_sec, ts.tv_nsec / 100000L);

                    fprintf(stderr, "[matrix] creating task_%s\n", task_id);
                    mx_write_task(ctx->mailbox_dir, task_id, msg_text);
                    mx_api_send_message(ctx, is_reply
                        ? "⏳ Continuing..." : "⏳ Processing...", NULL);

                } else if (strcmp(msgtype->valuestring, "m.image") == 0) {
                    /* Image message → download and create image analysis task */
                    cJSON *img_url = cJSON_GetObjectItem(content, "url");
                    if (!img_url || !img_url->valuestring) {
                        mx_api_send_message(ctx,
                            "⚠️ Image has no URL.", NULL);
                        continue;
                    }

                    /* Extract caption from body field (Matrix uses body for alt text) */
                    cJSON *body_j = cJSON_GetObjectItem(content, "body");
                    const char *caption = NULL;
                    if (body_j && body_j->valuestring &&
                        body_j->valuestring[0] &&
                        strcmp(body_j->valuestring, "image") != 0 &&
                        strncmp(body_j->valuestring, "image.", 6) != 0) {
                        caption = body_j->valuestring;
                    }

                    fprintf(stderr, "[matrix] received image from %s: %s\n",
                            sender->valuestring, img_url->valuestring);

                    /* Check if this is a reply to a pending ask */
                    if (pending_ask_id[0]) {
                        fprintf(stderr,
                                "[matrix] routing image as answer to ask_%s\n",
                                pending_ask_id);
                        mx_write_answer(ctx->mailbox_dir, pending_ask_id,
                                       caption ? caption : "(image)");
                        pending_ask_id[0] = 0;
                        mx_api_send_message(ctx, "✓ Answer received", NULL);
                        continue;
                    }

                    /* Check if this is a reply */
                    int is_reply = 0;
                    cJSON *relates = cJSON_GetObjectItem(content,
                                                         "m.relates_to");
                    if (relates) {
                        cJSON *in_reply = cJSON_GetObjectItem(relates,
                                                              "m.in_reply_to");
                        if (in_reply) is_reply = 1;
                    }

                    if (!is_reply) {
                        mx_write_cmd_new(ctx->mailbox_dir);
                        fprintf(stderr,
                                "[matrix] new image message → session reset\n");
                        usleep(100000);  /* 100ms for daemon to process */
                    } else {
                        fprintf(stderr,
                                "[matrix] image reply → continuing session\n");
                    }

                    /* Download the image */
                    char image_path[1040];
                    if (mx_download_image(ctx, img_url->valuestring,
                                          image_path,
                                          sizeof(image_path)) == 0) {
                        /* Build task text for image analysis */
                        str_t task_text = str_new(1024);
                        if (caption && caption[0]) {
                            str_appendf(&task_text,
                                "Analyze this image using image_analyze tool "
                                "(path: %s): %s", image_path, caption);
                        } else {
                            str_appendf(&task_text,
                                "Analyze this image using image_analyze tool "
                                "(path: %s). Describe what you see in detail.",
                                image_path);
                        }

                        char task_id[64];
                        struct timespec ts;
                        clock_gettime(CLOCK_REALTIME, &ts);
                        snprintf(task_id, sizeof(task_id), "mx%lx%04lx",
                                 (long)ts.tv_sec, ts.tv_nsec / 100000L);

                        fprintf(stderr,
                                "[matrix] creating image task_%s\n", task_id);
                        mx_write_task(ctx->mailbox_dir, task_id,
                                     task_text.data);
                        str_free(&task_text);
                        mx_api_send_message(ctx,
                            "📷 Analyzing image...", NULL);
                    } else {
                        mx_api_send_message(ctx,
                            "⚠️ Failed to download image from Matrix server.",
                            NULL);
                    }
                }
            }
            cJSON_Delete(events);
        }

        if (*ctx->shutdown) break;

        /* ── Phase 2: Check outbox ────────────────────────────── */
        if (ifd >= 0) {
            char evbuf[4096]
                __attribute__((aligned(__alignof__(struct inotify_event))));
            ssize_t nread = read(ifd, evbuf, sizeof(evbuf));
            if (nread > 0) {
                usleep(50000);  /* 50ms for atomic writes to complete */
                char *ptr = evbuf;
                while (ptr < evbuf + nread) {
                    struct inotify_event *iev = (struct inotify_event *)ptr;
                    if (iev->len > 0 && iev->name[0] != '.') {
                        /* Track ask_* files for pending ask routing */
                        if (strncmp(iev->name, "ask_", 4) == 0) {
                            size_t nlen = strlen(iev->name);
                            if (nlen < sizeof(pending_ask_id)) {
                                snprintf(pending_ask_id, sizeof(pending_ask_id),
                                         "%s", iev->name + 4);
                            }
                        }
                        mx_process_outbox_file(ctx, iev->name);
                    }
                    ptr += sizeof(struct inotify_event) + iev->len;
                }
            }
        } else {
            mx_scan_outbox(ctx);
        }

        /* Periodically save since_token */
        static int save_counter = 0;
        if (++save_counter >= 60) {  /* every ~60 sync cycles ≈ 5 min */
            mx_config_save(ctx);
            save_counter = 0;
        }
    }

    /* Send offline notification */
    mx_api_send_message(ctx, "🔴 Nash bot going offline", NULL);

    /* Cleanup */
    if (iwd >= 0) inotify_rm_watch(ifd, iwd);
    if (ifd >= 0) close(ifd);

    /* Save final since_token */
    mx_config_save(ctx);

    fprintf(stderr, "[matrix] bridge thread stopped\n");
    return NULL;
}
