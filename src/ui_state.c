#include "ui_state.h"
#include "str.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Lifecycle ───────────────────────────────────────── */

ui_state_t *ui_state_new(const char *session_dir, store_t *store) {
    ui_state_t *ui = calloc(1, sizeof(*ui));
    if (!ui) return NULL;
    ui->session_dir = session_dir ? strdup(session_dir) : NULL;
    ui->store = store;
    ui->focus = FOCUS_QUERY;
    ui->status = STATUS_READY;
    ui->status_text = strdup("Ready");
    ui->input_cap = 4096;
    ui->input_buffer = calloc(1, ui->input_cap);
    ui->stream_cap = 8192;
    ui->stream_tokens = calloc(1, ui->stream_cap);
    pthread_mutex_init(&ui->mtx, NULL);
    ui->dirty = 1;
    return ui;
}

static void free_queries(ui_state_t *ui) {
    for (int i = 0; i < ui->query_count; i++) {
        free(ui->queries[i].query_text);
        free(ui->queries[i].result_preview);
    }
    free(ui->queries);
    ui->queries = NULL;
    ui->query_count = 0;
    ui->query_cap = 0;
}

static void free_steps(ui_state_t *ui) {
    for (int i = 0; i < ui->step_count; i++) {
        free(ui->steps[i].ref);
        free(ui->steps[i].tool);
        free(ui->steps[i].description);
    }
    free(ui->steps);
    ui->steps = NULL;
    ui->step_count = 0;
    ui->step_cap = 0;
}

void ui_state_free(ui_state_t *ui) {
    if (!ui) return;
    pthread_mutex_destroy(&ui->mtx);
    free_queries(ui);
    free_steps(ui);
    free(ui->detail_content);
    free(ui->status_text);
    free(ui->input_buffer);
    free(ui->stream_tokens);
    free(ui->session_dir);
    free(ui);
}

/* ── Navigation ──────────────────────────────────────── */

void ui_state_tab(ui_state_t *ui) {
    ui->focus = (ui->focus == FOCUS_JOURNAL) ? FOCUS_QUERY : FOCUS_JOURNAL;
    pthread_mutex_init(&ui->mtx, NULL);
    ui->dirty = 1;
}

void ui_state_up(ui_state_t *ui) {
    if (ui->focus == FOCUS_QUERY) return;  /* no up/down in query pane */

    switch (ui->view) {
    case VIEW_QUERIES:
        if (ui->selected_query > 0) ui->selected_query--;
        break;
    case VIEW_MANIFEST:
        if (ui->selected_step > 0) ui->selected_step--;
        break;
    case VIEW_DETAIL:
        if (ui->detail_scroll > 0) ui->detail_scroll--;
        break;
    }
    pthread_mutex_init(&ui->mtx, NULL);
    ui->dirty = 1;
}

void ui_state_down(ui_state_t *ui) {
    if (ui->focus == FOCUS_QUERY) return;

    switch (ui->view) {
    case VIEW_QUERIES:
        if (ui->selected_query < ui->query_count - 1) ui->selected_query++;
        break;
    case VIEW_MANIFEST:
        if (ui->selected_step < ui->step_count - 1) ui->selected_step++;
        break;
    case VIEW_DETAIL:
        if (ui->detail_scroll < ui->detail_lines - 1) ui->detail_scroll++;
        break;
    }
    pthread_mutex_init(&ui->mtx, NULL);
    ui->dirty = 1;
}

void ui_state_enter(ui_state_t *ui) {
    if (ui->focus == FOCUS_QUERY) {
        /* Submit is handled separately via ui_state_input_submit */
        return;
    }

    switch (ui->view) {
    case VIEW_QUERIES:
        if (ui->query_count > 0) {
            ui_state_load_manifest(ui, ui->selected_query);
            ui->view = VIEW_MANIFEST;
            ui->selected_step = 0;
        }
        break;
    case VIEW_MANIFEST:
        if (ui->step_count > 0) {
            ui_state_load_detail(ui, ui->selected_step);
            ui->view = VIEW_DETAIL;
            ui->detail_scroll = 0;
        }
        break;
    case VIEW_DETAIL:
        /* Already at deepest level — no-op */
        break;
    }
    pthread_mutex_init(&ui->mtx, NULL);
    ui->dirty = 1;
}

void ui_state_back(ui_state_t *ui) {
    if (ui->focus == FOCUS_QUERY) return;

    switch (ui->view) {
    case VIEW_QUERIES:
        /* Already at top level — no-op */
        break;
    case VIEW_MANIFEST:
        ui->view = VIEW_QUERIES;
        free_steps(ui);
        break;
    case VIEW_DETAIL:
        ui->view = VIEW_MANIFEST;
        free(ui->detail_content);
        ui->detail_content = NULL;
        ui->detail_lines = 0;
        break;
    }
    pthread_mutex_init(&ui->mtx, NULL);
    ui->dirty = 1;
}

void ui_state_page_up(ui_state_t *ui) {
    if (ui->focus == FOCUS_JOURNAL && ui->view == VIEW_DETAIL) {
        ui->detail_scroll -= 20;
        if (ui->detail_scroll < 0) ui->detail_scroll = 0;
    pthread_mutex_init(&ui->mtx, NULL);
        ui->dirty = 1;
    }
}

void ui_state_page_down(ui_state_t *ui) {
    if (ui->focus == FOCUS_JOURNAL && ui->view == VIEW_DETAIL) {
        ui->detail_scroll += 20;
        if (ui->detail_scroll > ui->detail_lines - 1)
            ui->detail_scroll = ui->detail_lines > 0 ? ui->detail_lines - 1 : 0;
    pthread_mutex_init(&ui->mtx, NULL);
        ui->dirty = 1;
    }
}

/* ── Input editing ───────────────────────────────────── */

void ui_state_input_char(ui_state_t *ui, int ch) {
    if (ui->input_len + 2 >= ui->input_cap) {
        ui->input_cap *= 2;
        ui->input_buffer = realloc(ui->input_buffer, ui->input_cap);
    }
    /* Insert at cursor */
    memmove(ui->input_buffer + ui->cursor_pos + 1,
            ui->input_buffer + ui->cursor_pos,
            ui->input_len - ui->cursor_pos + 1);
    ui->input_buffer[ui->cursor_pos] = (char)ch;
    ui->cursor_pos++;
    ui->input_len++;
    pthread_mutex_init(&ui->mtx, NULL);
    ui->dirty = 1;
}

void ui_state_input_backspace(ui_state_t *ui) {
    if (ui->cursor_pos > 0) {
        memmove(ui->input_buffer + ui->cursor_pos - 1,
                ui->input_buffer + ui->cursor_pos,
                ui->input_len - ui->cursor_pos + 1);
        ui->cursor_pos--;
        ui->input_len--;
    pthread_mutex_init(&ui->mtx, NULL);
        ui->dirty = 1;
    }
}

void ui_state_input_delete(ui_state_t *ui) {
    if (ui->cursor_pos < ui->input_len) {
        memmove(ui->input_buffer + ui->cursor_pos,
                ui->input_buffer + ui->cursor_pos + 1,
                ui->input_len - ui->cursor_pos);
        ui->input_len--;
    pthread_mutex_init(&ui->mtx, NULL);
        ui->dirty = 1;
    }
}

void ui_state_input_left(ui_state_t *ui) {
    pthread_mutex_init(&ui->mtx, NULL);
    if (ui->cursor_pos > 0) { ui->cursor_pos--; ui->dirty = 1; }
}

void ui_state_input_right(ui_state_t *ui) {
    pthread_mutex_init(&ui->mtx, NULL);
    if (ui->cursor_pos < ui->input_len) { ui->cursor_pos++; ui->dirty = 1; }
}

void ui_state_input_home(ui_state_t *ui) {
    pthread_mutex_init(&ui->mtx, NULL);
    ui->cursor_pos = 0; ui->dirty = 1;
}

void ui_state_input_end(ui_state_t *ui) {
    pthread_mutex_init(&ui->mtx, NULL);
    ui->cursor_pos = ui->input_len; ui->dirty = 1;
}

const char *ui_state_input_submit(ui_state_t *ui) {
    if (ui->input_len == 0) return NULL;
    /* Return pointer to the buffer content (caller should strdup if needed) */
    ui->input_buffer[ui->input_len] = '\0';
    return ui->input_buffer;
}

/* Reset input after submission */
static void input_clear(ui_state_t *ui) {
    ui->input_buffer[0] = '\0';
    ui->input_len = 0;
    ui->cursor_pos = 0;
    pthread_mutex_init(&ui->mtx, NULL);
    ui->dirty = 1;
}

/* ── Journal loading ─────────────────────────────────── */

static void add_query(ui_state_t *ui, const char *text, double ts, int loop) {
    if (ui->query_count >= ui->query_cap) {
        ui->query_cap = ui->query_cap ? ui->query_cap * 2 : 16;
        ui->queries = realloc(ui->queries, ui->query_cap * sizeof(ui_query_t));
    }
    ui_query_t *q = &ui->queries[ui->query_count++];
    memset(q, 0, sizeof(*q));
    q->query_text = strdup(text);
    q->timestamp = ts;
    q->react_loop = loop;
}

void ui_state_load_journal(ui_state_t *ui, journal_t *journal) {
    free_queries(ui);

    FILE *f = fopen(journal->path, "r");
    if (!f) return;

    char line[65536];
    int current_loop = -1;

    while (fgets(line, sizeof(line), f)) {
        cJSON *entry = cJSON_Parse(line);
        if (!entry) continue;

        const char *tool = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "tool"));
        int loop = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(entry, "react_loop"));
        const char *ts_str = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "ts"));
        double ts = ts_str ? atof(ts_str) : 0;

        if (tool && strcmp(tool, "query") == 0) {
            cJSON *params = cJSON_GetObjectItem(entry, "params");
            const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(params, "text"));
            if (text) {
                add_query(ui, text, ts, loop);
                current_loop = loop;
            }
        }

        /* Count steps and failures for the current query */
        if (tool && strcmp(tool, "query") != 0 && strcmp(tool, "system") != 0 &&
            loop == current_loop && ui->query_count > 0) {
            ui_query_t *q = &ui->queries[ui->query_count - 1];
            q->step_count++;
            cJSON *failed = cJSON_GetObjectItem(entry, "failed");
            if (failed && cJSON_IsTrue(failed)) q->failed_count++;

            /* Capture done result preview */
            if (strcmp(tool, "done") == 0) {
                cJSON *params = cJSON_GetObjectItem(entry, "params");
                const char *result = cJSON_GetStringValue(
                    cJSON_GetObjectItem(params, "result"));
                if (result) {
                    char preview[128];
                    snprintf(preview, sizeof(preview), "%.120s", result);
                    free(q->result_preview);
                    q->result_preview = strdup(preview);
                }
            }
        }

        cJSON_Delete(entry);
    }
    fclose(f);
    pthread_mutex_init(&ui->mtx, NULL);
    ui->dirty = 1;
}

void ui_state_load_manifest(ui_state_t *ui, int query_idx) {
    free_steps(ui);
    if (query_idx < 0 || query_idx >= ui->query_count) return;

    int target_loop = ui->queries[query_idx].react_loop;

    /* Re-read journal for this specific loop's steps */
    char jpath[4096];
    snprintf(jpath, sizeof(jpath), "%s/journal.jsonl",
             ui->session_dir ? ui->session_dir : ".");

    FILE *f = fopen(jpath, "r");
    if (!f) return;

    char line[65536];
    while (fgets(line, sizeof(line), f)) {
        cJSON *entry = cJSON_Parse(line);
        if (!entry) continue;

        int loop = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(entry, "react_loop"));
        const char *tool = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "tool"));

        if (loop != target_loop || !tool ||
            strcmp(tool, "system") == 0 || strcmp(tool, "query") == 0) {
            cJSON_Delete(entry);
            continue;
        }

        /* Add step entry */
        if (ui->step_count >= ui->step_cap) {
            ui->step_cap = ui->step_cap ? ui->step_cap * 2 : 32;
            ui->steps = realloc(ui->steps, ui->step_cap * sizeof(ui_step_t));
        }
        ui_step_t *s = &ui->steps[ui->step_count++];
        memset(s, 0, sizeof(*s));

        s->step = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(entry, "step"));
        s->tool = strdup(tool);
        s->size = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(entry, "size"));

        const char *ref = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "ref"));
        s->ref = ref ? strdup(ref) : strdup("?");

        cJSON *failed = cJSON_GetObjectItem(entry, "failed");
        s->failed = (failed && cJSON_IsTrue(failed));

        /* Extract key parameter for description */
        cJSON *params = cJSON_GetObjectItem(entry, "params");
        if (params) {
            const char *desc = NULL;
            cJSON *cmd = cJSON_GetObjectItem(params, "command");
            cJSON *path = cJSON_GetObjectItem(params, "path");
            cJSON *pattern = cJSON_GetObjectItem(params, "pattern");
            cJSON *query = cJSON_GetObjectItem(params, "query");
            cJSON *result = cJSON_GetObjectItem(params, "result");
            cJSON *url = cJSON_GetObjectItem(params, "url");
            cJSON *key = cJSON_GetObjectItem(params, "key");
            if (cmd && cmd->valuestring) desc = cmd->valuestring;
            else if (path && path->valuestring) desc = path->valuestring;
            else if (pattern && pattern->valuestring) desc = pattern->valuestring;
            else if (query && query->valuestring) desc = query->valuestring;
            else if (url && url->valuestring) desc = url->valuestring;
            else if (key && key->valuestring) desc = key->valuestring;
            else if (result && result->valuestring) desc = result->valuestring;
            if (desc) {
                char trunc[128];
                snprintf(trunc, sizeof(trunc), "%.120s", desc);
                s->description = strdup(trunc);
            }
        }
        if (!s->description) s->description = strdup("");

        cJSON_Delete(entry);
    }
    fclose(f);
    pthread_mutex_init(&ui->mtx, NULL);
    ui->dirty = 1;
}

void ui_state_load_detail(ui_state_t *ui, int step_idx) {
    free(ui->detail_content);
    ui->detail_content = NULL;
    ui->detail_lines = 0;

    if (step_idx < 0 || step_idx >= ui->step_count) return;

    const char *ref = ui->steps[step_idx].ref;
    if (!ref || ref[0] == '?') return;

    /* Read from store via session symlink */
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s",
             ui->session_dir ? ui->session_dir : ".", ref);

    FILE *f = fopen(path, "r");
    if (!f) return;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    /* Cap at 64KB for display */
    if (sz > 65536) sz = 65536;

    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);

    ui->detail_content = buf;

    /* Count lines */
    int lines = 1;
    for (size_t i = 0; i < n; i++)
        if (buf[i] == '\n') lines++;
    ui->detail_lines = lines;
    pthread_mutex_init(&ui->mtx, NULL);
    ui->dirty = 1;
}

/* ── Status updates ──────────────────────────────────── */

void ui_state_set_status(ui_state_t *ui, ui_status_t status, const char *text) {
    ui->status = status;
    free(ui->status_text);
    ui->status_text = text ? strdup(text) : strdup("");
    pthread_mutex_init(&ui->mtx, NULL);
    ui->dirty = 1;
}

/* ── React event handler ─────────────────────────────── */

void ui_state_on_event(const react_event_t *ev, void *userdata) {
    ui_state_t *ui = (ui_state_t *)userdata;
    if (!ui) return;

    switch (ev->type) {
    case REACT_EVENT_STEP_START: {
        char buf[128];
        snprintf(buf, sizeof(buf), "Running step %d/%d...",
                 ev->step, ev->max_steps);
        ui_state_set_status(ui, STATUS_RUNNING, buf);
        ui->current_step = ev->step;
        ui->max_steps = ev->max_steps;
        /* Clear streaming tokens for new step */
        ui->stream_tokens[0] = '\0';
        ui->stream_len = 0;
        break;
    }

    case REACT_EVENT_LLM_TOKEN:
        if (ev->token) {
            int tlen = (int)strlen(ev->token);
            if (ui->stream_len + tlen + 1 >= ui->stream_cap) {
                ui->stream_cap = (ui->stream_len + tlen + 1) * 2;
                ui->stream_tokens = realloc(ui->stream_tokens, ui->stream_cap);
            }
            memcpy(ui->stream_tokens + ui->stream_len, ev->token, tlen);
            ui->stream_len += tlen;
            ui->stream_tokens[ui->stream_len] = '\0';
        }
    pthread_mutex_init(&ui->mtx, NULL);
        ui->dirty = 1;
        break;

    case REACT_EVENT_STEP_COMPLETE:
        /* Step finished — update status */
        {
            char buf[256];
            char dur[32];
            fmt_duration(ev->step_elapsed, dur, sizeof(dur));
            snprintf(buf, sizeof(buf), "Step %d: %s %s (%s)",
                     ev->step,
                     ev->action ? ev->action : "?",
                     ev->description ? ev->description : "",
                     dur);
            ui_state_set_status(ui, STATUS_RUNNING, buf);
        }
        break;

    case REACT_EVENT_TOOL_OUTPUT:
        /* Tool result available — could auto-refresh manifest */
    pthread_mutex_init(&ui->mtx, NULL);
        ui->dirty = 1;
        break;

    case REACT_EVENT_ERROR:
    case REACT_EVENT_WARNING:
        if (ev->message) {
            ui_state_set_status(ui,
                ev->type == REACT_EVENT_ERROR ? STATUS_ERROR : STATUS_RUNNING,
                ev->message);
        }
        break;

    case REACT_EVENT_DONE:
        ui_state_set_status(ui, STATUS_DONE,
            ev->result ? ev->result : "Task complete");
        /* Clear streaming */
        ui->stream_tokens[0] = '\0';
        ui->stream_len = 0;
        break;
    }
}

/* ── Serialization (for web frontend) ────────────────── */

char *ui_state_to_json(const ui_state_t *ui) {
    cJSON *root = cJSON_CreateObject();

    /* Focus and status */
    cJSON_AddStringToObject(root, "focus",
        ui->focus == FOCUS_JOURNAL ? "journal" : "query");
    cJSON_AddStringToObject(root, "status",
        ui->status == STATUS_READY ? "ready" :
        ui->status == STATUS_RUNNING ? "running" :
        ui->status == STATUS_AWAITING_INPUT ? "awaiting" :
        ui->status == STATUS_DONE ? "done" : "error");
    if (ui->status_text)
        cJSON_AddStringToObject(root, "status_text", ui->status_text);

    /* View level */
    cJSON_AddStringToObject(root, "view",
        ui->view == VIEW_QUERIES ? "queries" :
        ui->view == VIEW_MANIFEST ? "manifest" : "detail");

    /* Queries */
    cJSON *queries = cJSON_CreateArray();
    for (int i = 0; i < ui->query_count; i++) {
        cJSON *q = cJSON_CreateObject();
        cJSON_AddStringToObject(q, "text", ui->queries[i].query_text);
        cJSON_AddNumberToObject(q, "timestamp", ui->queries[i].timestamp);
        cJSON_AddNumberToObject(q, "react_loop", ui->queries[i].react_loop);
        cJSON_AddNumberToObject(q, "steps", ui->queries[i].step_count);
        cJSON_AddNumberToObject(q, "failed", ui->queries[i].failed_count);
        if (ui->queries[i].result_preview)
            cJSON_AddStringToObject(q, "result", ui->queries[i].result_preview);
        cJSON_AddBoolToObject(q, "selected", i == ui->selected_query);
        cJSON_AddItemToArray(queries, q);
    }
    cJSON_AddItemToObject(root, "queries", queries);

    /* Steps (if in manifest view) */
    if (ui->view == VIEW_MANIFEST || ui->view == VIEW_DETAIL) {
        cJSON *steps = cJSON_CreateArray();
        for (int i = 0; i < ui->step_count; i++) {
            cJSON *s = cJSON_CreateObject();
            cJSON_AddStringToObject(s, "ref", ui->steps[i].ref);
            cJSON_AddStringToObject(s, "tool", ui->steps[i].tool);
            cJSON_AddStringToObject(s, "desc", ui->steps[i].description);
            cJSON_AddBoolToObject(s, "failed", ui->steps[i].failed);
            cJSON_AddNumberToObject(s, "size", ui->steps[i].size);
            cJSON_AddNumberToObject(s, "step", ui->steps[i].step);
            cJSON_AddBoolToObject(s, "selected", i == ui->selected_step);
            cJSON_AddItemToArray(steps, s);
        }
        cJSON_AddItemToObject(root, "steps", steps);
    }

    /* Detail content (if in detail view) */
    if (ui->view == VIEW_DETAIL && ui->detail_content) {
        /* Send only visible portion (scroll window) */
        cJSON_AddNumberToObject(root, "detail_scroll", ui->detail_scroll);
        cJSON_AddNumberToObject(root, "detail_lines", ui->detail_lines);
        /* Truncate for JSON — send first 8KB */
        char trunc[8192];
        snprintf(trunc, sizeof(trunc), "%.8000s", ui->detail_content);
        cJSON_AddStringToObject(root, "detail", trunc);
    }

    /* Input */
    if (ui->input_buffer)
        cJSON_AddStringToObject(root, "input", ui->input_buffer);
    cJSON_AddNumberToObject(root, "cursor", ui->cursor_pos);

    /* Streaming */
    if (ui->stream_len > 0)
        cJSON_AddStringToObject(root, "streaming", ui->stream_tokens);
    cJSON_AddNumberToObject(root, "current_step", ui->current_step);
    cJSON_AddNumberToObject(root, "max_steps", ui->max_steps);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

/* Add a new query entry to the journal pane */
void ui_state_add_query(ui_state_t *ui, const char *query_text) {
    if (!ui || !query_text) return;
    if (ui->query_count >= ui->query_cap) {
        ui->query_cap = ui->query_cap ? ui->query_cap * 2 : 16;
        ui->queries = realloc(ui->queries, ui->query_cap * sizeof(ui_query_t));
    }
    ui_query_t *q = &ui->queries[ui->query_count++];
    memset(q, 0, sizeof(*q));
    q->query_text = strdup(query_text);
    struct timespec tp;
    clock_gettime(CLOCK_REALTIME, &tp);
    q->timestamp = (double)tp.tv_sec + (double)tp.tv_nsec / 1e9;
    q->react_loop = ui->query_count - 1;
    ui->selected_query = ui->query_count - 1;
    pthread_mutex_init(&ui->mtx, NULL);
    ui->dirty = 1;
}

/* Set the banner text (shown in main pane on startup) */
void ui_state_set_banner(ui_state_t *ui, const char *banner) {
    if (!ui) return;
    free(ui->banner);
    ui->banner = banner ? strdup(banner) : NULL;
    pthread_mutex_init(&ui->mtx, NULL);
    ui->dirty = 1;
}
