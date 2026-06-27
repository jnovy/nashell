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
#include "mailbox.h"
#include "nash_limits.h"
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
                          const char *text, const char *workspace,
                          const char *route_token);
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

/* Room-aware send variants (for multi-room workspace routing) */
static int  mx_api_send_to_room(matrix_ctx_t *ctx, const char *room_id,
                                const char *text, const char *html);
static int  mx_api_send_markdown_to(matrix_ctx_t *ctx, const char *room_id,
                                    const char *md_text);
static int  mx_api_send_image_to(matrix_ctx_t *ctx, const char *room_id,
                                 const char *mxc_url, const char *filename,
                                 const char *mimetype, size_t filesize);

/* Room-to-workspace mapping helpers */
static const char *mx_workspace_for_room(matrix_ctx_t *ctx, const char *room_id);
static void mx_route_map_add(matrix_ctx_t *ctx, const char *task_id,
                             const char *room_id);
static const char *mx_route_map_lookup(matrix_ctx_t *ctx, const char *task_id);

/* Defined in telegram.c — shared markdown→HTML converter */
extern char *md_to_html(const char *md);
/* Defined in telegram.c — check if markdown contains tables */
extern int md_has_table(const char *md);
/* Defined in telegram.c — convert markdown tables to bullet-point lists */
extern char *md_tables_to_bullets(const char *md);


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


/* ── Room-to-workspace mapping helpers ───────────────────── */

/* Look up workspace name for a given room_id from room_map config */
static const char *mx_workspace_for_room(matrix_ctx_t *ctx, const char *room_id) {
    if (!room_id) return NULL;
    for (int i = 0; i < ctx->room_map_count; i++) {
        if (strcmp(ctx->room_map[i].room_id, room_id) == 0)
            return ctx->room_map[i].workspace;
    }
    return NULL;
}

/* Record task_id -> room_id mapping for reply routing (circular buffer) */
static void mx_route_map_add(matrix_ctx_t *ctx, const char *task_id,
                             const char *room_id) {
    int idx = ctx->route_map_next;
    snprintf(ctx->route_map[idx].task_id, sizeof(ctx->route_map[idx].task_id),
             "%s", task_id);
    snprintf(ctx->route_map[idx].room_id, sizeof(ctx->route_map[idx].room_id),
             "%s", room_id);
    ctx->route_map_next = (idx + 1) % MX_MAX_ROUTE_MAP;
}

/* Look up source room_id for a given task_id */
__attribute__((unused))
static const char *mx_route_map_lookup(matrix_ctx_t *ctx, const char *task_id) {
    for (int i = 0; i < MX_MAX_ROUTE_MAP; i++) {
        if (ctx->route_map[i].task_id[0] &&
            strcmp(ctx->route_map[i].task_id, task_id) == 0)
            return ctx->route_map[i].room_id;
    }
    return NULL;
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
    for (int i = 0; i < ctx->room_map_count; i++) {
        free(ctx->room_map[i].room_id);
        free(ctx->room_map[i].workspace);
    }
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

        /* Parse [matrix.rooms] — room-to-workspace mapping */
        toml_table_t *rooms_tbl = toml_table_in(mx, "rooms");
        if (rooms_tbl) {
            ctx->room_map_count = 0;
            for (int ri = 0; ; ri++) {
                const char *key = toml_key_in(rooms_tbl, ri);
                if (!key) break;
                if (ctx->room_map_count >= MX_MAX_ROOM_MAP) break;
                toml_datum_t rd = toml_string_in(rooms_tbl, key);
                if (rd.ok) {
                    int idx = ctx->room_map_count++;
                    ctx->room_map[idx].room_id = strdup(key);
                    ctx->room_map[idx].workspace = rd.u.s;
                    fprintf(stderr, "[matrix] room %s -> workspace '%s'\n",
                            ctx->room_map[idx].room_id,
                            ctx->room_map[idx].workspace);
                }
            }
        }
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

    /* Extract timeline events from ALL joined rooms.
     * Attach "_room_id" to each event so the message handler can
     * route to the correct workspace. */
    {
        cJSON *rooms = cJSON_GetObjectItem(root, "rooms");
        cJSON *join = rooms ? cJSON_GetObjectItem(rooms, "join") : NULL;
        if (join) {
            cJSON *all_events = NULL;
            cJSON *room_node = NULL;
            cJSON_ArrayForEach(room_node, join) {
                /* room_node->string is the room_id (object key) */
                if (!room_node->string) continue;

                /* Only process rooms we care about:
                 * the default room OR any room in room_map */
                int dominated = 0;
                if (ctx->room_id &&
                    strcmp(room_node->string, ctx->room_id) == 0)
                    dominated = 1;
                if (!dominated) {
                    for (int ri = 0; ri < ctx->room_map_count; ri++) {
                        if (strcmp(room_node->string,
                                  ctx->room_map[ri].room_id) == 0) {
                            dominated = 1;
                            break;
                        }
                    }
                }
                if (!dominated) continue;

                cJSON *timeline = cJSON_GetObjectItem(room_node, "timeline");
                cJSON *events = timeline
                    ? cJSON_GetObjectItem(timeline, "events") : NULL;
                if (!events || !cJSON_IsArray(events) ||
                    cJSON_GetArraySize(events) == 0)
                    continue;

                /* Tag each event with the source room_id */
                cJSON *ev = NULL;
                cJSON_ArrayForEach(ev, events) {
                    cJSON_AddStringToObject(ev, "_room_id",
                                            room_node->string);
                }

                /* Merge into combined array */
                if (!all_events) {
                    all_events = cJSON_DetachItemFromObject(timeline,
                                                           "events");
                } else {
                    /* Move each event from this array to all_events */
                    while (cJSON_GetArraySize(events) > 0) {
                        cJSON *item = cJSON_DetachItemFromArray(events, 0);
                        cJSON_AddItemToArray(all_events, item);
                    }
                }
            }
            *out_events = all_events;
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
static int mx_api_send_message_inner(matrix_ctx_t *ctx, const char *room_id,
                                     const char *text, const char *html,
                                     int retries_left) {
    char *enc_room = url_encode(room_id);
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
                        return mx_api_send_message_inner(ctx, room_id,
                                                        text, html,
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
    return mx_api_send_message_inner(ctx, ctx->room_id, text, html, 3);
}

/* Send a message to a specific room (for multi-room routing) */
static int mx_api_send_to_room(matrix_ctx_t *ctx, const char *room_id,
                               const char *text, const char *html) {
    return mx_api_send_message_inner(ctx, room_id ? room_id : ctx->room_id,
                                     text, html, 3);
}

/*
 * Check if the content inside a <pre> block is an ASCII table
 * (generated by md_to_html for markdown tables).
 *
 * ASCII tables look like:
 *   | Header1 | Header2 |\n
 *   +--------+--------+\n
 *   | val1    | val2    |\n
 *
 * Code blocks use <pre><code> so they won't match (content starts with <code>).
 */
static int mx_is_pre_table(const char *content, size_t content_len) {
    if (content_len == 0) return 0;
    /* Skip leading newline */
    size_t i = 0;
    if (content[i] == '\n') i++;
    if (i >= content_len) return 0;
    /* Must start with | or + (first row of ASCII table) */
    return (content[i] == '|' || content[i] == '+');
}

/*
 * Convert ASCII table content (from inside <pre>...</pre>) to proper
 * HTML <table> markup for Matrix.
 *
 * Input format (the content between <pre>\n and </pre>):
 *   | Header1 | Header2 |\n
 *   +---------+---------+\n
 *   | val1    | val2    |\n
 *
 * Output: <table><thead><tr><th>Header1</th>...</tr></thead>
 *         <tbody><tr><td>val1</td>...</tr></tbody></table>
 *
 * The first data row (before the separator) becomes <thead>.
 * Separator rows (+---+---+) are skipped.
 * Remaining data rows go into <tbody>.
 */
static void mx_pre_table_to_html(str_t *out, const char *content, size_t content_len) {
    #define MX_TBL_MAX_LINES 256
    /* Collect lines */
    const char *lines[MX_TBL_MAX_LINES];
    size_t line_lens[MX_TBL_MAX_LINES];
    int nlines = 0;

    size_t i = 0;
    /* Skip leading newline */
    if (i < content_len && content[i] == '\n') i++;

    while (i < content_len && nlines < MX_TBL_MAX_LINES) {
        lines[nlines] = &content[i];
        size_t ls = i;
        while (i < content_len && content[i] != '\n') i++;
        line_lens[nlines] = i - ls;
        if (i < content_len && content[i] == '\n') i++;
        /* Skip empty trailing lines */
        if (line_lens[nlines] == 0) continue;
        nlines++;
    }

    if (nlines == 0) return;

    /* Classify lines: separator (+---) vs data (| ...) */
    int is_sep[MX_TBL_MAX_LINES];
    for (int r = 0; r < nlines; r++) {
        const char *line = lines[r];
        size_t ll = line_lens[r];
        /* Skip leading whitespace */
        size_t p = 0;
        while (p < ll && (line[p] == ' ' || line[p] == '\t')) p++;
        is_sep[r] = (p < ll && line[p] == '+');
    }

    /* Find the first separator — rows before it are headers */
    int first_sep = -1;
    for (int r = 0; r < nlines; r++) {
        if (is_sep[r]) { first_sep = r; break; }
    }

    str_append_cstr(out, "<table>\n");

    int in_thead = 0, in_tbody = 0;

    for (int r = 0; r < nlines; r++) {
        if (is_sep[r]) {
            /* Separator: close thead if open, ensure tbody opens next */
            if (in_thead) {
                str_append_cstr(out, "</thead>\n");
                in_thead = 0;
            }
            continue;
        }

        /* Determine if this is a header or body row */
        int is_header = (first_sep > 0 && r < first_sep);

        /* Open appropriate section */
        if (is_header && !in_thead) {
            str_append_cstr(out, "<thead>\n");
            in_thead = 1;
        } else if (!is_header && !in_tbody) {
            if (in_thead) {
                str_append_cstr(out, "</thead>\n");
                in_thead = 0;
            }
            str_append_cstr(out, "<tbody>\n");
            in_tbody = 1;
        }

        const char *tag = is_header ? "th" : "td";
        const char *line = lines[r];
        size_t ll = line_lens[r];

        str_append_cstr(out, "<tr>");

        /* Parse | delimited cells */
        size_t p = 0;
        while (p < ll && (line[p] == ' ' || line[p] == '\t')) p++;
        if (p < ll && line[p] == '|') p++;  /* skip leading | */

        while (p < ll) {
            /* Find next | */
            size_t cs = p;
            while (p < ll && line[p] != '|') p++;
            size_t ce = p;
            if (p < ll && line[p] == '|') p++;  /* skip | */

            /* Trim whitespace */
            while (cs < ce && (line[cs] == ' ' || line[cs] == '\t')) cs++;
            while (ce > cs && (line[ce-1] == ' ' || line[ce-1] == '\t')) ce--;

            /* Skip if this is just the trailing | with nothing */
            if (cs == ce && p >= ll) break;

            str_appendf(out, "<%s>", tag);
            if (ce > cs) {
                str_append(out, &line[cs], ce - cs);
            }
            str_appendf(out, "</%s>", tag);
        }

        str_append_cstr(out, "</tr>\n");
    }

    /* Close any open section */
    if (in_thead) str_append_cstr(out, "</thead>\n");
    if (in_tbody) str_append_cstr(out, "</tbody>\n");

    str_append_cstr(out, "</table>\n");
    #undef MX_TBL_MAX_LINES
}

/*
 * Post-process md_to_html() output for Matrix.
 *
 * md_to_html() was written for Telegram which treats \n as line breaks in
 * HTML mode.  Matrix follows standard HTML rules where bare \n is collapsed
 * to whitespace.  This function:
 *   - Converts ASCII tables in <pre> blocks to proper <table> HTML
 *   - Converts \n to <br/>\n outside of <pre>…</pre> blocks
 *   - Upgrades <b>heading</b>\n (Telegram-style headings) to proper
 *     <h3>heading</h3>\n for visual hierarchy in Matrix clients
 *
 * Returns heap-allocated string.  Caller frees.
 */
static char *mx_html_fixup(const char *html) {
    if (!html) return strdup("");
    size_t len = strlen(html);
    str_t out = str_new(len + len / 4 + 64);
    int in_pre = 0;
    size_t i = 0;

    while (i < len) {
        /* Detect <pre> opening tag */
        if (!in_pre && i + 4 < len &&
            html[i] == '<' && html[i+1] == 'p' && html[i+2] == 'r' &&
            html[i+3] == 'e' && (html[i+4] == '>' || html[i+4] == ' ')) {
            /* Find the end of the opening tag */
            size_t tag_end = i + 4;
            while (tag_end < len && html[tag_end] != '>') tag_end++;
            if (tag_end < len) tag_end++;  /* skip '>' */

            /* Find matching </pre> */
            const char *close = strstr(&html[tag_end], "</pre>");
            if (close) {
                size_t content_start = tag_end;
                size_t content_len = (size_t)(close - &html[tag_end]);

                /* Check if this is an ASCII table <pre> (not <pre><code>) */
                if (content_len > 0 &&
                    !(content_len >= 6 && strncmp(&html[content_start], "<code>", 6) == 0) &&
                    mx_is_pre_table(&html[content_start], content_len)) {
                    /* Convert ASCII table to proper HTML <table> */
                    mx_pre_table_to_html(&out, &html[content_start], content_len);
                    i = (size_t)(close - html) + 6;  /* skip past </pre> */
                    if (i < len && html[i] == '\n') i++;  /* skip trailing \n */
                    continue;
                }
            }

            /* Not a table — enter pre mode as before */
            in_pre = 1;
            str_append(&out, &html[i], 1);
            i++;
            continue;
        }
        if (in_pre && i + 5 < len &&
            html[i] == '<' && html[i+1] == '/' && html[i+2] == 'p' &&
            html[i+3] == 'r' && html[i+4] == 'e' && html[i+5] == '>') {
            in_pre = 0;
            str_append_cstr(&out, "</pre>");
            i += 6;
            continue;
        }

        if (!in_pre) {
            /* Upgrade Telegram-style bold headings to proper <h3>.
             * Pattern: <b>text</b> followed by \n (or end).
             * Only match when at start of output or after a newline/br. */
            if (html[i] == '<' && html[i+1] == 'b' && html[i+2] == '>' &&
                (i == 0 || html[i-1] == '\n' || (i >= 5 &&
                 html[i-5] == '<' && html[i-4] == 'b' && html[i-3] == 'r' &&
                 html[i-2] == '/' && html[i-1] == '>'))) {
                /* Find closing </b> */
                const char *close = strstr(&html[i+3], "</b>");
                if (close) {
                    size_t text_len = (size_t)(close - &html[i+3]);
                    str_append_cstr(&out, "<h3>");
                    str_append(&out, &html[i+3], text_len);
                    str_append_cstr(&out, "</h3>");
                    i = (size_t)(close - html) + 4;  /* skip past </b> */
                    /* Skip trailing \n — the <h3> block element handles it */
                    if (i < len && html[i] == '\n') i++;
                    continue;
                }
            }

            /* Convert \n to <br/>\n */
            if (html[i] == '\n') {
                str_append_cstr(&out, "<br/>\n");
                i++;
                continue;
            }
        }

        str_append(&out, &html[i], 1);
        i++;
    }

    return str_steal(&out);
}

/* Send markdown as an HTML-formatted Matrix message (default room) */
__attribute__((unused))
static int mx_api_send_markdown(matrix_ctx_t *ctx, const char *md_text) {
    /* Convert markdown to HTML, then fix up for Matrix */
    char *tg_html = md_to_html(md_text);
    char *html = mx_html_fixup(tg_html);
    free(tg_html);
    int rc = mx_api_send_message(ctx, md_text, html);
    free(html);
    return rc;
}

/* Send markdown to a specific room (for multi-room routing) */
static int mx_api_send_markdown_to(matrix_ctx_t *ctx, const char *room_id,
                                   const char *md_text) {
    char *tg_html = md_to_html(md_text);
    char *html = mx_html_fixup(tg_html);
    free(tg_html);
    int rc = mx_api_send_to_room(ctx, room_id, md_text, html);
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
static int mx_api_send_image_to(matrix_ctx_t *ctx, const char *room_id,
                                const char *mxc_url, const char *filename,
                                const char *mimetype, size_t filesize) {
    char *enc_room = url_encode(room_id);
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

/* Backward-compatible wrapper: send image to default room */
__attribute__((unused))
static int mx_api_send_image(matrix_ctx_t *ctx, const char *mxc_url,
                             const char *filename, const char *mimetype,
                             size_t filesize) {
    return mx_api_send_image_to(ctx, ctx->room_id, mxc_url, filename,
                                mimetype, filesize);
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

    /* Note: E2EE (m.room.encryption) is NOT enabled here because the bot
     * uses plain Matrix client API without Olm/Megolm key management.
     * Enabling room encryption would prevent the bot from reading or
     * sending messages.  The "Not encrypted" warning in Element is
     * expected for bot-operated rooms. */

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
                          const char *text, const char *workspace,
                          const char *route_token) {
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
    if (route_token && route_token[0])
        fprintf(f, "X-Route-Token: %s\n", route_token);
    if ((workspace && workspace[0]) || (route_token && route_token[0]))
        fputs("---\n", f);
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

    /* Parse and strip any route token headers from outbox file content.
     * Result files from mailbox_write_result_routed() may have:
     *   X-Route-Token: <room_id>
     *   ---
     *   <actual content>
     */
    char *route_token = NULL;
    char *actual_content = mailbox_parse_headers(content, NULL, &route_token);

    /* Route to correct room: use route_token if present, else default room */
    const char *target_room = (route_token && route_token[0])
                              ? route_token : ctx->room_id;

    if (strncmp(filename, "result_", 7) == 0) {
        /* Task result -> convert any markdown tables to bullet-point lists
         * for inline display, then send as formatted HTML message. */
        char *display = md_has_table(actual_content)
                        ? md_tables_to_bullets(actual_content) : NULL;
        const char *text = display ? display : actual_content;
        mx_api_send_markdown_to(ctx, target_room, text);
        free(display);
    } else if (strncmp(filename, "ask_", 4) == 0) {
        /* user_ask question -> send with prompt */
        str_t msg = str_new(strlen(actual_content) + 64);
        str_append_cstr(&msg, "\xe2\x9d\x93 ");
        str_append_cstr(&msg, actual_content);
        str_append_cstr(&msg, "\n\n_(Reply to answer)_");

        str_t html = str_new(strlen(actual_content) + 128);
        str_append_cstr(&html, "\xe2\x9d\x93 ");
        char *html_content = md_to_html(actual_content);
        str_append_cstr(&html, html_content);
        free(html_content);
        str_append_cstr(&html, "<br/><br/><i>(Reply to answer)</i>");

        mx_api_send_to_room(ctx, target_room, msg.data, html.data);
        str_free(&msg);
        str_free(&html);
    } else if (strncmp(filename, "status_", 7) == 0) {
        /* Status notification.
         * Suppress [done] notifications -- the result itself is already
         * sent via result_* so this would just duplicate the "completed"
         * message in the chat. */
        if (strncmp(actual_content, "[done]", 6) == 0) {
            free(route_token);
            free(content);
            return;
        }
        str_t msg = str_new(strlen(actual_content) + 16);
        str_append_cstr(&msg, "\xf0\x9f\x93\x8b ");
        str_append_cstr(&msg, actual_content);
        mx_api_send_to_room(ctx, target_room, msg.data, NULL);
        str_free(&msg);
    } else if (strncmp(filename, "image_", 6) == 0) {
        /* Image file -> upload to Matrix and send as m.image.
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

                mx_api_send_image_to(ctx, target_room, mxc_url, basename,
                                     mimetype, fsize);

                /* Send caption as a follow-up text message if present */
                if (caption && caption[0]) {
                    mx_api_send_markdown_to(ctx, target_room, caption);
                }

                fprintf(stderr, "[matrix] sent image: %s\n", basename);
            } else {
                fprintf(stderr, "[matrix] failed to upload image: %s\n",
                        img_path);
                mx_api_send_to_room(ctx, target_room,
                    "\xe2\x9a\xa0\xef\xb8\x8f Failed to upload image to Matrix server.", NULL);
            }
        }
    }

    free(route_token);
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
    fprintf(stderr, "[matrix] default room: %s\n", ctx->room_id);
    if (ctx->room_map_count > 0) {
        fprintf(stderr, "[matrix] multi-room mode: %d workspace mappings\n",
                ctx->room_map_count);
    }

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

    /* Pending ask ID and room for routing replies as answers */
    char pending_ask_id[128] = {0};
    char pending_ask_room[256] = {0};

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

                /* Extract source room_id (added by mx_api_sync) */
                cJSON *ev_room_j = cJSON_GetObjectItem(ev, "_room_id");
                const char *ev_room = (ev_room_j && ev_room_j->valuestring)
                                      ? ev_room_j->valuestring : ctx->room_id;
                const char *ev_workspace = mx_workspace_for_room(ctx, ev_room);

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
                    fprintf(stderr, "[matrix] received from %s in %s: %.100s%s\n",
                            sender->valuestring, ev_room, msg_text,
                            strlen(msg_text) > 100 ? "..." : "");

                    /* Handle commands */
                    if (msg_text[0] == '!' || msg_text[0] == '/') {
                        const char *cmd = msg_text + 1;
                        if (strcmp(cmd, "new") == 0 ||
                            strcmp(cmd, "clear") == 0) {
                            mx_write_cmd_new(ctx->mailbox_dir);
                            mx_api_send_to_room(ctx, ev_room,
                                "\xf0\x9f\x94\x84 Starting new session -- context cleared.",
                                NULL);
                            fprintf(stderr, "[matrix] !new -> session reset\n");
                            continue;
                        }
                        if (strcmp(cmd, "help") == 0) {
                            mx_api_send_to_room(ctx, ev_room,
                                "\xf0\x9f\xa4\x96 Nash Bot Commands\n\n"
                                "!new or !clear -- Start a new session\n"
                                "!help -- Show this help\n\n"
                                "Just type your query to interact with the agent.",
                                "<b>\xf0\x9f\xa4\x96 Nash Bot Commands</b><br/><br/>"
                                "<b>!new</b> or <b>!clear</b> -- Start a new session<br/>"
                                "<b>!help</b> -- Show this help<br/><br/>"
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
                        mx_api_send_to_room(ctx,
                            pending_ask_room[0] ? pending_ask_room : ev_room,
                            "\xe2\x9c\x93 Answer received", NULL);
                        pending_ask_room[0] = 0;
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
                        /* New standalone message -> reset session */
                        mx_write_cmd_new(ctx->mailbox_dir);
                        fprintf(stderr, "[matrix] new message -> session reset\n");
                        usleep(100000);  /* 100ms for daemon to process cmd_new */
                    } else {
                        fprintf(stderr, "[matrix] reply -> continuing session\n");
                    }

                    /* Create task */
                    char task_id[64];
                    struct timespec ts;
                    clock_gettime(CLOCK_REALTIME, &ts);
                    snprintf(task_id, sizeof(task_id), "mx%lx%04lx",
                             (long)ts.tv_sec, ts.tv_nsec / 100000L);

                    fprintf(stderr, "[matrix] creating task_%s (room=%s ws=%s)\n",
                            task_id, ev_room,
                            ev_workspace ? ev_workspace : "(default)");
                    mx_write_task(ctx->mailbox_dir, task_id, msg_text,
                                  ev_workspace, ev_room);
                    mx_route_map_add(ctx, task_id, ev_room);
                    mx_api_send_to_room(ctx, ev_room, is_reply
                        ? "\xe2\x8f\xb3 Continuing..." : "\xe2\x8f\xb3 Processing...", NULL);

                } else if (strcmp(msgtype->valuestring, "m.image") == 0) {
                    /* Image message -> download and create image analysis task */
                    cJSON *img_url = cJSON_GetObjectItem(content, "url");
                    if (!img_url || !img_url->valuestring) {
                        mx_api_send_to_room(ctx, ev_room,
                            "\xe2\x9a\xa0\xef\xb8\x8f Image has no URL.", NULL);
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
                        mx_api_send_to_room(ctx,
                            pending_ask_room[0] ? pending_ask_room : ev_room,
                            "\xe2\x9c\x93 Answer received", NULL);
                        pending_ask_room[0] = 0;
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
                                "[matrix] new image message -> session reset\n");
                        usleep(100000);  /* 100ms for daemon to process */
                    } else {
                        fprintf(stderr,
                                "[matrix] image reply -> continuing session\n");
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
                                "[matrix] creating image task_%s (room=%s ws=%s)\n",
                                task_id, ev_room,
                                ev_workspace ? ev_workspace : "(default)");
                        mx_write_task(ctx->mailbox_dir, task_id,
                                     task_text.data, ev_workspace, ev_room);
                        mx_route_map_add(ctx, task_id, ev_room);
                        str_free(&task_text);
                        mx_api_send_to_room(ctx, ev_room,
                            "\xf0\x9f\x93\xb7 Analyzing image...", NULL);
                    } else {
                        mx_api_send_to_room(ctx, ev_room,
                            "\xe2\x9a\xa0\xef\xb8\x8f Failed to download image from Matrix server.",
                            NULL);
                    }
                }
            }
            cJSON_Delete(events);
        }

        if (*ctx->shutdown) break;

        /* ── Phase 2: Check outbox ────────────────────────────── */
        if (ifd >= 0) {
            char evbuf[NASH_PATH_MAX]
                __attribute__((aligned(__alignof__(struct inotify_event))));
            ssize_t nread = read(ifd, evbuf, sizeof(evbuf));
            if (nread > 0) {
                usleep(50000);  /* 50ms for atomic writes to complete */
                char *ptr = evbuf;
                while (ptr < evbuf + nread) {
                    struct inotify_event *iev = (struct inotify_event *)ptr;
                    if (iev->len > 0 && iev->name[0] != '.') {
                        /* Track ask_* files for pending ask routing.
                         * Read route_token from ask file to know which
                         * room to send the answer acknowledgment to. */
                        if (strncmp(iev->name, "ask_", 4) == 0) {
                            size_t nlen = strlen(iev->name);
                            if (nlen < sizeof(pending_ask_id)) {
                                snprintf(pending_ask_id, sizeof(pending_ask_id),
                                         "%s", iev->name + 4);
                            }
                            /* Read route_token from ask file before
                             * it gets unlinked by process_outbox */
                            char askpath[512];
                            snprintf(askpath, sizeof(askpath),
                                     "%s/outbox/%s", ctx->mailbox_dir,
                                     iev->name);
                            char *ask_data = slurp_file(askpath, NULL);
                            if (ask_data) {
                                char *ask_rt = NULL;
                                mailbox_parse_headers(ask_data, NULL, &ask_rt);
                                if (ask_rt && ask_rt[0]) {
                                    snprintf(pending_ask_room,
                                             sizeof(pending_ask_room),
                                             "%s", ask_rt);
                                }
                                free(ask_rt);
                                free(ask_data);
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
