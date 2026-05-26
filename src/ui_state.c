#include "ui_state.h"
#include "md_render.h"
#include "str.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ── helpers ─────────────────────────────────────────── */


/* Sanitize text for use inside MD link [text](uri) syntax.
 * Replaces characters that break the link: ] [ ( ) and newlines.
 * Returns a static buffer u2014 NOT thread-safe, use immediately. */
static const char *sanitize_md_link(const char *text) {
    static char buf[512];
    int j = 0;
    if (!text) return "";
    for (int i = 0; text[i] && j < (int)sizeof(buf) - 1; i++) {
        switch (text[i]) {
            case ']': buf[j++] = ')'; break;
            case '[': buf[j++] = '('; break;
            case '\n': buf[j++] = ' '; break;
            case '\r': break; /* skip */
            default: buf[j++] = text[i]; break;
        }
    }
    buf[j] = '\0';
    return buf;
}

/* ── lifecycle ───────────────────────────────────────── */

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
    ui->cursor_link = 0;
    ui->dirty = 1;
    pthread_mutex_init(&ui->mtx, NULL);
    return ui;
}

void ui_state_free(ui_state_t *ui) {
    if (!ui) return;
    md_doc_free(ui->doc);
    free(ui->link_states);
    /* Free expanded step URIs */
    for (int i = 0; i < ui->expanded_count; i++)
        free(ui->expanded_uris[i]);
    free(ui->expanded_uris);
    free(ui->banner);
    free(ui->session_dir);
    free(ui->status_text);
    free(ui->input_buffer);
    free(ui->stream_tokens);
    /* Free query history */
    for (int i = 0; i < ui->history_count; i++)
        free(ui->history[i]);
    free(ui->history);
    pthread_mutex_destroy(&ui->mtx);
    free(ui);
}

/* ── MD document generation from journal ─────────────── */

void ui_state_rebuild_md(ui_state_t *ui) {
    if (!ui) return;

    str_t md = str_new(8192);

    /* Banner */
    if (ui->banner && ui->banner[0]) {
        str_append_cstr(&md, ui->banner);
        str_append_cstr(&md, "\n---\n\n");
    }

    /* Read journal and build query list */
    if (!ui->journal) {
        /* No journal yet — just show banner */
        if (md.len == 0)
            str_append_cstr(&md, "# Nash\n\n*No session loaded*\n");
        goto done;
    }

    char jpath[4096];
    snprintf(jpath, sizeof(jpath), "%s/journal.jsonl",
             ui->session_dir ? ui->session_dir : ".");
    FILE *f = fopen(jpath, "r");
    if (!f) goto done;

    /* First pass: collect queries (tool="query" entries) */
    typedef struct {
        char *text;
        double ts;
        int react_loop;
        int step_count;
        int failed_count;
        char *result;
    } qinfo_t;

    qinfo_t *qinfos = NULL;
    int qcount = 0, qcap = 0;

    char line[65536];
    while (fgets(line, sizeof(line), f)) {
        cJSON *entry = cJSON_Parse(line);
        if (!entry) continue;

        const char *tool = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "tool"));
        int loop = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(entry, "react_loop"));

        if (tool && strcmp(tool, "query") == 0) {
            /* New query */
            if (qcount >= qcap) {
                qcap = qcap ? qcap * 2 : 16;
                qinfos = realloc(qinfos, qcap * sizeof(qinfo_t));
            }
            qinfo_t *qi = &qinfos[qcount++];
            memset(qi, 0, sizeof(*qi));
            cJSON *params = cJSON_GetObjectItem(entry, "params");
            cJSON *text = params ? cJSON_GetObjectItem(params, "text") : NULL;
            qi->text = (text && text->valuestring) ? strdup(text->valuestring) : strdup("?");
            cJSON *ts = cJSON_GetObjectItem(entry, "ts");
            qi->ts = ts && ts->valuestring ? atof(ts->valuestring) : 0;
            qi->react_loop = loop;
        } else if (tool && strcmp(tool, "system") != 0 && strcmp(tool, "query") != 0) {
            /* Count steps per query */
            for (int i = qcount - 1; i >= 0; i--) {
                if (qinfos[i].react_loop == loop) {
                    qinfos[i].step_count++;
                    cJSON *failed_j = cJSON_GetObjectItem(entry, "failed");
                    if (failed_j && cJSON_IsTrue(failed_j))
                        qinfos[i].failed_count++;

                    /* Capture done result */
                    if (strcmp(tool, "done") == 0) {
                        cJSON *params = cJSON_GetObjectItem(entry, "params");
                        cJSON *res = params ? cJSON_GetObjectItem(params, "result") : NULL;
                        if (res && res->valuestring) {
                            free(qinfos[i].result);
                            qinfos[i].result = strdup(res->valuestring);
                        }
                    }
                    break;
                }
            }
        }
        cJSON_Delete(entry);
    }
    fclose(f);

    /* Ensure link_states array is big enough */
    if (qcount > ui->link_states_cap) {
        ui->link_states_cap = qcount + 16;
        ui->link_states = realloc(ui->link_states,
                                   ui->link_states_cap * sizeof(link_state_t));
    }
    /* Initialize new link states to COLLAPSED */
    for (int i = ui->link_states_count; i < qcount; i++)
        ui->link_states[i] = LINK_COLLAPSED;
    ui->link_states_count = qcount;

    /* Generate MD with session history */
    str_append_cstr(&md, "## Session History\n\n");

    for (int i = 0; i < qcount; i++) {
        qinfo_t *qi = &qinfos[i];
        link_state_t ls = (i < ui->link_states_count) ?
                          ui->link_states[i] : LINK_COLLAPSED;

        /* When a react loop is actively running, override display:
         * - Active loop (last one): force SHOW_STEPS so agent progress is visible
         * - All other loops: force COLLAPSED to reduce noise */
        int is_active_loop = (ui->status == STATUS_RUNNING && i == qcount - 1);
        if (ui->status == STATUS_RUNNING) {
            ls = is_active_loop ? LINK_SHOW_STEPS : LINK_COLLAPSED;
        }

        /* Format timestamp */
        char ts_buf[32] = "";
        if (qi->ts > 0) {
            time_t t = (time_t)qi->ts;
            struct tm *tm = localtime(&t);
            if (tm) strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%d %H:%M", tm);
        }

        /* Query as hyperlink */
        char prefix = (ls == LINK_COLLAPSED) ? '>' : 'v';
        str_appendf(&md, "[%c %s  %s](file://session/R%d)\n",
                    prefix, ts_buf, sanitize_md_link(qi->text), qi->react_loop);

        /* Expanded content based on link state */
        if (ls == LINK_SHOW_RESULT && qi->result) {
            str_appendf(&md, "> %s\n", qi->result);
        } else if (ls == LINK_SHOW_STEPS) {
            /* Read journal for this query's steps (shown BEFORE result) */
            FILE *f2 = fopen(jpath, "r");
            if (f2) {
                char line2[65536];
                while (fgets(line2, sizeof(line2), f2)) {
                    cJSON *e = cJSON_Parse(line2);
                    if (!e) continue;
                    int loop = (int)cJSON_GetNumberValue(
                        cJSON_GetObjectItem(e, "react_loop"));
                    const char *t = cJSON_GetStringValue(
                        cJSON_GetObjectItem(e, "tool"));
                    if (loop == qi->react_loop && t &&
                        strcmp(t, "query") != 0) {

                        const char *ref = cJSON_GetStringValue(
                            cJSON_GetObjectItem(e, "ref"));
                        int sz = (int)cJSON_GetNumberValue(
                            cJSON_GetObjectItem(e, "size"));
                        cJSON *failed_j = cJSON_GetObjectItem(e, "failed");
                        int failed = (failed_j && cJSON_IsTrue(failed_j));

                        /* Extract key param */
                        cJSON *params = cJSON_GetObjectItem(e, "params");
                        const char *desc = "";
                        const char *thought = "";
                        if (params) {
                            cJSON *th = cJSON_GetObjectItem(params, "thought");
                            if (th && th->valuestring) thought = th->valuestring;
                            cJSON *cmd = cJSON_GetObjectItem(params, "command");
                            cJSON *path = cJSON_GetObjectItem(params, "path");
                            cJSON *pat = cJSON_GetObjectItem(params, "pattern");
                            cJSON *qry = cJSON_GetObjectItem(params, "query");
                            cJSON *res = cJSON_GetObjectItem(params, "result");
                            if (strcmp(t, "grep_search") == 0 && pat && pat->valuestring)
                                desc = pat->valuestring;
                            else if (cmd && cmd->valuestring) desc = cmd->valuestring;
                            else if (path && path->valuestring) desc = path->valuestring;
                            else if (pat && pat->valuestring) desc = pat->valuestring;
                            else if (qry && qry->valuestring) desc = qry->valuestring;
                            else if (res && res->valuestring) desc = res->valuestring;
                        }

                        /* Render step as a hyperlink so cursor can land on it */
                        {
                            char step_uri[256];
                            snprintf(step_uri, sizeof(step_uri),
                                     "file://session/%s", ref ? ref : "?");
                            char desc_trunc[61];
                            utf8_truncate(desc_trunc, sanitize_md_link(desc), 60);
                            /* Show thought as a line ABOVE the step entry,
                             * aligned with "  + R0S6:" step lines. */
                            if (thought[0]) {
                                /* Trim trailing whitespace/newlines from thought */
                                int tlen = (int)strlen(thought);
                                while (tlen > 0 && (thought[tlen-1] == '\n' ||
                                       thought[tlen-1] == '\r' ||
                                       thought[tlen-1] == ' '))
                                    tlen--;
                                if (tlen > 0)
                                    str_appendf(&md, "  💭 %.*s\n", tlen, thought);
                            }
                            str_appendf(&md, "[  %s %s: %s \"%s\"",
                                        failed ? "x" : "+",
                                        ref ? ref : "?",
                                        t, desc_trunc);
                            if (!failed && sz > 0)
                                str_appendf(&md, " -> %d chars", sz);
                            str_appendf(&md, "](%s)\n", step_uri);

                            /* Check if this step is expanded (URI-based lookup) */
                            int is_expanded = 0;
                            for (int ei = 0; ei < ui->expanded_count; ei++) {
                                if (strcmp(ui->expanded_uris[ei], step_uri) == 0) {
                                    is_expanded = 1;
                                    break;
                                }
                            }
                            if (is_expanded && ref) {
                                /* Read content from store */
                                char rpath[4096];
                                snprintf(rpath, sizeof(rpath), "%s/%s",
                                         ui->session_dir, ref);
                                FILE *cf = fopen(rpath, "r");
                                if (cf) {
                                    str_append_cstr(&md, "```\n");
                                    char cbuf[4096];
                                    size_t total = 0;
                                    size_t n;
                                    while ((n = fread(cbuf, 1, sizeof(cbuf)-1, cf)) > 0
                                           && total < 8000) {
                                        cbuf[n] = '\0';
                                        str_append(&md, cbuf, n);
                                        total += n;
                                    }
                                    if (total >= 8000)
                                        str_append_cstr(&md, "\n... (truncated)\n");
                                    str_append_cstr(&md, "```\n");
                                    fclose(cf);
                                }
                            }
                        }
                    }
                    cJSON_Delete(e);
                }
                fclose(f2);
            }

            /* Show result AFTER steps (at the bottom) */
            if (qi->result)
                str_appendf(&md, "> %s\n", qi->result);

            /* If this is the active loop during inference, show streaming
             * content directly after the steps (no separate section). */
            if (is_active_loop && ui->status == STATUS_RUNNING) {
                if (ui->max_steps > 0)
                    str_appendf(&md, "  ⏳ Step %d/%d thinking...\n",
                                ui->current_step, ui->max_steps);
                else
                    str_appendf(&md, "  ⏳ Step %d thinking...\n",
                                ui->current_step);
                /* Show streaming content (full, not just preview) */
                if (ui->stream_tokens && ui->stream_len > 0) {
                    str_append_cstr(&md, "```\n");
                    str_append(&md, ui->stream_tokens, (size_t)ui->stream_len);
                    str_append_cstr(&md, "```\n");
                }
            }
        }
    }

    /* Free query infos */
    for (int i = 0; i < qcount; i++) {
        free(qinfos[i].text);
        free(qinfos[i].result);
    }
    free(qinfos);

done:;
    char *md_source = str_steal(&md);

    /* Parse into MD document */
    md_doc_free(ui->doc);
    ui->doc = md_parse(md_source);
    free(md_source);

    /* Auto-scroll to bottom when actively streaming */
    if (ui->status == STATUS_RUNNING && ui->doc) {
        int vis = ui->visible_rows > 0 ? ui->visible_rows : 20;
        int bottom = ui->doc->total_lines - vis;
        if (bottom < 0) bottom = 0;
        ui->scroll_y = bottom;
    }

    /* Resize link_states to cover ALL links (queries + steps).
     * The initial sizing (qcount) only covers query links.
     * Step links get indices beyond qcount and need states too. */
    if (ui->doc && ui->doc->link_count > ui->link_states_count) {
        if (ui->doc->link_count > ui->link_states_cap) {
            ui->link_states_cap = ui->doc->link_count + 16;
            ui->link_states = realloc(ui->link_states,
                                       ui->link_states_cap * sizeof(link_state_t));
        }
        /* Initialize new step link states to COLLAPSED */
        for (int i = ui->link_states_count; i < ui->doc->link_count; i++)
            ui->link_states[i] = LINK_COLLAPSED;
        ui->link_states_count = ui->doc->link_count;
    }

    /* Clamp cursor */
    if (ui->doc && ui->cursor_link >= ui->doc->link_count)
        ui->cursor_link = ui->doc->link_count > 0 ? ui->doc->link_count - 1 : 0;

    ui->dirty = 1;
}

/* ── Navigation ──────────────────────────────────────── */

/* Forward declaration — defined below */
static int find_visible_links(md_doc_t *doc, int scroll_y, int vis_h,
                               int *first, int *last);

void ui_state_tab(ui_state_t *ui) {
    if (!ui) return;
    ui->focus = (ui->focus == FOCUS_JOURNAL) ? FOCUS_QUERY : FOCUS_JOURNAL;

    /* When switching TO journal, snap cursor to first visible link
     * so the highlight is immediately visible (not stuck on an
     * off-screen link at index 0). */
    if (ui->focus == FOCUS_JOURNAL && ui->doc && ui->doc->link_count > 0) {
        int vis = ui->visible_rows > 0 ? ui->visible_rows : 20;
        int first_vis, last_vis;
        int n_vis = find_visible_links(ui->doc, ui->scroll_y, vis,
                                        &first_vis, &last_vis);
        if (n_vis > 0 && (ui->cursor_link < first_vis || ui->cursor_link > last_vis))
            ui->cursor_link = first_vis;
    }

    ui->dirty = 1;
}

/* Find the range of link indices visible in the current viewport.
 * Returns count of visible links. first/last are set to the first and
 * last visible link index (-1 if none visible). */
static int find_visible_links(md_doc_t *doc, int scroll_y, int vis_h,
                               int *first, int *last) {
    *first = -1;
    *last = -1;
    if (!doc || doc->link_count == 0) return 0;
    int count = 0;
    for (int i = 0; i < doc->link_count; i++) {
        int line = md_link_line(doc, i);
        if (line >= scroll_y && line < scroll_y + vis_h) {
            if (*first < 0) *first = i;
            *last = i;
            count++;
        }
    }
    return count;
}

void ui_state_up(ui_state_t *ui) {
    if (!ui) return;
    if (ui->focus == FOCUS_JOURNAL && ui->doc) {
        int vis = ui->visible_rows > 0 ? ui->visible_rows : 20;
        int first_vis, last_vis;
        int n_vis = find_visible_links(ui->doc, ui->scroll_y, vis,
                                        &first_vis, &last_vis);

        if (n_vis == 0) {
            /* No links on screen — pure scroll mode */
            if (ui->scroll_y > 0) ui->scroll_y--;
        } else if (ui->cursor_link <= first_vis) {
            /* At or above topmost visible link — scroll up */
            if (ui->scroll_y > 0) ui->scroll_y--;
            /* Re-find visible links after scroll and snap cursor */
            n_vis = find_visible_links(ui->doc, ui->scroll_y, vis,
                                        &first_vis, &last_vis);
            if (n_vis > 0 && ui->cursor_link > first_vis)
                ui->cursor_link = first_vis;
            else if (n_vis > 0 && first_vis < ui->cursor_link)
                ui->cursor_link = first_vis;
        } else {
            /* Move cursor to previous visible link (no scroll) */
            for (int i = ui->cursor_link - 1; i >= 0; i--) {
                int line = md_link_line(ui->doc, i);
                if (line >= ui->scroll_y && line < ui->scroll_y + vis) {
                    ui->cursor_link = i;
                    break;
                }
            }
        }
    }
    ui->dirty = 1;
}

void ui_state_down(ui_state_t *ui) {
    if (!ui) return;
    if (ui->focus == FOCUS_JOURNAL && ui->doc) {
        int vis = ui->visible_rows > 0 ? ui->visible_rows : 20;
        int first_vis, last_vis;
        int n_vis = find_visible_links(ui->doc, ui->scroll_y, vis,
                                        &first_vis, &last_vis);

        if (n_vis == 0) {
            /* No links on screen — pure scroll mode */
            if (ui->scroll_y < ui->doc->total_lines - 1)
                ui->scroll_y++;
        } else if (ui->cursor_link >= last_vis) {
            /* At or below bottommost visible link — scroll down */
            if (ui->scroll_y < ui->doc->total_lines - 1)
                ui->scroll_y++;
            /* Re-find visible links after scroll and snap cursor */
            n_vis = find_visible_links(ui->doc, ui->scroll_y, vis,
                                        &first_vis, &last_vis);
            if (n_vis > 0 && ui->cursor_link < last_vis)
                ui->cursor_link = last_vis;
        } else {
            /* Move cursor to next visible link (no scroll) */
            for (int i = ui->cursor_link + 1; i < ui->doc->link_count; i++) {
                int line = md_link_line(ui->doc, i);
                if (line >= ui->scroll_y && line < ui->scroll_y + vis) {
                    ui->cursor_link = i;
                    break;
                }
            }
        }
    }
    ui->dirty = 1;
}

void ui_state_enter(ui_state_t *ui) {
    if (!ui) return;
    if (ui->focus != FOCUS_JOURNAL) return;
    if (!ui->doc || ui->doc->link_count == 0) return;

    int idx = ui->cursor_link;
    if (idx < 0 || idx >= ui->link_states_count) return;

    /* Check if this is a step link (ref-based URI like "file://session/R0S3")
     * or a query link. Step refs always match R<digit>S<digit> pattern. */
    const char *uri = (idx < ui->doc->link_count) ? ui->doc->links[idx].uri : NULL;
    int is_step_link = 0;
    if (uri) {
        const char *r = strstr(uri, "/R");
        if (r) {
            r++;  /* skip the '/' */
            /* Check for R<digits>S<digits> pattern */
            if (*r == 'R' && r[1] >= '0' && r[1] <= '9') {
                const char *s = strchr(r, 'S');
                if (s && s[1] >= '0' && s[1] <= '9')
                    is_step_link = 1;
            }
        }
    }

    if (is_step_link && uri) {
        /* Step link: toggle expansion via URI-based tracking.
         * We can't rely on link_states[idx] because indices change
         * when the MD is regenerated (content insertion shifts links). */
        int found = 0;
        for (int i = 0; i < ui->expanded_count; i++) {
            if (strcmp(ui->expanded_uris[i], uri) == 0) {
                /* Already expanded — collapse: remove from list */
                free(ui->expanded_uris[i]);
                ui->expanded_uris[i] = ui->expanded_uris[--ui->expanded_count];
                found = 1;
                break;
            }
        }
        if (!found) {
            /* Not expanded — expand: add URI to list */
            if (ui->expanded_count >= ui->expanded_cap) {
                ui->expanded_cap = ui->expanded_cap ? ui->expanded_cap * 2 : 16;
                ui->expanded_uris = realloc(ui->expanded_uris,
                                             ui->expanded_cap * sizeof(char *));
            }
            ui->expanded_uris[ui->expanded_count++] = strdup(uri);
        }
    } else {
        /* Query link: cycle COLLAPSED -> SHOW_RESULT -> SHOW_STEPS -> COLLAPSED */
        switch (ui->link_states[idx]) {
            case LINK_COLLAPSED:   ui->link_states[idx] = LINK_SHOW_RESULT; break;
            case LINK_SHOW_RESULT: ui->link_states[idx] = LINK_SHOW_STEPS;  break;
            case LINK_SHOW_STEPS:  ui->link_states[idx] = LINK_COLLAPSED;   break;
            default:               ui->link_states[idx] = LINK_COLLAPSED;   break;
        }
    }

    /* Regenerate MD to reflect new state */
    ui_state_rebuild_md(ui);
    /* Auto-scroll to keep cursor visible after expansion change */
    if (ui->doc && ui->cursor_link >= 0 && ui->cursor_link < ui->doc->link_count) {
        int link_line = md_link_line(ui->doc, ui->cursor_link);
        int vis = ui->visible_rows > 0 ? ui->visible_rows : 20;
        if (link_line < ui->scroll_y)
            ui->scroll_y = link_line;
        else if (link_line >= ui->scroll_y + vis)
            ui->scroll_y = link_line - vis + 1;
    }
}

void ui_state_back(ui_state_t *ui) {
    if (!ui) return;
    if (ui->focus == FOCUS_JOURNAL) {
        int idx = ui->cursor_link;
        if (idx >= 0 && idx < ui->link_states_count &&
            ui->link_states[idx] != LINK_COLLAPSED) {
            ui->link_states[idx] = LINK_COLLAPSED;
            ui_state_rebuild_md(ui);
    /* Auto-scroll to keep cursor visible after expansion change */
    if (ui->doc && ui->cursor_link >= 0 && ui->cursor_link < ui->doc->link_count) {
        int link_line = md_link_line(ui->doc, ui->cursor_link);
        int vis = ui->visible_rows > 0 ? ui->visible_rows : 20;
        if (link_line < ui->scroll_y)
            ui->scroll_y = link_line;
        else if (link_line >= ui->scroll_y + vis)
            ui->scroll_y = link_line - vis + 1;
    }
        }
    }
    ui->dirty = 1;
}

void ui_state_page_up(ui_state_t *ui) {
    if (!ui) return;
    int page = ui->visible_rows > 2 ? ui->visible_rows - 2 : 10;
    ui->scroll_y -= page;
    if (ui->scroll_y < 0) ui->scroll_y = 0;

    /* Snap cursor to nearest visible link at bottom of viewport */
    if (ui->doc && ui->doc->link_count > 0) {
        int link_line = md_link_line(ui->doc, ui->cursor_link);
        int vis = ui->visible_rows > 0 ? ui->visible_rows : 20;
        if (link_line >= ui->scroll_y + vis) {
            /* Cursor is below viewport — find closest link at bottom of visible area */
            for (int i = ui->doc->link_count - 1; i >= 0; i--) {
                int ll = md_link_line(ui->doc, i);
                if (ll >= ui->scroll_y && ll < ui->scroll_y + vis) {
                    ui->cursor_link = i;
                    break;
                }
            }
        } else if (link_line < ui->scroll_y) {
            /* Cursor is above viewport — find closest link at top of visible area */
            for (int i = 0; i < ui->doc->link_count; i++) {
                int ll = md_link_line(ui->doc, i);
                if (ll >= ui->scroll_y && ll < ui->scroll_y + vis) {
                    ui->cursor_link = i;
                    break;
                }
            }
        }
    }
    ui->dirty = 1;
}

void ui_state_page_down(ui_state_t *ui) {
    if (!ui) return;
    int page = ui->visible_rows > 2 ? ui->visible_rows - 2 : 10;
    ui->scroll_y += page;

    /* Snap cursor to nearest visible link at top of viewport */
    if (ui->doc && ui->doc->link_count > 0) {
        int link_line = md_link_line(ui->doc, ui->cursor_link);
        int vis = ui->visible_rows > 0 ? ui->visible_rows : 20;
        if (link_line < ui->scroll_y) {
            /* Cursor is above viewport — find closest link at top of visible area */
            for (int i = 0; i < ui->doc->link_count; i++) {
                int ll = md_link_line(ui->doc, i);
                if (ll >= ui->scroll_y && ll < ui->scroll_y + vis) {
                    ui->cursor_link = i;
                    break;
                }
            }
        } else if (link_line >= ui->scroll_y + vis) {
            /* Cursor is below viewport — find closest link at bottom of visible area */
            for (int i = ui->doc->link_count - 1; i >= 0; i--) {
                int ll = md_link_line(ui->doc, i);
                if (ll >= ui->scroll_y && ll < ui->scroll_y + vis) {
                    ui->cursor_link = i;
                    break;
                }
            }
        }
    }
    ui->dirty = 1;
}

/* ── Input editing ───────────────────────────────────── */

void ui_state_input_char(ui_state_t *ui, int ch) {
    if (!ui || ch < 32 || ch > 126) return;
    if (ui->input_len >= ui->input_cap - 1) {
        ui->input_cap *= 2;
        ui->input_buffer = realloc(ui->input_buffer, ui->input_cap);
    }
    /* Insert at cursor position */
    memmove(ui->input_buffer + ui->cursor_pos + 1,
            ui->input_buffer + ui->cursor_pos,
            ui->input_len - ui->cursor_pos + 1);
    ui->input_buffer[ui->cursor_pos] = (char)ch;
    ui->cursor_pos++;
    ui->input_len++;
    ui->dirty = 1;
}

void ui_state_input_backspace(ui_state_t *ui) {
    if (!ui || ui->cursor_pos <= 0) return;
    memmove(ui->input_buffer + ui->cursor_pos - 1,
            ui->input_buffer + ui->cursor_pos,
            ui->input_len - ui->cursor_pos + 1);
    ui->cursor_pos--;
    ui->input_len--;
    ui->dirty = 1;
}

void ui_state_input_delete(ui_state_t *ui) {
    if (!ui || ui->cursor_pos >= ui->input_len) return;
    memmove(ui->input_buffer + ui->cursor_pos,
            ui->input_buffer + ui->cursor_pos + 1,
            ui->input_len - ui->cursor_pos);
    ui->input_len--;
    ui->dirty = 1;
}

void ui_state_input_left(ui_state_t *ui) {
    if (ui && ui->cursor_pos > 0) { ui->cursor_pos--; ui->dirty = 1; }
}
void ui_state_input_right(ui_state_t *ui) {
    if (ui && ui->cursor_pos < ui->input_len) { ui->cursor_pos++; ui->dirty = 1; }
}
void ui_state_input_home(ui_state_t *ui) {
    if (ui) { ui->cursor_pos = 0; ui->dirty = 1; }
}
void ui_state_input_end(ui_state_t *ui) {
    if (ui) { ui->cursor_pos = ui->input_len; ui->dirty = 1; }
}

/* ── React event handler ─────────────────────────────── */

void ui_state_on_event(const react_event_t *ev, void *userdata) {
    ui_state_t *ui = (ui_state_t *)userdata;
    if (!ui) return;

    switch (ev->type) {
    case REACT_EVENT_STEP_START: {
        char buf[128];
        if (ev->max_steps > 0)
            snprintf(buf, sizeof(buf), "Running step %d/%d...",
                     ev->step, ev->max_steps);
        else
            snprintf(buf, sizeof(buf), "Running step %d...",
                     ev->step);
        ui->status = STATUS_RUNNING;
        free(ui->status_text);
        ui->status_text = strdup(buf);
        ui->current_step = ev->step;
        ui->max_steps = ev->max_steps;
        /* Clear streaming tokens for new step */
        if (ui->stream_tokens) ui->stream_tokens[0] = '\0';
        ui->stream_len = 0;
        ui_state_rebuild_md(ui);
        /* Auto-expand current query to show steps during inference */
        if (ui->doc) {
            for (int i = ui->doc->link_count - 1; i >= 0; i--) {
                if (ui->doc->links[i].uri && strstr(ui->doc->links[i].uri, "file://session/R") &&
                    !strstr(ui->doc->links[i].uri, "/S")) {
                    if (i < ui->link_states_count && ui->link_states[i] != LINK_SHOW_STEPS) {
                        ui->link_states[i] = LINK_SHOW_STEPS;
                        ui_state_rebuild_md(ui);
                    }
                    break;
                }
            }
        }
    /* Auto-scroll to keep cursor visible — but NOT during streaming,
     * where ui_state_rebuild_md() already scrolls to bottom */
    if (ui->status != STATUS_RUNNING) {
    if (ui->doc && ui->cursor_link >= 0 && ui->cursor_link < ui->doc->link_count) {
        int link_line = md_link_line(ui->doc, ui->cursor_link);
        int vis = ui->visible_rows > 0 ? ui->visible_rows : 20;
        if (link_line < ui->scroll_y)
            ui->scroll_y = link_line;
        else if (link_line >= ui->scroll_y + vis)
            ui->scroll_y = link_line - vis + 1;
    }
    }
        break;
    }
    case REACT_EVENT_LLM_TOKEN:
        if (ev->token && ev->token[0]) {
            int tlen = (int)strlen(ev->token);
            if (ui->stream_len + tlen >= ui->stream_cap - 1) {
                ui->stream_cap = (ui->stream_len + tlen + 1) * 2;
                ui->stream_tokens = realloc(ui->stream_tokens, ui->stream_cap);
            }
            memcpy(ui->stream_tokens + ui->stream_len, ev->token, tlen);
            ui->stream_len += tlen;
            ui->stream_tokens[ui->stream_len] = '\0';
            ui_state_rebuild_md(ui);
    /* Auto-scroll to keep cursor visible — but NOT during streaming,
     * where ui_state_rebuild_md() already scrolls to bottom */
    if (ui->status != STATUS_RUNNING) {
    if (ui->doc && ui->cursor_link >= 0 && ui->cursor_link < ui->doc->link_count) {
        int link_line = md_link_line(ui->doc, ui->cursor_link);
        int vis = ui->visible_rows > 0 ? ui->visible_rows : 20;
        if (link_line < ui->scroll_y)
            ui->scroll_y = link_line;
        else if (link_line >= ui->scroll_y + vis)
            ui->scroll_y = link_line - vis + 1;
    }
    }
        }
        break;

    case REACT_EVENT_STEP_COMPLETE:
    case REACT_EVENT_TOOL_OUTPUT:
        ui_state_rebuild_md(ui);
    /* Auto-scroll to keep cursor visible — but NOT during streaming,
     * where ui_state_rebuild_md() already scrolls to bottom */
    if (ui->status != STATUS_RUNNING) {
    if (ui->doc && ui->cursor_link >= 0 && ui->cursor_link < ui->doc->link_count) {
        int link_line = md_link_line(ui->doc, ui->cursor_link);
        int vis = ui->visible_rows > 0 ? ui->visible_rows : 20;
        if (link_line < ui->scroll_y)
            ui->scroll_y = link_line;
        else if (link_line >= ui->scroll_y + vis)
            ui->scroll_y = link_line - vis + 1;
    }
    }
        break;

    case REACT_EVENT_DONE:
        ui->status = STATUS_DONE;
        free(ui->status_text);
        ui->status_text = strdup("Done");
        if (ui->stream_tokens) ui->stream_tokens[0] = '\0';
        ui->stream_len = 0;
        ui_state_rebuild_md(ui);
        /* Auto-collapse to show result when done */
        if (ui->doc) {
            for (int i = ui->doc->link_count - 1; i >= 0; i--) {
                if (ui->doc->links[i].uri && strstr(ui->doc->links[i].uri, "file://session/R") &&
                    !strstr(ui->doc->links[i].uri, "/S")) {
                    if (i < ui->link_states_count) {
                        ui->link_states[i] = LINK_SHOW_RESULT;
                        ui_state_rebuild_md(ui);
                    }
                    break;
                }
            }
        }
    /* Auto-scroll to keep cursor visible — but NOT during streaming,
     * where ui_state_rebuild_md() already scrolls to bottom */
    if (ui->status != STATUS_RUNNING) {
    if (ui->doc && ui->cursor_link >= 0 && ui->cursor_link < ui->doc->link_count) {
        int link_line = md_link_line(ui->doc, ui->cursor_link);
        int vis = ui->visible_rows > 0 ? ui->visible_rows : 20;
        if (link_line < ui->scroll_y)
            ui->scroll_y = link_line;
        else if (link_line >= ui->scroll_y + vis)
            ui->scroll_y = link_line - vis + 1;
    }
    }
        break;

    case REACT_EVENT_ERROR:
    case REACT_EVENT_WARNING:
        ui_state_rebuild_md(ui);
    /* Auto-scroll to keep cursor visible — but NOT during streaming,
     * where ui_state_rebuild_md() already scrolls to bottom */
    if (ui->status != STATUS_RUNNING) {
    if (ui->doc && ui->cursor_link >= 0 && ui->cursor_link < ui->doc->link_count) {
        int link_line = md_link_line(ui->doc, ui->cursor_link);
        int vis = ui->visible_rows > 0 ? ui->visible_rows : 20;
        if (link_line < ui->scroll_y)
            ui->scroll_y = link_line;
        else if (link_line >= ui->scroll_y + vis)
            ui->scroll_y = link_line - vis + 1;
    }
    }
        break;
    }
}

/* ── Status & data updates ───────────────────────────── */

void ui_state_set_status(ui_state_t *ui, ui_status_t status, const char *text) {
    if (!ui) return;
    ui->status = status;
    free(ui->status_text);
    ui->status_text = text ? strdup(text) : NULL;
    ui->dirty = 1;
}

void ui_state_set_banner(ui_state_t *ui, const char *banner) {
    if (!ui) return;
    free(ui->banner);
    ui->banner = banner ? strdup(banner) : NULL;
    ui_state_rebuild_md(ui);
    /* Auto-scroll to keep cursor visible — but NOT during streaming,
     * where ui_state_rebuild_md() already scrolls to bottom */
    if (ui->status != STATUS_RUNNING) {
    if (ui->doc && ui->cursor_link >= 0 && ui->cursor_link < ui->doc->link_count) {
        int link_line = md_link_line(ui->doc, ui->cursor_link);
        int vis = ui->visible_rows > 0 ? ui->visible_rows : 20;
        if (link_line < ui->scroll_y)
            ui->scroll_y = link_line;
        else if (link_line >= ui->scroll_y + vis)
            ui->scroll_y = link_line - vis + 1;
    }
    }
}

void ui_state_add_query(ui_state_t *ui, const char *query_text) {
    (void)query_text;
    if (!ui) return;
    /* The query will appear in journal.jsonl after react_run processes it.
     * We auto-expand the latest query during inference. */
    int new_idx = ui->link_states_count;
    if (new_idx >= ui->link_states_cap) {
        ui->link_states_cap = (new_idx + 1) * 2;
        ui->link_states = realloc(ui->link_states,
                                   ui->link_states_cap * sizeof(link_state_t));
    }
    ui->link_states[new_idx] = LINK_SHOW_STEPS;  /* auto-expand new query */
    ui->link_states_count = new_idx + 1;
    ui->cursor_link = new_idx;
    ui_state_rebuild_md(ui);
    /* Auto-scroll to keep cursor visible — but NOT during streaming,
     * where ui_state_rebuild_md() already scrolls to bottom */
    if (ui->status != STATUS_RUNNING) {
    if (ui->doc && ui->cursor_link >= 0 && ui->cursor_link < ui->doc->link_count) {
        int link_line = md_link_line(ui->doc, ui->cursor_link);
        int vis = ui->visible_rows > 0 ? ui->visible_rows : 20;
        if (link_line < ui->scroll_y)
            ui->scroll_y = link_line;
        else if (link_line >= ui->scroll_y + vis)
            ui->scroll_y = link_line - vis + 1;
    }
    }
}

void ui_state_load_journal(ui_state_t *ui, journal_t *journal) {
    if (!ui) return;
    int first_load = (ui->journal == NULL);
    ui->journal = journal;
    ui_state_rebuild_md(ui);

    if (first_load) {
        /* First load: show banner at top, cursor on first link */
        ui->scroll_y = 0;
        ui->cursor_link = 0;
    } else {
        /* Subsequent reloads (after query completes): keep cursor visible */
        if (ui->doc && ui->cursor_link >= 0 && ui->cursor_link < ui->doc->link_count) {
            int link_line = md_link_line(ui->doc, ui->cursor_link);
            int vis = ui->visible_rows > 0 ? ui->visible_rows : 20;
            if (link_line < ui->scroll_y)
                ui->scroll_y = link_line;
            else if (link_line >= ui->scroll_y + vis)
                ui->scroll_y = link_line - vis + 1;
        }
    }
}
