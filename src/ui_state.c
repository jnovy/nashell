#include "ui_state.h"
#include "md_render.h"
#include "journal.h"
#include "str.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <libgen.h>

/* ── helpers ─────────────────────────────────────────────── */

/* Sanitize text for use inside MD link [text](uri) syntax. */
static const char *sanitize_md_link(const char *text) {
    static char buf[512];
    int j = 0;
    if (!text) return "";
    for (int i = 0; text[i] && j < (int)sizeof(buf) - 1; i++) {
        switch (text[i]) {
            case ']': buf[j++] = ')'; break;
            case '[': buf[j++] = '('; break;
            case '\n': buf[j++] = ' '; break;
            case '\r': break;
            default: buf[j++] = text[i]; break;
        }
    }
    buf[j] = '\0';
    return buf;
}

/* Read last N lines from a file. Returns malloc'd string or NULL. */
static char *read_last_lines(const char *path, int n_lines) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;

    /* Read entire file (cap at 64KB for preview) */
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0) { fclose(f); return strdup(""); }
    if (sz > 65536) {
        fseek(f, sz - 65536, SEEK_SET);
        sz = 65536;
    } else {
        fseek(f, 0, SEEK_SET);
    }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';

    /* Find the start of the last n_lines */
    int count = 0;
    char *p = buf + rd;
    /* Skip trailing newline */
    if (p > buf && *(p-1) == '\n') p--;
    while (p > buf && count < n_lines) {
        p--;
        if (*p == '\n') count++;
    }
    if (*p == '\n') p++;

    char *result = strdup(p);
    free(buf);
    return result;
}

/* Read entire file. Returns malloc'd string or NULL. */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0) { fclose(f); return strdup(""); }
    if (sz > 1048576) sz = 1048576; /* cap at 1MB */
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    return buf;
}

/* Write string to file atomically (write to .tmp, rename). */
static void write_md_file(const char *path, const char *content) {
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    if (content) fputs(content, f);
    fclose(f);
    rename(tmp, path);
}

/* Extract tool description from journal params (for step display). */
static const char *extract_desc(const char *tool, cJSON *params) {
    if (!params) return "";
    cJSON *cmd  = cJSON_GetObjectItem(params, "command");
    cJSON *path = cJSON_GetObjectItem(params, "path");
    cJSON *pat  = cJSON_GetObjectItem(params, "pattern");
    cJSON *qry  = cJSON_GetObjectItem(params, "query");
    cJSON *res  = cJSON_GetObjectItem(params, "result");
    cJSON *url  = cJSON_GetObjectItem(params, "url");

    if (strcmp(tool, "grep_search") == 0 && pat && pat->valuestring) {
        static char grep_desc[256];
        if (path && path->valuestring && path->valuestring[0])
            snprintf(grep_desc, sizeof(grep_desc), "%s in %s",
                     pat->valuestring, path->valuestring);
        else
            snprintf(grep_desc, sizeof(grep_desc), "%s", pat->valuestring);
        return grep_desc;
    }
    if (strcmp(tool, "file_read") == 0 && path && path->valuestring) {
        cJSON *sl = cJSON_GetObjectItem(params, "start_line");
        cJSON *el = cJSON_GetObjectItem(params, "end_line");
        if (sl || el) {
            static char fr_desc[256];
            int s = sl ? (int)cJSON_GetNumberValue(sl) : 0;
            int e = el ? (int)cJSON_GetNumberValue(el) : 0;
            if (s > 0 && e > 0)
                snprintf(fr_desc, sizeof(fr_desc), "%s:%d-%d", path->valuestring, s, e);
            else if (s > 0)
                snprintf(fr_desc, sizeof(fr_desc), "%s:%d-EOF", path->valuestring, s);
            else
                snprintf(fr_desc, sizeof(fr_desc), "%s", path->valuestring);
            return fr_desc;
        }
        return path->valuestring;
    }
    if ((strcmp(tool, "web_fetch") == 0 || strcmp(tool, "web_search") == 0) &&
        url && url->valuestring)
        return url->valuestring;
    /* System log entries: show the message */
    if (strcmp(tool, "log") == 0) {
        cJSON *msg = cJSON_GetObjectItem(params, "message");
        if (msg && msg->valuestring) return msg->valuestring;
        return "(system log)";
    }
    /* Truncate done/plan result to first line, max 80 chars */
    if ((strcmp(tool, "done") == 0 || strcmp(tool, "plan") == 0)
        && res && res->valuestring) {
        static char trunc_desc[128];
        const char *s = res->valuestring;
        /* Find first newline */
        const char *nl = strchr(s, '\n');
        int len = nl ? (int)(nl - s) : (int)strlen(s);
        if (len > 80) len = 80;
        snprintf(trunc_desc, sizeof(trunc_desc), "%.*s%s",
                 len, s, (nl || (int)strlen(s) > 80) ? "..." : "");
        return trunc_desc;
    }
    if (cmd && cmd->valuestring) return cmd->valuestring;
    if (path && path->valuestring) return path->valuestring;
    if (pat && pat->valuestring) return pat->valuestring;
    if (qry && qry->valuestring) return qry->valuestring;
    if (res && res->valuestring) return res->valuestring;
    cJSON *err = cJSON_GetObjectItem(params, "error");
    if (err && err->valuestring) return err->valuestring;
    return "";
}

/* Extract thought from params, unwrapping nested JSON if needed.
 * Delegates to the shared unwrap_thought() in journal.c. */
static char *extract_thought(cJSON *params) {
    if (!params) return NULL;
    cJSON *th = cJSON_GetObjectItem(params, "thought");
    if (!th || !th->valuestring || !th->valuestring[0]) return NULL;

    char *clean = unwrap_thought(th->valuestring);
    return clean ? clean : strdup(th->valuestring);
}

/* ── lifecycle ─────────────────────────────────────────────── */

ui_state_t *ui_state_new(const char *session_dir, store_t *store) {
    ui_state_t *ui = calloc(1, sizeof(*ui));
    if (!ui) return NULL;
    ui->session_dir = session_dir ? strdup(session_dir) : NULL;
    ui->store = store;
    ui->focus = FOCUS_QUERY;
    ui->status = STATUS_READY;
    ui->status_text = strdup("Ready");
    ui->input_cap = 4096;
    ui->input_buffer = calloc(1, (size_t)ui->input_cap);
    ui->stream_cap = 8192;
    ui->stream_tokens = calloc(1, (size_t)ui->stream_cap);
    ui->cursor_link = 0;
    ui->nav_cap = 16;
    ui->nav_stack = calloc((size_t)ui->nav_cap, sizeof(nav_entry_t));
    ui->nav_depth = 0;
    ui->current_react_loop = -1;

    /* Set initial file to session.md */
    if (session_dir) {
        char path[4096];
        snprintf(path, sizeof(path), "%s/session.md", session_dir);
        ui->current_filepath = strdup(path);
    }

    pthread_mutex_init(&ui->mtx, NULL);
    return ui;
}

void ui_state_free(ui_state_t *ui) {
    if (!ui) return;
    md_doc_free(ui->doc);
    free(ui->banner);
    free(ui->session_dir);
    free(ui->status_text);
    free(ui->input_buffer);
    free(ui->stream_tokens);
    free(ui->model_name);
    free(ui->current_filepath);
    free(ui->user_ask_question);
    for (int i = 0; i < ui->nav_depth; i++)
        free(ui->nav_stack[i].filepath);
    free(ui->nav_stack);
    for (int i = 0; i < ui->history_count; i++)
        free(ui->history[i]);
    free(ui->history);
    for (int i = 0; i < ui->expanded_count; i++)
        free(ui->expanded_uris[i]);
    free(ui->expanded_uris);
    pthread_mutex_destroy(&ui->mtx);
    free(ui);
}

/* ── MD file generation ──────────────────────────────────── */

void ui_state_generate_session_md(ui_state_t *ui) {
    if (!ui || !ui->session_dir) return;

    str_t md = str_new(8192);

    /* Banner */
    if (ui->banner && ui->banner[0]) {
        str_append_cstr(&md, ui->banner);
        str_append_cstr(&md, "\n---\n\n");
    }

    /* Read journal for query list */
    char jpath[4096];
    snprintf(jpath, sizeof(jpath), "%s/journal.jsonl", ui->session_dir);
    FILE *f = fopen(jpath, "r");
    if (!f) {
        if (md.len == 0)
            str_append_cstr(&md, "# Nash\n\n*No session loaded*\n");
        goto write_out;
    }

    /* Collect query info */
    typedef struct {
        char *text;
        double ts;
        int react_loop;
        int step_count;
        char *result;
        int done;
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
            if (qcount >= qcap) {
                qcap = qcap ? qcap * 2 : 16;
                qinfos = realloc(qinfos, (size_t)qcap * sizeof(qinfo_t));
            }
            qinfo_t *qi = &qinfos[qcount++];
            memset(qi, 0, sizeof(*qi));
            cJSON *params = cJSON_GetObjectItem(entry, "params");
            cJSON *text = params ? cJSON_GetObjectItem(params, "text") : NULL;
            qi->text = (text && text->valuestring) ? strdup(text->valuestring) : strdup("?");
            cJSON *ts = cJSON_GetObjectItem(entry, "ts");
            qi->ts = ts && ts->valuestring ? atof(ts->valuestring) : 0;
            qi->react_loop = loop;
        } else if (tool && strcmp(tool, "query") != 0 && strcmp(tool, "system") != 0) {
            for (int i = qcount - 1; i >= 0; i--) {
                if (qinfos[i].react_loop == loop) {
                    qinfos[i].step_count++;
                    if (strcmp(tool, "done") == 0) {
                        qinfos[i].done = 1;
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

    str_append_cstr(&md, "## Session History\n\n");

    for (int i = 0; i < qcount; i++) {
        qinfo_t *qi = &qinfos[i];

        /* Format timestamp */
        char ts_buf[32] = "";
        if (qi->ts > 0) {
            time_t t = (time_t)qi->ts;
            struct tm *tm = localtime(&t);
            if (tm) strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%d %H:%M", tm);
        }

        /* Status icon */
        int is_active = (ui->status == STATUS_RUNNING &&
                         qi->react_loop == ui->current_react_loop);
        const char *icon = is_active ? "⟳" : (qi->done ? "✓" : "▶");

        /* Query as hyperlink to reactRX.md */
        str_appendf(&md, "[%s %s  %s](reactR%d.md)\n",
                    icon, ts_buf, sanitize_md_link(qi->text), qi->react_loop);

        /* Preview: show for the ACTIVE react loop, or if user toggled
         * with 'c' key (URI in expanded_uris). */
        char react_uri[64];
        snprintf(react_uri, sizeof(react_uri), "reactR%d.md", qi->react_loop);
        int is_expanded = 0;
        for (int ei = 0; ei < ui->expanded_count; ei++) {
            if (strcmp(ui->expanded_uris[ei], react_uri) == 0) {
                is_expanded = 1;
                break;
            }
        }
        if (is_active || is_expanded) {
            char rpath[4096];
            snprintf(rpath, sizeof(rpath), "%s/reactR%d.md",
                     ui->session_dir, qi->react_loop);
            char *preview = read_last_lines(rpath, 10);
            if (preview && preview[0]) {
                str_append_cstr(&md, preview);
                /* Ensure trailing newline */
                if (preview[strlen(preview) - 1] != '\n')
                    str_append_cstr(&md, "\n");
            }
            free(preview);
        }
    }

    for (int i = 0; i < qcount; i++) {
        free(qinfos[i].text);
        free(qinfos[i].result);
    }
    free(qinfos);

write_out:;
    char *md_str = str_steal(&md);
    char spath[4096];
    snprintf(spath, sizeof(spath), "%s/session.md", ui->session_dir);
    write_md_file(spath, md_str);
    free(md_str);
}

void ui_state_generate_react_md(ui_state_t *ui, int react_loop) {
    if (!ui || !ui->session_dir) return;

    str_t md = str_new(8192);

    /* Read journal for this react loop's entries */
    char jpath[4096];
    snprintf(jpath, sizeof(jpath), "%s/journal.jsonl", ui->session_dir);
    FILE *f = fopen(jpath, "r");
    if (!f) return;

    /* Collect all entries for this react loop into an array */
    typedef struct {
        char *tool;
        char *desc;
        char *thought;
        char *ref;
        int   step;
        int   size;
        int   failed;
        double ts;
    } step_info_t;

    step_info_t *steps = NULL;
    int nsteps = 0, scap = 0;
    char *query_text = NULL;
    char line[65536];

    while (fgets(line, sizeof(line), f)) {
        cJSON *entry = cJSON_Parse(line);
        if (!entry) continue;

        int loop = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(entry, "react_loop"));
        const char *tool = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "tool"));

        if (loop != react_loop || !tool) {
            cJSON_Delete(entry);
            continue;
        }

        if (strcmp(tool, "query") == 0) {
            cJSON *params = cJSON_GetObjectItem(entry, "params");
            cJSON *text = params ? cJSON_GetObjectItem(params, "text") : NULL;
            if (text && text->valuestring) {
                free(query_text);
                query_text = strdup(text->valuestring);
            }
            cJSON_Delete(entry);
            continue;
        }

        if (strcmp(tool, "system") == 0) {
            cJSON_Delete(entry);
            continue;
        }

        /* Collect step info */
        if (nsteps >= scap) {
            scap = scap ? scap * 2 : 32;
            steps = realloc(steps, (size_t)scap * sizeof(step_info_t));
        }
        step_info_t *si = &steps[nsteps++];
        memset(si, 0, sizeof(*si));

        si->tool = strdup(tool);
        si->step = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(entry, "step"));
        cJSON *ts_j = cJSON_GetObjectItem(entry, "ts");
        si->ts = (ts_j && ts_j->valuestring) ? atof(ts_j->valuestring) : 0;
        const char *ref = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "ref"));
        si->ref = ref ? strdup(ref) : NULL;
        si->size = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(entry, "size"));
        cJSON *failed_j = cJSON_GetObjectItem(entry, "failed");
        si->failed = (failed_j && cJSON_IsTrue(failed_j));

        cJSON *params = cJSON_GetObjectItem(entry, "params");
        char *thought = extract_thought(params);
        si->thought = thought;
        si->desc = strdup(extract_desc(tool, params));

        cJSON_Delete(entry);
    }
    fclose(f);

    /* Header */
    str_appendf(&md, "# Query: %s\n\n", query_text ? query_text : "?");

    /* Render each step in nashell-style compact format */
    for (int i = 0; i < nsteps; i++) {
        step_info_t *si = &steps[i];
        int is_last = (i == nsteps - 1);

        /* Compute elapsed time from previous step */
        double elapsed = 0;
        if (i > 0 && si->ts > 0 && steps[i-1].ts > 0)
            elapsed = si->ts - steps[i-1].ts;

        /* Format timestamp HH:MM */
        char ts_buf[8] = "";
        if (si->ts > 0) {
            time_t t = (time_t)si->ts;
            struct tm *tm = localtime(&t);
            if (tm) strftime(ts_buf, sizeof(ts_buf), "%H:%M", tm);
        }

        /* Status icon */
        const char *icon = si->failed ? "\xe2\x9c\x97" : "\xe2\x9c\x93";

        /* Format elapsed */
        char elapsed_str[32] = "";
        if (elapsed > 0) {
            int es = (int)elapsed;
            if (es > 0)
                snprintf(elapsed_str, sizeof(elapsed_str), " (%ds)", es);
        }

        /* Truncate desc to ~60 chars */
        char desc_trunc[80];
        if (si->desc && si->desc[0]) {
            /* Replace newlines with spaces */
            int j = 0;
            for (int k = 0; si->desc[k] && j < 60; k++) {
                if (si->desc[k] == '\n' || si->desc[k] == '\r')
                    desc_trunc[j++] = ' ';
                else
                    desc_trunc[j++] = si->desc[k];
            }
            desc_trunc[j] = '\0';
            if ((int)strlen(si->desc) > 60)
                strcat(desc_trunc, "...");
        } else {
            desc_trunc[0] = '\0';
        }

        /* Prepare thought text: strip leading/trailing whitespace/newlines */
        const char *thought_start = si->thought;
        int tlen = 0;
        if (thought_start && thought_start[0]) {
            while (*thought_start == '\n' || *thought_start == '\r' ||
                   *thought_start == ' ' || *thought_start == '\t')
                thought_start++;
            tlen = (int)strlen(thought_start);
            while (tlen > 0 && (thought_start[tlen-1] == '\n' ||
                   thought_start[tlen-1] == '\r' ||
                   thought_start[tlen-1] == ' ' ||
                   thought_start[tlen-1] == '\t'))
                tlen--;
        }

        /* Build the step line */
        /* Format: [  icon  step HH:MM tool_name    thought (or desc)](ref)
         *         then desc (or thought) on indented second line
         *         shell_exec: command shown on second line as **`cmd`** */
        int is_shell = (strcmp(si->tool, "shell_exec") == 0);
        if (is_shell && tlen > 0) {
            /* shell_exec with thought: show "thought" on main line,
             * command on second line as bold code */
            char thought_trunc[128];
            if (tlen <= 120) {
                snprintf(thought_trunc, sizeof(thought_trunc), "\"%.*s\"", tlen, thought_start);
            } else {
                snprintf(thought_trunc, sizeof(thought_trunc), "\"%.114s...\"", thought_start);
            }
            if (si->ref) {
                str_appendf(&md, "[  %s %3d %s %-13s %s%s](%s)\n",
                            icon, si->step, ts_buf,
                            si->tool, sanitize_md_link(thought_trunc),
                            elapsed_str, si->ref);
            } else {
                str_appendf(&md, "  %s %3d %s %-13s %s%s\n",
                            icon, si->step, ts_buf,
                            si->tool, thought_trunc, elapsed_str);
            }
            if (desc_trunc[0]) {
                str_appendf(&md, "                **`%s`**\n", desc_trunc);
            }
        } else if (is_shell && desc_trunc[0]) {
            /* shell_exec without thought: command on same line as tool */
            if (si->ref) {
                str_appendf(&md, "[  %s %3d %s %-13s %s%s](%s)\n",
                            icon, si->step, ts_buf,
                            si->tool, sanitize_md_link(desc_trunc),
                            elapsed_str, si->ref);
            } else {
                str_appendf(&md, "  %s %3d %s %-13s %s%s\n",
                            icon, si->step, ts_buf,
                            si->tool, desc_trunc, elapsed_str);
            }
        } else if (tlen > 0) {
            /* Has thought: show thought on main line, desc on second line */
            char thought_trunc[128];
            if (tlen <= 120) {
                snprintf(thought_trunc, sizeof(thought_trunc), "%.*s", tlen, thought_start);
            } else {
                snprintf(thought_trunc, sizeof(thought_trunc), "%.117s...", thought_start);
            }
            if (si->ref) {
                str_appendf(&md, "[  %s %3d %s %-13s %s](%s)\n",
                            icon, si->step, ts_buf,
                            si->tool, sanitize_md_link(thought_trunc),
                            si->ref);
            } else {
                str_appendf(&md, "  %s %3d %s %-13s %s\n",
                            icon, si->step, ts_buf,
                            si->tool, thought_trunc);
            }
            if (desc_trunc[0]) {
                str_appendf(&md, "                %s%s\n", desc_trunc, elapsed_str);
            }
        } else {
            /* No thought: show desc on main line (original behavior) */
            if (si->ref) {
                str_appendf(&md, "[  %s %3d %s %-13s %s%s](%s)\n",
                            icon, si->step, ts_buf,
                            si->tool, sanitize_md_link(desc_trunc),
                            elapsed_str, si->ref);
            } else {
                str_appendf(&md, "  %s %3d %s %-13s %s%s\n",
                            icon, si->step, ts_buf,
                            si->tool, desc_trunc, elapsed_str);
            }
        }

        /* Preview: show for last step or explicitly expanded steps */
        int show_preview = is_last;
        if (!show_preview && si->ref) {
            for (int ei = 0; ei < ui->expanded_count; ei++) {
                if (strcmp(ui->expanded_uris[ei], si->ref) == 0) {
                    show_preview = 1;
                    break;
                }
            }
        }

        if (show_preview && si->ref) {
            char rpath[4096];
            snprintf(rpath, sizeof(rpath), "%s/%s", ui->session_dir, si->ref);
            FILE *cf = fopen(rpath, "r");
            if (cf) {
                if (strcmp(si->tool, "file_edit") == 0)
                    str_append_cstr(&md, "```diff\n");
                else
                    str_append_cstr(&md, "```\n");
                char cbuf[4096];
                int line_count = 0;
                size_t total = 0;
                size_t n;
                while ((n = fread(cbuf, 1, sizeof(cbuf)-1, cf)) > 0
                       && total < 8000 && line_count < 5) {
                    cbuf[n] = '\0';
                    for (size_t k = 0; k < n && line_count < 5; k++) {
                        str_append(&md, &cbuf[k], 1);
                        total++;
                        if (cbuf[k] == '\n') line_count++;
                    }
                }
                long file_sz = 0;
                fseek(cf, 0, SEEK_END);
                file_sz = ftell(cf);
                if (total < (size_t)file_sz)
                    str_append_cstr(&md, "  ...\n");
                if (md.len > 0 && md.data[md.len - 1] != '\n')
                    str_append_cstr(&md, "\n");
                str_append_cstr(&md, "```\n");
                fclose(cf);
            }
        }
    }

    /* Streaming indicator if actively running */
    if (ui->status == STATUS_RUNNING &&
        ui->current_react_loop == react_loop) {
        if (ui->max_steps > 0)
            str_appendf(&md, "  | %3d %s %-13s processing...\n",
                        ui->current_step, "", "");
        else
            str_appendf(&md, "  | %3d        %-13s processing...\n",
                        ui->current_step, "");
        if (ui->stream_tokens && ui->stream_len > 0) {
            str_append_cstr(&md, "```\n");
            str_append(&md, ui->stream_tokens, (size_t)ui->stream_len);
            str_append_cstr(&md, "\n```\n");
        }
    }

    /* user_ask: display full question in main pane */
    if (ui->status == STATUS_AWAITING_INPUT &&
        ui->current_react_loop == react_loop &&
        ui->user_ask_question && ui->user_ask_question[0]) {
        str_append_cstr(&md, "\n---\n\n");
        str_append_cstr(&md, "## \xf0\x9f\xa4\x94 Agent Question\n\n");
        str_append_cstr(&md, ui->user_ask_question);
        str_append_cstr(&md, "\n\n---\n");
        str_append_cstr(&md, "*Type your answer in the input bar below and press Enter*\n");
    }

    /* Free collected steps */
    for (int i = 0; i < nsteps; i++) {
        free(steps[i].tool);
        free(steps[i].desc);
        free(steps[i].thought);
        free(steps[i].ref);
    }
    free(steps);
    free(query_text);

    char *md_str = str_steal(&md);
    char rpath[4096];
    snprintf(rpath, sizeof(rpath), "%s/reactR%d.md",
             ui->session_dir, react_loop);
    write_md_file(rpath, md_str);
    free(md_str);
}

void ui_state_reload_file(ui_state_t *ui) {
    if (!ui || !ui->current_filepath) return;

    char *content = read_file(ui->current_filepath);
    if (!content) content = strdup("*File not found*\n");

    md_doc_free(ui->doc);
    ui->doc = md_parse(content);
    free(content);

    /* Clamp cursor */
    if (ui->doc && ui->cursor_link >= ui->doc->link_count)
        ui->cursor_link = ui->doc->link_count > 0 ? ui->doc->link_count - 1 : 0;

    ui->dirty = 1;
}

/* ── Navigation ──────────────────────────────────────────── */

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

void ui_state_tab(ui_state_t *ui) {
    if (!ui) return;
    ui->focus = (ui->focus == FOCUS_JOURNAL) ? FOCUS_QUERY : FOCUS_JOURNAL;

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

void ui_state_up(ui_state_t *ui) {
    if (!ui) return;
    if (ui->focus == FOCUS_JOURNAL && ui->doc) {
        int vis = ui->visible_rows > 0 ? ui->visible_rows : 20;
        int first_vis, last_vis;
        int n_vis = find_visible_links(ui->doc, ui->scroll_y, vis,
                                        &first_vis, &last_vis);

        if (n_vis == 0) {
            if (ui->scroll_y > 0) ui->scroll_y--;
        } else if (ui->cursor_link <= first_vis) {
            if (ui->scroll_y > 0) ui->scroll_y--;
            n_vis = find_visible_links(ui->doc, ui->scroll_y, vis,
                                        &first_vis, &last_vis);
            if (n_vis > 0 && first_vis < ui->cursor_link)
                ui->cursor_link = first_vis;
        } else {
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
        int max_scroll = ui->doc->total_lines - vis;
        if (max_scroll < 0) max_scroll = 0;
        int first_vis, last_vis;
        int n_vis = find_visible_links(ui->doc, ui->scroll_y, vis,
                                        &first_vis, &last_vis);

        if (n_vis == 0) {
            if (ui->scroll_y < max_scroll) ui->scroll_y++;
        } else if (ui->cursor_link >= last_vis) {
            if (ui->scroll_y < max_scroll) ui->scroll_y++;
            n_vis = find_visible_links(ui->doc, ui->scroll_y, vis,
                                        &first_vis, &last_vis);
            if (n_vis > 0 && ui->cursor_link < last_vis)
                ui->cursor_link = last_vis;
        } else {
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
    if (idx < 0 || idx >= ui->doc->link_count) return;

    const char *uri = ui->doc->links[idx].uri;
    if (!uri) return;

    /* Check if URI points to a .md file */
    int len = (int)strlen(uri);
    if (len >= 3 && strcmp(uri + len - 3, ".md") == 0) {
        /* Navigate into the .md file */

        /* Save current state to nav stack */
        if (ui->nav_depth >= ui->nav_cap) {
            ui->nav_cap = ui->nav_cap ? ui->nav_cap * 2 : 16;
            ui->nav_stack = realloc(ui->nav_stack,
                                     (size_t)ui->nav_cap * sizeof(nav_entry_t));
        }
        nav_entry_t *entry = &ui->nav_stack[ui->nav_depth];
        entry->filepath = ui->current_filepath ? strdup(ui->current_filepath) : NULL;
        entry->scroll_y = ui->scroll_y;
        entry->scroll_x = ui->scroll_x;
        entry->cursor_link = ui->cursor_link;
        ui->nav_depth++;

        /* Resolve URI relative to current file's directory */
        char new_path[4096];
        if (uri[0] == '/') {
            /* Absolute path */
            snprintf(new_path, sizeof(new_path), "%s", uri);
        } else {
            /* Relative to session_dir (since session.md is there) */
            snprintf(new_path, sizeof(new_path), "%s/%s",
                     ui->session_dir, uri);
        }

        free(ui->current_filepath);
        ui->current_filepath = strdup(new_path);
        ui->scroll_y = 0;
        ui->scroll_x = 0;
        ui->cursor_link = 0;

        ui_state_reload_file(ui);
        return;  /* done — don't fall through to raw file handler */
    }
    /* Non-.md links: treat as raw file (store ref like R0S3) */
    /* Resolve relative to session_dir and display content */
    char raw_path[4096];
    if (uri[0] == '/') {
        snprintf(raw_path, sizeof(raw_path), "%s", uri);
    } else {
        snprintf(raw_path, sizeof(raw_path), "%s/%s",
                 ui->session_dir, uri);
    }

    /* Check file exists */
    FILE *test = fopen(raw_path, "r");
    if (!test) return;
    fclose(test);

    /* Save current state to nav stack */
    if (ui->nav_depth >= ui->nav_cap) {
        ui->nav_cap = ui->nav_cap ? ui->nav_cap * 2 : 16;
        ui->nav_stack = realloc(ui->nav_stack,
                                 (size_t)ui->nav_cap * sizeof(nav_entry_t));
    }
    nav_entry_t *raw_entry = &ui->nav_stack[ui->nav_depth];
    raw_entry->filepath = ui->current_filepath ? strdup(ui->current_filepath) : NULL;
    raw_entry->scroll_y = ui->scroll_y;
    raw_entry->scroll_x = ui->scroll_x;
    raw_entry->cursor_link = ui->cursor_link;
    ui->nav_depth++;

    /* Read file content and wrap in MD */
    char *raw_content = read_file(raw_path);
    if (!raw_content) raw_content = strdup("*Empty*\n");

    str_t wrapped = str_new(strlen(raw_content) + 256);
    /* Extract just the filename for the heading */
    const char *fname = strrchr(uri, '/');
    fname = fname ? fname + 1 : uri;
    str_appendf(&wrapped, "# %s\n\n", fname);
    /* Render content as markdown — store refs may contain formatted
     * done results, plans, or other markdown-rich text. Wrapping in
     * a code fence would suppress all formatting (bold, headers, etc.). */
    str_append_cstr(&wrapped, raw_content);
    str_append_cstr(&wrapped, "\n");
    free(raw_content);

    char *md_source = str_steal(&wrapped);
    md_doc_free(ui->doc);
    ui->doc = md_parse(md_source);
    free(md_source);

    free(ui->current_filepath);
    ui->current_filepath = strdup(raw_path);
    ui->scroll_y = 0;
    ui->scroll_x = 0;
    ui->cursor_link = 0;
    ui->dirty = 1;
}

/* Toggle collapse/expand preview for the currently selected link */
void ui_state_toggle_preview(ui_state_t *ui) {
    if (!ui || !ui->doc || ui->doc->link_count == 0) return;
    if (ui->focus != FOCUS_JOURNAL) return;

    int idx = ui->cursor_link;
    if (idx < 0 || idx >= ui->doc->link_count) return;

    const char *uri = ui->doc->links[idx].uri;
    if (!uri || !uri[0]) return;

    /* Check if already expanded — if so, remove it */
    for (int i = 0; i < ui->expanded_count; i++) {
        if (strcmp(ui->expanded_uris[i], uri) == 0) {
            free(ui->expanded_uris[i]);
            ui->expanded_uris[i] = ui->expanded_uris[--ui->expanded_count];
            goto regen;
        }
    }

    /* Not expanded — add it */
    if (ui->expanded_count >= ui->expanded_cap) {
        ui->expanded_cap = ui->expanded_cap ? ui->expanded_cap * 2 : 16;
        ui->expanded_uris = realloc(ui->expanded_uris,
                                     (size_t)ui->expanded_cap * sizeof(char *));
    }
    ui->expanded_uris[ui->expanded_count++] = strdup(uri);

regen:
    /* Regenerate the current file to reflect the change */
    if (ui->nav_depth > 0) {
        /* Inside a reactRX.md — figure out which react loop */
        if (ui->current_filepath) {
            const char *r = strstr(ui->current_filepath, "reactR");
            if (r) {
                int loop = atoi(r + 6);
                ui_state_generate_react_md(ui, loop);
            }
        }
    } else {
        ui_state_generate_session_md(ui);
    }
    ui_state_reload_file(ui);
    ui->dirty = 1;
}

void ui_state_back(ui_state_t *ui) {
    if (!ui) return;
    if (ui->focus != FOCUS_JOURNAL) return;

    if (ui->nav_depth > 0) {
        /* Pop nav stack */
        ui->nav_depth--;
        nav_entry_t *entry = &ui->nav_stack[ui->nav_depth];

        free(ui->current_filepath);
        ui->current_filepath = entry->filepath;
        entry->filepath = NULL;
        ui->scroll_y = entry->scroll_y;
        ui->scroll_x = entry->scroll_x;
        ui->cursor_link = entry->cursor_link;

        /* Just reload from disk — session.md is already there.
         * The 1-second timer refresh will regenerate it if the react
         * loop has progressed. Calling generate_session_md() here was
         * expensive: it re-reads journal.jsonl + all reactRX.md files
         * for previews, making Esc noticeably slow. */
        ui_state_reload_file(ui);
    }
    ui->dirty = 1;
}

void ui_state_page_up(ui_state_t *ui) {
    if (!ui) return;
    int page = (ui->visible_rows > 3) ? (ui->visible_rows / 3) : 3;
    ui->scroll_y -= page;
    if (ui->scroll_y < 0) ui->scroll_y = 0;

    if (ui->doc && ui->doc->link_count > 0) {
        int link_line = md_link_line(ui->doc, ui->cursor_link);
        int vis = ui->visible_rows > 0 ? ui->visible_rows : 20;
        if (link_line >= ui->scroll_y + vis) {
            for (int i = ui->doc->link_count - 1; i >= 0; i--) {
                int ll = md_link_line(ui->doc, i);
                if (ll >= ui->scroll_y && ll < ui->scroll_y + vis) {
                    ui->cursor_link = i;
                    break;
                }
            }
        } else if (link_line < ui->scroll_y) {
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
    int page = (ui->visible_rows > 3) ? (ui->visible_rows / 3) : 3;
    ui->scroll_y += page;

    if (ui->doc) {
        int vis = ui->visible_rows > 0 ? ui->visible_rows : 20;
        int max_scroll = ui->doc->total_lines - vis;
        if (max_scroll < 0) max_scroll = 0;
        if (ui->scroll_y > max_scroll) ui->scroll_y = max_scroll;
    }

    if (ui->doc && ui->doc->link_count > 0) {
        int link_line = md_link_line(ui->doc, ui->cursor_link);
        int vis = ui->visible_rows > 0 ? ui->visible_rows : 20;
        if (link_line < ui->scroll_y) {
            for (int i = 0; i < ui->doc->link_count; i++) {
                int ll = md_link_line(ui->doc, i);
                if (ll >= ui->scroll_y && ll < ui->scroll_y + vis) {
                    ui->cursor_link = i;
                    break;
                }
            }
        } else if (link_line >= ui->scroll_y + vis) {
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

/* ── Input editing ─────────────────────────────────────────── */

void ui_state_input_char(ui_state_t *ui, int ch) {
    if (!ui || ch < 32 || ch > 126) return;
    if (ui->input_len >= ui->input_cap - 1) {
        ui->input_cap *= 2;
        ui->input_buffer = realloc(ui->input_buffer, (size_t)ui->input_cap);
    }
    memmove(ui->input_buffer + ui->cursor_pos + 1,
            ui->input_buffer + ui->cursor_pos,
            (size_t)(ui->input_len - ui->cursor_pos + 1));
    ui->input_buffer[ui->cursor_pos] = (char)ch;
    ui->cursor_pos++;
    ui->input_len++;
    ui->dirty = 1;
}

void ui_state_input_backspace(ui_state_t *ui) {
    if (!ui || ui->cursor_pos <= 0) return;
    memmove(ui->input_buffer + ui->cursor_pos - 1,
            ui->input_buffer + ui->cursor_pos,
            (size_t)(ui->input_len - ui->cursor_pos + 1));
    ui->cursor_pos--;
    ui->input_len--;
    ui->dirty = 1;
}

void ui_state_input_delete(ui_state_t *ui) {
    if (!ui || ui->cursor_pos >= ui->input_len) return;
    memmove(ui->input_buffer + ui->cursor_pos,
            ui->input_buffer + ui->cursor_pos + 1,
            (size_t)(ui->input_len - ui->cursor_pos));
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

/* ── React event handler ─────────────────────────────────── */

/* Check if user is currently viewing the given react loop's file */
static int viewing_react_file(ui_state_t *ui, int react_loop) {
    if (!ui->current_filepath) return 0;
    char expected[64];
    snprintf(expected, sizeof(expected), "reactR%d.md", react_loop);
    const char *base = strrchr(ui->current_filepath, '/');
    base = base ? base + 1 : ui->current_filepath;
    return strcmp(base, expected) == 0;
}

/* Check if user is viewing session.md */
static int viewing_session(ui_state_t *ui) {
    if (!ui->current_filepath) return 0;
    const char *base = strrchr(ui->current_filepath, '/');
    base = base ? base + 1 : ui->current_filepath;
    return strcmp(base, "session.md") == 0;
}

/* Auto-scroll to bottom of document */
static void auto_scroll_bottom(ui_state_t *ui) {
    if (!ui->doc) return;
    int vis = ui->visible_rows > 0 ? ui->visible_rows : 20;
    int max_scroll = ui->doc->total_lines - vis;
    if (max_scroll < 0) max_scroll = 0;
    ui->scroll_y = max_scroll;
    /* Move cursor to last link */
    if (ui->doc->link_count > 0)
        ui->cursor_link = ui->doc->link_count - 1;
}

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
            snprintf(buf, sizeof(buf), "Running step %d...", ev->step);
        ui->status = STATUS_RUNNING;
        free(ui->status_text);
        ui->status_text = strdup(buf);
        ui->current_step = ev->step;
        ui->max_steps = ev->max_steps;
        if (ev->context_size > 0)
            ui->context_size = ev->context_size;
        /* Clear streaming tokens for new step */
        if (ui->stream_tokens) ui->stream_tokens[0] = '\0';
        ui->stream_len = 0;

        /* Regenerate react MD and reload if viewing it */
        ui_state_generate_react_md(ui, ui->current_react_loop);

        /* Auto-navigate into reactRX.md on first step so user sees
         * streaming tokens in real-time instead of just the preview
         * in session.md. Push current view onto nav stack. */
        if (ev->step == 0 && !viewing_react_file(ui, ui->current_react_loop)) {
            if (ui->nav_depth >= ui->nav_cap) {
                ui->nav_cap = ui->nav_cap ? ui->nav_cap * 2 : 16;
                ui->nav_stack = realloc(ui->nav_stack,
                                         (size_t)ui->nav_cap * sizeof(nav_entry_t));
            }
            nav_entry_t *ne = &ui->nav_stack[ui->nav_depth];
            ne->filepath = ui->current_filepath ? strdup(ui->current_filepath) : NULL;
            ne->scroll_y = ui->scroll_y;
            ne->scroll_x = ui->scroll_x;
            ne->cursor_link = ui->cursor_link;
            ui->nav_depth++;

            char rpath[4096];
            snprintf(rpath, sizeof(rpath), "%s/reactR%d.md",
                     ui->session_dir, ui->current_react_loop);
            free(ui->current_filepath);
            ui->current_filepath = strdup(rpath);
            ui->scroll_y = 0;
            ui->scroll_x = 0;
            ui->cursor_link = 0;
            ui->focus = FOCUS_JOURNAL;
        }

        if (viewing_react_file(ui, ui->current_react_loop)) {
            ui_state_reload_file(ui);
            auto_scroll_bottom(ui);
        }
        /* Update session.md preview */
        ui_state_generate_session_md(ui);
        if (viewing_session(ui))
            ui_state_reload_file(ui);
        break;
    }

    case REACT_EVENT_LLM_TOKEN:
        if (ev->token && ev->token[0]) {
            int tlen = (int)strlen(ev->token);
            if (ui->stream_len + tlen >= ui->stream_cap - 1) {
                ui->stream_cap = (ui->stream_len + tlen + 1) * 2;
                ui->stream_tokens = realloc(ui->stream_tokens,
                                             (size_t)ui->stream_cap);
            }
            memcpy(ui->stream_tokens + ui->stream_len, ev->token, (size_t)tlen);
            ui->stream_len += tlen;
            ui->stream_tokens[ui->stream_len] = '\0';

            /* Throttle: update file every 64 bytes of tokens.
             * 512 was too aggressive — at ~250 chars/sec typical LLM output,
             * updates only happened every ~2 seconds, making streaming
             * invisible. 64 bytes = ~4-5 updates/sec = smooth streaming. */
            if (ui->stream_len % 64 < tlen) {
                ui_state_generate_react_md(ui, ui->current_react_loop);
                if (viewing_react_file(ui, ui->current_react_loop)) {
                    ui_state_reload_file(ui);
                    auto_scroll_bottom(ui);
                }
            }
        }
        break;

    case REACT_EVENT_STEP_COMPLETE:
    case REACT_EVENT_TOOL_OUTPUT:
        if (ev->stats.prompt_tokens > 0) {
            ui->context_used = ev->stats.prompt_tokens;
            if (ev->context_size > 0)
                ui->context_size = ev->context_size;
        }
        /* Clear streaming tokens */
        if (ui->stream_tokens) ui->stream_tokens[0] = '\0';
        ui->stream_len = 0;

        ui_state_generate_react_md(ui, ui->current_react_loop);
        ui_state_generate_session_md(ui);
        if (viewing_react_file(ui, ui->current_react_loop)) {
            ui_state_reload_file(ui);
            auto_scroll_bottom(ui);
        } else if (viewing_session(ui)) {
            ui_state_reload_file(ui);
        }
        break;

    case REACT_EVENT_DONE:
        ui->status = STATUS_DONE;
        free(ui->status_text);
        ui->status_text = strdup("Done");
        if (ui->stream_tokens) ui->stream_tokens[0] = '\0';
        ui->stream_len = 0;
        if (ev->stats.prompt_tokens > 0) {
            ui->context_used = ev->stats.prompt_tokens;
            if (ev->context_size > 0)
                ui->context_size = ev->context_size;
        }

        ui_state_generate_react_md(ui, ui->current_react_loop);
        ui_state_generate_session_md(ui);
        if (viewing_react_file(ui, ui->current_react_loop)) {
            ui_state_reload_file(ui);
            auto_scroll_bottom(ui);
        } else if (viewing_session(ui)) {
            ui_state_reload_file(ui);
        }
        break;

    case REACT_EVENT_USER_ASK:
        ui->status = STATUS_AWAITING_INPUT;
        free(ui->status_text);
        ui->status_text = strdup("Agent is asking a question — see main pane. Type answer below.");
        /* Store full question for display in main pane */
        free(ui->user_ask_question);
        ui->user_ask_question = (ev->message && ev->message[0])
            ? strdup(ev->message) : strdup("(no question specified)");
        ui_state_generate_react_md(ui, ui->current_react_loop);
        if (viewing_react_file(ui, ui->current_react_loop)) {
            ui_state_reload_file(ui);
            auto_scroll_bottom(ui);
        }
        break;

    case REACT_EVENT_ERROR:
    case REACT_EVENT_WARNING:
        ui_state_generate_react_md(ui, ui->current_react_loop);
        if (viewing_react_file(ui, ui->current_react_loop))
            ui_state_reload_file(ui);
        break;
    }
}

/* ── Status & data updates ─────────────────────────────── */

void ui_state_set_status(ui_state_t *ui, ui_status_t status, const char *text) {
    if (!ui) return;
    ui->status = status;
    free(ui->status_text);
    ui->status_text = text ? strdup(text) : NULL;
    /* Clear user_ask question when leaving AWAITING_INPUT state */
    if (status != STATUS_AWAITING_INPUT) {
        free(ui->user_ask_question);
        ui->user_ask_question = NULL;
    }
    ui->dirty = 1;
}

void ui_state_set_banner(ui_state_t *ui, const char *banner) {
    if (!ui) return;
    free(ui->banner);
    ui->banner = banner ? strdup(banner) : NULL;
    ui_state_generate_session_md(ui);
    if (viewing_session(ui))
        ui_state_reload_file(ui);
}

void ui_state_add_query(ui_state_t *ui, const char *query_text) {
    if (!ui) return;
    /* Save to history */
    if (query_text && query_text[0]) {
        if (ui->history_count >= ui->history_cap) {
            ui->history_cap = ui->history_cap ? ui->history_cap * 2 : 32;
            ui->history = realloc(ui->history,
                                   (size_t)ui->history_cap * sizeof(char *));
        }
        ui->history[ui->history_count++] = strdup(query_text);
        ui->history_idx = ui->history_count;
    }
    /* Session.md will be regenerated when the journal entry is written */
    ui->dirty = 1;
}

void ui_state_load_journal(ui_state_t *ui, journal_t *journal) {
    if (!ui) return;
    ui->journal = journal;
    ui_state_generate_session_md(ui);
    ui_state_reload_file(ui);
}

/* ── Breadcrumb ──────────────────────────────────────────── */

char *ui_state_breadcrumb(ui_state_t *ui) {
    if (!ui) return strdup("");

    str_t s = str_new(256);
    for (int i = 0; i < ui->nav_depth; i++) {
        if (ui->nav_stack[i].filepath) {
            const char *base = strrchr(ui->nav_stack[i].filepath, '/');
            base = base ? base + 1 : ui->nav_stack[i].filepath;
            str_append_cstr(&s, base);
            str_append_cstr(&s, " > ");
        }
    }
    if (ui->current_filepath) {
        const char *base = strrchr(ui->current_filepath, '/');
        base = base ? base + 1 : ui->current_filepath;
        str_append_cstr(&s, base);
    }
    return str_steal(&s);
}
