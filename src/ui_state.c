#include "ui_state.h"
#include "str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ── Create / Free ─────────────────────────────────────── */

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
    ui->selected_step = -1;  /* -1 = cursor on query, not on a step */
    ui->dirty = 1;
    pthread_mutex_init(&ui->mtx, NULL);
    return ui;
}

void ui_state_free(ui_state_t *ui) {
    if (!ui) return;
    pthread_mutex_destroy(&ui->mtx);
    free(ui->session_dir);
    free(ui->status_text);
    free(ui->input_buffer);
    free(ui->stream_tokens);
    free(ui->banner);
    free(ui->preview);
    free(ui->bottom_content);
    for (int i = 0; i < ui->query_count; i++) {
        ui_query_t *q = &ui->queries[i];
        free(q->query_text);
        free(q->result_preview);
        for (int j = 0; j < q->step_count; j++) {
            free(q->steps[j].ref);
            free(q->steps[j].tool);
            free(q->steps[j].description);
        }
        free(q->steps);
    }
    free(ui->queries);
    free(ui);
}

/* ── Navigation ────────────────────────────────────────── */

void ui_state_tab(ui_state_t *ui) {
    ui->focus = (ui->focus == FOCUS_JOURNAL) ? FOCUS_QUERY : FOCUS_JOURNAL;
    ui->dirty = 1;
}

void ui_state_up(ui_state_t *ui) {
    if (ui->focus != FOCUS_JOURNAL || ui->query_count == 0) return;

    if (ui->selected_step >= 0) {
        /* Currently on a step — move up within steps or back to query */
        ui->selected_step--;
        /* selected_step == -1 means back on the query header */
    } else {
        /* On a query header — move to previous query */
        if (ui->selected_query > 0) {
            ui->selected_query--;
            /* If previous query is expanded, jump to its last step */
            ui_query_t *q = &ui->queries[ui->selected_query];
            if (q->expanded && q->step_count > 0) {
                ui->selected_step = q->step_count - 1;
            }
        }
    }

    /* Load bottom content for selected item */
    ui_state_load_bottom_content(ui);
    ui->dirty = 1;
}

void ui_state_down(ui_state_t *ui) {
    if (ui->focus != FOCUS_JOURNAL || ui->query_count == 0) return;

    ui_query_t *q = &ui->queries[ui->selected_query];

    if (q->expanded && ui->selected_step < q->step_count - 1) {
        /* Move down within expanded steps */
        ui->selected_step++;
    } else {
        /* Move to next query */
        if (ui->selected_query < ui->query_count - 1) {
            ui->selected_query++;
            ui->selected_step = -1;
        }
    }

    ui_state_load_bottom_content(ui);
    ui->dirty = 1;
}

void ui_state_enter(ui_state_t *ui) {
    if (ui->focus != FOCUS_JOURNAL || ui->query_count == 0) return;

    ui_query_t *q = &ui->queries[ui->selected_query];

    if (ui->selected_step < 0) {
        /* On a query header — toggle expansion */
        q->expanded = !q->expanded;
        if (q->expanded && q->step_count == 0) {
            /* Load manifest steps for this query */
            ui_state_load_manifest(ui, ui->selected_query);
        }
        if (!q->expanded) {
            ui->selected_step = -1;
        }
    }
    /* On a step — bottom pane already shows content */

    ui_state_load_bottom_content(ui);
    ui->dirty = 1;
}

void ui_state_back(ui_state_t *ui) {
    if (ui->focus == FOCUS_JOURNAL) {
        ui_query_t *q = &ui->queries[ui->selected_query];
        if (ui->selected_step >= 0) {
            /* Back from step to query header */
            ui->selected_step = -1;
        } else if (q->expanded) {
            /* Collapse the query */
            q->expanded = 0;
        }
    }
    ui_state_load_bottom_content(ui);
    ui->dirty = 1;
}

void ui_state_page_up(ui_state_t *ui) {
    if (ui->bottom_scroll > 0) {
        ui->bottom_scroll -= 10;
        if (ui->bottom_scroll < 0) ui->bottom_scroll = 0;
        ui->dirty = 1;
    }
}

void ui_state_page_down(ui_state_t *ui) {
    if (ui->bottom_content) {
        ui->bottom_scroll += 10;
        ui->dirty = 1;
    }
}

/* ── Input editing ─────────────────────────────────────── */

void ui_state_input_char(ui_state_t *ui, int ch) {
    if (ui->input_len >= ui->input_cap - 1) return;
    /* Insert at cursor position */
    if (ui->cursor_pos < ui->input_len) {
        memmove(ui->input_buffer + ui->cursor_pos + 1,
                ui->input_buffer + ui->cursor_pos,
                ui->input_len - ui->cursor_pos);
    }
    ui->input_buffer[ui->cursor_pos] = (char)ch;
    ui->cursor_pos++;
    ui->input_len++;
    ui->input_buffer[ui->input_len] = '\0';
    ui->dirty = 1;
}

void ui_state_input_backspace(ui_state_t *ui) {
    if (ui->cursor_pos > 0) {
        memmove(ui->input_buffer + ui->cursor_pos - 1,
                ui->input_buffer + ui->cursor_pos,
                ui->input_len - ui->cursor_pos);
        ui->cursor_pos--;
        ui->input_len--;
        ui->input_buffer[ui->input_len] = '\0';
        ui->dirty = 1;
    }
}

void ui_state_input_delete(ui_state_t *ui) {
    if (ui->cursor_pos < ui->input_len) {
        memmove(ui->input_buffer + ui->cursor_pos,
                ui->input_buffer + ui->cursor_pos + 1,
                ui->input_len - ui->cursor_pos - 1);
        ui->input_len--;
        ui->input_buffer[ui->input_len] = '\0';
        ui->dirty = 1;
    }
}

void ui_state_input_left(ui_state_t *ui) {
    if (ui->cursor_pos > 0) { ui->cursor_pos--; ui->dirty = 1; }
}

void ui_state_input_right(ui_state_t *ui) {
    if (ui->cursor_pos < ui->input_len) { ui->cursor_pos++; ui->dirty = 1; }
}

void ui_state_input_home(ui_state_t *ui) {
    ui->cursor_pos = 0; ui->dirty = 1;
}

void ui_state_input_end(ui_state_t *ui) {
    ui->cursor_pos = ui->input_len; ui->dirty = 1;
}

/* ── Load journal entries into query list ──────────────── */

void ui_state_load_journal(ui_state_t *ui, journal_t *journal) {
    if (!ui || !journal) return;

    /* Clear existing queries */
    for (int i = 0; i < ui->query_count; i++) {
        ui_query_t *q = &ui->queries[i];
        free(q->query_text);
        free(q->result_preview);
        for (int j = 0; j < q->step_count; j++) {
            free(q->steps[j].ref);
            free(q->steps[j].tool);
            free(q->steps[j].description);
        }
        free(q->steps);
    }
    ui->query_count = 0;

    /* Read journal.jsonl */
    FILE *f = fopen(journal->path, "r");
    if (!f) return;

    char line[65536];
    while (fgets(line, sizeof(line), f)) {
        cJSON *entry = cJSON_Parse(line);
        if (!entry) continue;

        const char *tool = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "tool"));
        int loop = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(entry, "react_loop"));

        if (tool && strcmp(tool, "query") == 0) {
            /* New query entry */
            cJSON *params = cJSON_GetObjectItem(entry, "params");
            const char *text = params ? cJSON_GetStringValue(cJSON_GetObjectItem(params, "text")) : NULL;
            double ts = 0;
            cJSON *ts_j = cJSON_GetObjectItem(entry, "ts");
            if (ts_j && ts_j->valuestring) ts = atof(ts_j->valuestring);

            /* Ensure capacity */
            if (ui->query_count >= ui->query_cap) {
                ui->query_cap = ui->query_cap ? ui->query_cap * 2 : 16;
                ui->queries = realloc(ui->queries, ui->query_cap * sizeof(ui_query_t));
            }
            ui_query_t *q = &ui->queries[ui->query_count++];
            memset(q, 0, sizeof(*q));
            q->query_text = text ? strdup(text) : strdup("(empty)");
            q->timestamp = ts;
            q->react_loop = loop;
        }

        cJSON_Delete(entry);
    }
    fclose(f);

    /* Load steps for each query */
    for (int i = 0; i < ui->query_count; i++) {
        ui_state_load_manifest(ui, i);
    }

    ui->dirty = 1;
}

/* ── Load manifest steps for a specific query ──────────── */

void ui_state_load_manifest(ui_state_t *ui, int query_idx) {
    if (!ui || query_idx < 0 || query_idx >= ui->query_count) return;
    ui_query_t *q = &ui->queries[query_idx];

    /* Free existing steps */
    for (int j = 0; j < q->step_count; j++) {
        free(q->steps[j].ref);
        free(q->steps[j].tool);
        free(q->steps[j].description);
    }
    free(q->steps);
    q->steps = NULL;
    q->step_count = 0;
    q->step_cap = 0;

    /* Read journal and collect steps for this react_loop */
    /* Read journal.jsonl for this session */
    char jpath[4096];
    snprintf(jpath, sizeof(jpath), "%s/journal.jsonl", ui->session_dir);
    FILE *f = fopen(jpath, "r");
    if (!f) return;

    char line[65536];
    while (fgets(line, sizeof(line), f)) {
        cJSON *entry = cJSON_Parse(line);
        if (!entry) continue;

        int loop = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(entry, "react_loop"));
        const char *tool = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "tool"));
        int step = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(entry, "step"));

        if (loop == q->react_loop && tool &&
            strcmp(tool, "system") != 0 && strcmp(tool, "query") != 0) {

            /* Extract key parameter for display */
            cJSON *params = cJSON_GetObjectItem(entry, "params");
            const char *desc = "";
            static char desc_buf[512];  /* static buffer for combined descriptions */
            if (params) {
                cJSON *cmd = cJSON_GetObjectItem(params, "command");
                cJSON *path = cJSON_GetObjectItem(params, "path");
                cJSON *pat = cJSON_GetObjectItem(params, "pattern");
                cJSON *qry = cJSON_GetObjectItem(params, "query");
                cJSON *res = cJSON_GetObjectItem(params, "result");
                /* grep_search: show pattern (and path if present) */
                if (strcmp(tool, "grep_search") == 0 && pat && pat->valuestring) {
                    if (path && path->valuestring)
                        snprintf(desc_buf, sizeof(desc_buf), "/%s/ in %s",
                                 pat->valuestring, path->valuestring);
                    else
                        snprintf(desc_buf, sizeof(desc_buf), "/%s/",
                                 pat->valuestring);
                    desc = desc_buf;
                }
                else if (cmd && cmd->valuestring) desc = cmd->valuestring;
                else if (path && path->valuestring) desc = path->valuestring;
                else if (pat && pat->valuestring) desc = pat->valuestring;
                else if (qry && qry->valuestring) desc = qry->valuestring;
                else if (res && res->valuestring) desc = res->valuestring;
            }

            const char *ref = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "ref"));
            int sz = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(entry, "size"));
            cJSON *failed_j = cJSON_GetObjectItem(entry, "failed");
            int failed = (failed_j && cJSON_IsTrue(failed_j));

            /* Add step */
            if (q->step_count >= q->step_cap) {
                q->step_cap = q->step_cap ? q->step_cap * 2 : 32;
                q->steps = realloc(q->steps, q->step_cap * sizeof(ui_step_t));
            }
            ui_step_t *s = &q->steps[q->step_count++];
            memset(s, 0, sizeof(*s));
            s->ref = ref ? strdup(ref) : NULL;
            s->tool = tool ? strdup(tool) : NULL;
            s->description = strdup(desc);
            s->failed = failed;
            s->size = sz;
            s->step = step;
        }

        cJSON_Delete(entry);
    }
    fclose(f);

    q->failed_count = 0;
    for (int j = 0; j < q->step_count; j++) {
        if (q->steps[j].failed) q->failed_count++;
    }
}

/* ── Load bottom pane content based on selection ───────── */

void ui_state_load_bottom_content(ui_state_t *ui) {
    if (!ui) return;

    free(ui->bottom_content);
    ui->bottom_content = NULL;
    ui->bottom_lines = 0;
    ui->bottom_scroll = 0;

    if (ui->query_count == 0 || ui->selected_query < 0) return;
    ui_query_t *q = &ui->queries[ui->selected_query];

    if (ui->selected_step >= 0 && ui->selected_step < q->step_count) {
        /* Step selected — show actual file content from store */
        ui_step_t *s = &q->steps[ui->selected_step];
        if (s->ref) {
            char path[4096];
            snprintf(path, sizeof(path), "%s/%s", ui->session_dir, s->ref);
            FILE *f = fopen(path, "r");
            if (f) {
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                if (sz > 32768) sz = 32768;  /* cap at 32K */
                fseek(f, 0, SEEK_SET);
                ui->bottom_content = malloc((size_t)sz + 1);
                if (ui->bottom_content) {
                    size_t n = fread(ui->bottom_content, 1, (size_t)sz, f);
                    ui->bottom_content[n] = '\0';
                    /* Count lines */
                    ui->bottom_lines = 0;
                    for (size_t i = 0; i < n; i++)
                        if (ui->bottom_content[i] == '\n') ui->bottom_lines++;
                    if (n > 0 && ui->bottom_content[n-1] != '\n') ui->bottom_lines++;
                }
                fclose(f);
            }
        }
    } else if (ui->selected_step < 0 && q->expanded && q->step_count > 0) {
        /* Query selected with steps — show summary in bottom */
        str_t s = str_new(1024);
        str_appendf(&s, "Query: %s\n", q->query_text ? q->query_text : "");
        str_appendf(&s, "Steps: %d | Failed: %d\n", q->step_count, q->failed_count);
        if (q->result_preview)
            str_appendf(&s, "Result: %s\n", q->result_preview);
        ui->bottom_content = str_steal(&s);
        ui->bottom_lines = 3;
    }

    ui->dirty = 1;
}

/* ── Load detail for a step ────────────────────────────── */

void ui_state_load_detail(ui_state_t *ui, int step_idx) {
    /* Now handled by ui_state_load_bottom_content */
    (void)ui; (void)step_idx;
}

/* ── Status ────────────────────────────────────────────── */

void ui_state_set_status(ui_state_t *ui, ui_status_t status, const char *text) {
    if (!ui) return;
    ui->status = status;
    free(ui->status_text);
    ui->status_text = text ? strdup(text) : NULL;
    ui->dirty = 1;
}

/* ── Add query ─────────────────────────────────────────── */

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
    q->expanded = 1;  /* Auto-expand the current query */
    ui->selected_query = ui->query_count - 1;
    ui->selected_step = -1;
    ui->dirty = 1;
}

/* ── Set banner ────────────────────────────────────────── */

void ui_state_set_banner(ui_state_t *ui, const char *banner) {
    if (!ui) return;
    free(ui->banner);
    ui->banner = banner ? strdup(banner) : NULL;
    ui->dirty = 1;
}

/* ── React event handler ───────────────────────────────── */

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
        if (ui->stream_tokens) {
            ui->stream_tokens[0] = '\0';
            ui->stream_len = 0;
        }
        ui->dirty = 1;
        break;
    }

    case REACT_EVENT_LLM_TOKEN:
        if (ev->token && ui->stream_tokens) {
            int tlen = (int)strlen(ev->token);
            if (ui->stream_len + tlen < ui->stream_cap - 1) {
                memcpy(ui->stream_tokens + ui->stream_len, ev->token, tlen);
                ui->stream_len += tlen;
                ui->stream_tokens[ui->stream_len] = '\0';
            }
            ui->dirty = 1;
        }
        break;

    case REACT_EVENT_STEP_COMPLETE: {
        /* Add step to the current (last) query */
        if (ui->query_count > 0) {
            ui_query_t *q = &ui->queries[ui->query_count - 1];

            if (q->step_count >= q->step_cap) {
                q->step_cap = q->step_cap ? q->step_cap * 2 : 32;
                q->steps = realloc(q->steps, q->step_cap * sizeof(ui_step_t));
            }
            ui_step_t *s = &q->steps[q->step_count++];
            memset(s, 0, sizeof(*s));
            s->tool = ev->action ? strdup(ev->action) : NULL;
            s->description = ev->description ? strdup(ev->description) : NULL;
            s->step = ev->step;
            s->elapsed = ev->step_elapsed;
        }
        /* Clear streaming tokens */
        if (ui->stream_tokens) {
            ui->stream_tokens[0] = '\0';
            ui->stream_len = 0;
        }
        ui->dirty = 1;
        break;
    }

    case REACT_EVENT_TOOL_OUTPUT:
        /* Update the last step with store ref and metadata */
        if (ui->query_count > 0) {
            ui_query_t *q = &ui->queries[ui->query_count - 1];
            if (q->step_count > 0) {
                ui_step_t *s = &q->steps[q->step_count - 1];
                if (ev->store_ref) {
                    free(s->ref);
                    s->ref = strdup(ev->store_ref);
                }
                if (ev->tool_meta) {
                    cJSON *sz = cJSON_GetObjectItem(ev->tool_meta, "chars");
                    if (!sz) sz = cJSON_GetObjectItem(ev->tool_meta, "size");
                    if (sz) s->size = (int)cJSON_GetNumberValue(sz);
                    cJSON *err = cJSON_GetObjectItem(ev->tool_meta, "error");
                    if (err) s->failed = 1;
                }

                /* Auto-load bottom content for the latest step */
                ui->selected_step = q->step_count - 1;
                ui_state_load_bottom_content(ui);
            }
        }
        ui->dirty = 1;
        break;

    case REACT_EVENT_ERROR:
    case REACT_EVENT_WARNING:
        if (ev->message) {
            char buf[256];
            snprintf(buf, sizeof(buf), "%s: %s",
                     ev->type == REACT_EVENT_ERROR ? "Error" : "Warning",
                     ev->message);
            ui_state_set_status(ui, STATUS_ERROR, buf);
        }
        ui->dirty = 1;
        break;

    case REACT_EVENT_DONE:
        ui_state_set_status(ui, STATUS_DONE, "Done");
        /* Update result preview */
        if (ui->query_count > 0 && ev->result) {
            ui_query_t *q = &ui->queries[ui->query_count - 1];
            free(q->result_preview);
            q->result_preview = strndup(ev->result, 200);
        }
        ui->dirty = 1;
        break;
    }
}
