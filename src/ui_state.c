#include "ui_state.h"
#include "nash_limits.h"
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
    if (sz > NASH_LINE_MAX) {
        fseek(f, sz - NASH_LINE_MAX, SEEK_SET);
        sz = NASH_LINE_MAX;
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
    if (sz > NASH_FILE_READ_MAX) sz = NASH_FILE_READ_MAX;
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
    char tmp[NASH_PATH_MAX];
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
    /* Context entry: show message count */
    if (strcmp(tool, "context") == 0) {
        cJSON *nm = cJSON_GetObjectItem(params, "n_messages");
        if (nm) {
            static char ctx_desc[64];
            snprintf(ctx_desc, sizeof(ctx_desc), "%d messages",
                     (int)cJSON_GetNumberValue(nm));
            return ctx_desc;
        }
        return "full LLM context";
    }
    /* System log entries: show the message */
    if (strcmp(tool, "log") == 0) {
        cJSON *msg = cJSON_GetObjectItem(params, "message");
        if (msg && msg->valuestring) return msg->valuestring;
        return "(system log)";
    }
    /* Plan: don't show inline text — the full plan is rendered below */
    if (strcmp(tool, "plan") == 0)
        return "";
    /* Truncate done result to first line, max 80 chars */
    if (strcmp(tool, "done") == 0 && res && res->valuestring) {
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
    ui->input_cap = NASH_PATH_MAX;
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
        char path[NASH_PATH_MAX];
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
    char jpath[NASH_PATH_MAX];
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

    char line[NASH_LINE_MAX];
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
            if (tm) strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%d %H:%M:%S", tm);
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
            char rpath[NASH_PATH_MAX];
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
    char spath[NASH_PATH_MAX];
    snprintf(spath, sizeof(spath), "%s/session.md", ui->session_dir);
    write_md_file(spath, md_str);
    free(md_str);
}

void ui_state_generate_react_md(ui_state_t *ui, int react_loop) {
    if (!ui || !ui->session_dir) return;

    str_t md = str_new(8192);

    /* Read journal for this react loop's entries */
    char jpath[NASH_PATH_MAX];
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
    char line[NASH_LINE_MAX];

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

        if (strcmp(tool, "system") == 0 ||
            strcmp(tool, "memory_context") == 0) {
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

    /* Compute max tool name length for column alignment */
    int max_tool_len = 0;
    for (int i = 0; i < nsteps; i++) {
        int tl = (int)strlen(steps[i].tool);
        if (tl > max_tool_len) max_tool_len = tl;
    }

    /* Header — for multi-line queries, show first line as heading
     * and remaining lines as a blockquote block */
    if (query_text && query_text[0]) {
        const char *nl = strchr(query_text, '\n');
        if (nl) {
            /* Multi-line query: first line as heading */
            str_append_cstr(&md, "# Query: ");
            str_append(&md, query_text, (size_t)(nl - query_text));
            str_append_cstr(&md, "\n\n");
            /* Remaining lines as blockquote */
            const char *rest = nl + 1;
            while (*rest) {
                const char *eol = strchr(rest, '\n');
                str_append_cstr(&md, "> ");
                if (eol) {
                    str_append(&md, rest, (size_t)(eol - rest));
                    str_append_cstr(&md, "\n");
                    rest = eol + 1;
                } else {
                    str_append_cstr(&md, rest);
                    str_append_cstr(&md, "\n");
                    break;
                }
            }
            str_append_cstr(&md, "\n");
        } else {
            str_appendf(&md, "# Query: %s\n\n", query_text);
        }
    } else {
        str_appendf(&md, "# Query: ?\n\n");
    }

    /* Render each step in nashell-style compact format */
    for (int i = 0; i < nsteps; i++) {
        step_info_t *si = &steps[i];
        int is_last = (i == nsteps - 1);

        /* Compute elapsed time from previous step */
        double elapsed = 0;
        if (i > 0 && si->ts > 0 && steps[i-1].ts > 0)
            elapsed = si->ts - steps[i-1].ts;

        /* Format elapsed */
        char elapsed_str[32] = "";
        if (elapsed > 0) {
            int es = (int)elapsed;
            if (es > 0)
                snprintf(elapsed_str, sizeof(elapsed_str), " (%ds)", es);
        }

        /* Clean desc: replace newlines with spaces, full length (no truncation) */
        char *desc_clean = NULL;
        int desc_len = 0;
        if (si->desc && si->desc[0]) {
            desc_len = (int)strlen(si->desc);
            desc_clean = malloc((size_t)desc_len + 1);
            if (desc_clean) {
                for (int k = 0; k < desc_len; k++) {
                    if (si->desc[k] == '\n' || si->desc[k] == '\r')
                        desc_clean[k] = ' ';
                    else
                        desc_clean[k] = si->desc[k];
                }
                desc_clean[desc_len] = '\0';
            }
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

        /* Build link URI with tool name in fragment: ref#toolname */
        char link_uri[256];
        if (si->ref)
            snprintf(link_uri, sizeof(link_uri), "%s#%s", si->ref, si->tool);

        /* Build the step line */
        /* Format:   RXSY [tool_name](ref#tool) `args` (Ns)
         *         Only tool_name is a hyperlink, args rendered as `code`
         *         Ref column is padded to 10 chars to fit R999S9999.
         *
         * If the text (desc or thought) is longer than the remaining
         * space to the end of the terminal, print it in full on a
         * continuation line underneath instead of truncating. */

        /* Pad ref to fixed width (10 = fits "R999S9999") */
        #define REF_COL_WIDTH 10
        char ref_pad[16] = "";
        if (si->ref)
            snprintf(ref_pad, sizeof(ref_pad), "%-*s", REF_COL_WIDTH, si->ref);
        else
            snprintf(ref_pad, sizeof(ref_pad), "%-*s", REF_COL_WIDTH, "");

        /* Pad tool name to max_tool_len for column alignment */
        char tool_pad[64];
        snprintf(tool_pad, sizeof(tool_pad), "%-*s", max_tool_len, si->tool);

        /* Available display columns for inline text after the prefix.
         * Prefix (rendered): "RXSY_pad tool_pad " = REF_COL_WIDTH + 1 + max_tool_len */
        int term_cols = ui->visible_cols > 0 ? ui->visible_cols : 120;
        int prefix_cols = REF_COL_WIDTH + 1 + max_tool_len + 1;
        int suffix_cols = (int)strlen(elapsed_str);
        int avail = term_cols - prefix_cols - suffix_cols;
        if (avail < 10) avail = 10;

        int is_shell = (strcmp(si->tool, "shell_exec") == 0);

        /* Helper: emit the tool header line (without any text content) */
        #define EMIT_TOOL_HEADER(with_elapsed) do { \
            if (si->ref) { \
                str_appendf(&md, "%s [%s](%s)%s\n", \
                            ref_pad, \
                            tool_pad, link_uri, \
                            (with_elapsed) ? elapsed_str : ""); \
            } else { \
                str_appendf(&md, "%s %s%s\n", \
                            ref_pad, \
                            tool_pad, \
                            (with_elapsed) ? elapsed_str : ""); \
            } \
        } while (0)

        /* Helper: emit the tool header with inline text */
        #define EMIT_TOOL_WITH_TEXT(text, with_elapsed) do { \
            if (si->ref) { \
                str_appendf(&md, "%s [%s](%s) `%s`%s\n", \
                            ref_pad, \
                            tool_pad, link_uri, \
                            (text), (with_elapsed) ? elapsed_str : ""); \
            } else { \
                str_appendf(&md, "%s %s `%s`%s\n", \
                            ref_pad, \
                            tool_pad, (text), \
                            (with_elapsed) ? elapsed_str : ""); \
            } \
        } while (0)

        /* Helper: emit continuation line with text (indented to match tool column) */
        #define EMIT_CONTINUATION(text, with_elapsed) do { \
            str_appendf(&md, "%*s `%s`%s\n", \
                        REF_COL_WIDTH + 1 + max_tool_len, "", \
                        (text), (with_elapsed) ? elapsed_str : ""); \
        } while (0)

        if (is_shell && tlen > 0) {
            /* shell_exec with thought: thought on main line,
             * command on second line */
            char *thought_text = NULL;
            if (tlen > 0) {
                thought_text = malloc((size_t)tlen + 3); /* +2 for quotes, +1 for NUL */
                if (thought_text) {
                    thought_text[0] = '"';
                    memcpy(thought_text + 1, thought_start, (size_t)tlen);
                    thought_text[tlen + 1] = '"';
                    thought_text[tlen + 2] = '\0';
                }
            }
            int thought_display = tlen + 2; /* +2 for quotes */
            if (thought_text && thought_display <= avail) {
                EMIT_TOOL_WITH_TEXT(thought_text, !desc_clean);
            } else if (thought_text) {
                EMIT_TOOL_HEADER(!desc_clean);
                EMIT_CONTINUATION(thought_text, 0);
            } else {
                EMIT_TOOL_HEADER(!desc_clean);
            }
            free(thought_text);
            if (desc_clean) {
                EMIT_CONTINUATION(desc_clean, 1);
            }
        } else if (is_shell && desc_clean) {
            /* shell_exec without thought: command always on same line as tool;
             * the md renderer word-wraps long code spans across multiple lines */
            EMIT_TOOL_WITH_TEXT(desc_clean, 1);
        } else if (tlen > 0) {
            /* Has thought: thought on main line, desc on second line */
            char *thought_text = malloc((size_t)tlen + 1);
            if (thought_text) {
                memcpy(thought_text, thought_start, (size_t)tlen);
                thought_text[tlen] = '\0';
            }
            if (thought_text && tlen <= avail) {
                EMIT_TOOL_WITH_TEXT(thought_text, !desc_clean);
            } else if (thought_text) {
                EMIT_TOOL_HEADER(!desc_clean);
                EMIT_CONTINUATION(thought_text, 0);
            } else {
                EMIT_TOOL_HEADER(!desc_clean);
            }
            free(thought_text);
            if (desc_clean) {
                EMIT_CONTINUATION(desc_clean, 1);
            }
        } else {
            /* No thought: show desc on main line or underneath */
            if (desc_clean && desc_len <= avail) {
                EMIT_TOOL_WITH_TEXT(desc_clean, 1);
            } else if (desc_clean) {
                EMIT_TOOL_HEADER(0);
                EMIT_CONTINUATION(desc_clean, 1);
            } else {
                EMIT_TOOL_HEADER(1);
            }
        }

        #undef EMIT_TOOL_HEADER
        #undef EMIT_TOOL_WITH_TEXT
        #undef EMIT_CONTINUATION

        free(desc_clean);

        /* Preview: show for last step or explicitly expanded steps.
         * Plan tool always shows full preview rendered as markdown. */
        int is_plan = (strcmp(si->tool, "plan") == 0);
        int show_preview = is_last || is_plan;
        if (!show_preview && si->ref) {
            for (int ei = 0; ei < ui->expanded_count; ei++) {
                if (strcmp(ui->expanded_uris[ei], si->ref) == 0) {
                    show_preview = 1;
                    break;
                }
            }
        }

        if (show_preview && si->ref) {
            char rpath[NASH_PATH_MAX];
            snprintf(rpath, sizeof(rpath), "%s/%s", ui->session_dir, si->ref);

            if (is_plan) {
                /* Plan: read full file and render as markdown (no code
                 * fences, no line limit) so numbered steps display
                 * with proper formatting. */
                char *plan_text = slurp_file(rpath, NULL);
                if (plan_text) {
                    str_append_cstr(&md, "\n");
                    str_append_cstr(&md, plan_text);
                    if (plan_text[0] &&
                        plan_text[strlen(plan_text) - 1] != '\n')
                        str_append_cstr(&md, "\n");
                    str_append_cstr(&md, "\n");
                    free(plan_text);
                }
            } else {
                FILE *cf = fopen(rpath, "r");
                if (cf) {
                    if (strcmp(si->tool, "file_edit") == 0)
                        str_append_cstr(&md, "```diff\n");
                    else
                        str_append_cstr(&md, "```\n");
                    char cbuf[NASH_PATH_MAX];
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
    }

    /* Streaming indicator if actively running */
    if (ui->status == STATUS_RUNNING &&
        ui->current_react_loop == react_loop) {
        static const char spin[] = "|/-\\";
        char sc = spin[ui->spinner_phase % 4];
        ui->spinner_phase++;
        if (ui->max_steps > 0)
            str_appendf(&md, "  %c %3d %s %-13s processing...\n",
                        sc, ui->current_step, "", "");
        else
            str_appendf(&md, "  %c %3d        %-13s processing...\n",
                        sc, ui->current_step, "");
        if (ui->stream_tokens && ui->stream_len > 0) {
            str_append_cstr(&md, "```\n");
            str_append(&md, ui->stream_tokens, (size_t)ui->stream_len);
            str_append_cstr(&md, "\n```\n");
        }
    }

    /* Token generation statistics footer — show at end of active react loop */
    if (ui->current_react_loop == react_loop &&
        (ui->cum_prompt_tokens > 0 || ui->cum_completion_tokens > 0)) {
        str_append_cstr(&md, "\n---\n");

        /* Build stats line: "📊 N in → M out" */
        str_appendf(&md, "\xf0\x9f\x93\x8a %d in \xe2\x86\x92 %d out",
                    ui->cum_prompt_tokens, ui->cum_completion_tokens);

        /* Generation speed (last step's value — most representative) */
        if (ui->cum_predicted_per_second > 0)
            str_appendf(&md, " | gen %.0f t/s", ui->cum_predicted_per_second);

        /* Prompt processing speed */
        if (ui->cum_prompt_per_second > 0)
            str_appendf(&md, " | pp %.0f t/s", ui->cum_prompt_per_second);

        /* Start time HH:MM:SS from first step */
        if (nsteps > 0 && steps[0].ts > 0) {
            time_t t0 = (time_t)steps[0].ts;
            struct tm *tm0 = localtime(&t0);
            if (tm0) {
                char start_buf[16];
                strftime(start_buf, sizeof(start_buf), "%H:%M:%S", tm0);
                str_appendf(&md, " | start %s", start_buf);
            }
        }

        /* Total elapsed time */
        if (ui->react_total_elapsed > 0) {
            char dur[32];
            fmt_duration(ui->react_total_elapsed, dur, sizeof(dur));
            str_appendf(&md, " | total %s", dur);
        }

        /* Number of LLM calls */
        if (ui->cum_llm_steps > 1)
            str_appendf(&md, " | %d calls", ui->cum_llm_steps);

        str_append_cstr(&md, "\n");
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
    char rpath[NASH_PATH_MAX];
    snprintf(rpath, sizeof(rpath), "%s/reactR%d.md",
             ui->session_dir, react_loop);
    write_md_file(rpath, md_str);
    free(md_str);
}

void ui_state_reload_file(ui_state_t *ui) {
    if (!ui || !ui->current_filepath) return;

    /* Skip reload for raw (non-.md) files — they are static store refs
     * that were already wrapped in code fences by ui_state_enter().
     * Reloading them would read the raw content without wrapping,
     * causing a brief correct render followed by broken markdown. */
    int len = (int)strlen(ui->current_filepath);
    if (len < 3 || strcmp(ui->current_filepath + len - 3, ".md") != 0)
        return;

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
        /* Arrow-up while react loop is running → user takes scroll control */
        if (ui->status == STATUS_RUNNING)
            ui->user_scrolled = 1;

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

        /* If cursor reached the last link, resume auto-scroll */
        if (ui->doc->link_count > 0 &&
            ui->cursor_link == ui->doc->link_count - 1)
            ui->user_scrolled = 0;
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
        char new_path[NASH_PATH_MAX];
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
    /* Non-.md links: treat as raw file (store ref like R0S3)
     * URI may contain a #fragment with the tool name (e.g., "R0S3#file_read") */
    const char *fragment = strchr(uri, '#');
    const char *tool_hint = fragment ? fragment + 1 : NULL;

    /* Strip fragment from URI to get the file path portion */
    char uri_path[NASH_PATH_MAX];
    if (fragment) {
        size_t plen = (size_t)(fragment - uri);
        if (plen >= sizeof(uri_path)) plen = sizeof(uri_path) - 1;
        memcpy(uri_path, uri, plen);
        uri_path[plen] = '\0';
    } else {
        snprintf(uri_path, sizeof(uri_path), "%s", uri);
    }

    /* Resolve relative to session_dir and display content */
    char raw_path[NASH_PATH_MAX * 2];
    if (uri_path[0] == '/') {
        snprintf(raw_path, sizeof(raw_path), "%s", uri_path);
    } else {
        snprintf(raw_path, sizeof(raw_path), "%s/%s",
                 ui->session_dir, uri_path);
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

    /* Determine rendering mode based on tool hint:
     * - Markdown tools (done, plan, notes, memory_*, user_ask): render as markdown
     * - file_edit: wrap in ```diff code fence
     * - All other tools: wrap in ``` code fence */
    int render_as_md = 0;
    int render_as_diff = 0;
    if (tool_hint) {
        if (strcmp(tool_hint, "done") == 0 ||
            strcmp(tool_hint, "plan") == 0 ||
            strcmp(tool_hint, "notes") == 0 ||
            strcmp(tool_hint, "context") == 0 ||
            strcmp(tool_hint, "user_ask") == 0 ||
            strncmp(tool_hint, "memory_", 7) == 0) {
            render_as_md = 1;
        } else if (strcmp(tool_hint, "file_edit") == 0) {
            render_as_diff = 1;
        }
    }

    str_t wrapped = str_new(strlen(raw_content) + 256);
    /* Extract just the filename for the heading (strip fragment) */
    const char *fname = strrchr(uri_path, '/');
    fname = fname ? fname + 1 : uri_path;
    str_appendf(&wrapped, "# %s\n\n", fname);

    if (render_as_md) {
        /* Markdown tools: render content with formatting */
        str_append_cstr(&wrapped, raw_content);
    } else if (render_as_diff) {
        /* file_edit: render as diff */
        str_append_cstr(&wrapped, "```diff\n");
        str_append_cstr(&wrapped, raw_content);
        if (raw_content[0] && raw_content[strlen(raw_content)-1] != '\n')
            str_append_cstr(&wrapped, "\n");
        str_append_cstr(&wrapped, "```\n");
    } else {
        /* All other tools: render as code */
        str_append_cstr(&wrapped, "```\n");
        str_append_cstr(&wrapped, raw_content);
        if (raw_content[0] && raw_content[strlen(raw_content)-1] != '\n')
            str_append_cstr(&wrapped, "\n");
        str_append_cstr(&wrapped, "```\n");
    }
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
    /* Page-up while react loop is running → user takes scroll control */
    if (ui->status == STATUS_RUNNING)
        ui->user_scrolled = 1;
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
    if (!ui) return;
    /* Allow printable ASCII, newline, and Unicode codepoints (UTF-8).
     * With ncursesw, getch() returns full Unicode codepoints as int values.
     * We encode them as UTF-8 bytes into the input buffer. */
    if (ch != '\n' && ch < 32) return;      /* reject control chars except newline */
    if (ch == 127) return;                   /* reject DEL */

    /* Encode codepoint to UTF-8 */
    char utf8[4];
    int nbytes;
    if (ch < 0x80) {
        utf8[0] = (char)ch;
        nbytes = 1;
    } else if (ch < 0x800) {
        utf8[0] = (char)(0xC0 | (ch >> 6));
        utf8[1] = (char)(0x80 | (ch & 0x3F));
        nbytes = 2;
    } else if (ch < 0x10000) {
        utf8[0] = (char)(0xE0 | (ch >> 12));
        utf8[1] = (char)(0x80 | ((ch >> 6) & 0x3F));
        utf8[2] = (char)(0x80 | (ch & 0x3F));
        nbytes = 3;
    } else if (ch < 0x110000) {
        utf8[0] = (char)(0xF0 | (ch >> 18));
        utf8[1] = (char)(0x80 | ((ch >> 12) & 0x3F));
        utf8[2] = (char)(0x80 | ((ch >> 6) & 0x3F));
        utf8[3] = (char)(0x80 | (ch & 0x3F));
        nbytes = 4;
    } else {
        return;  /* invalid codepoint */
    }

    /* Ensure capacity for nbytes */
    while (ui->input_len + nbytes >= ui->input_cap - 1) {
        ui->input_cap *= 2;
        ui->input_buffer = realloc(ui->input_buffer, (size_t)ui->input_cap);
    }
    memmove(ui->input_buffer + ui->cursor_pos + nbytes,
            ui->input_buffer + ui->cursor_pos,
            (size_t)(ui->input_len - ui->cursor_pos + 1));
    memcpy(ui->input_buffer + ui->cursor_pos, utf8, (size_t)nbytes);
    ui->cursor_pos += nbytes;
    ui->input_len += nbytes;
    ui->dirty = 1;
}

/* Helper: count bytes in the UTF-8 character ending at buf[pos-1] (backspace).
 * Returns 1-4 for valid UTF-8, 1 for ASCII or malformed sequences. */
static int utf8_char_len_back(const char *buf, int pos) {
    if (pos <= 0) return 0;
    /* Walk backward over continuation bytes (10xxxxxx = 0x80..0xBF) */
    int i = pos - 1;
    int count = 1;
    while (i > 0 && count < 4 &&
           ((unsigned char)buf[i] & 0xC0) == 0x80) {
        i--;
        count++;
    }
    return count;
}

/* Helper: count bytes in the UTF-8 character starting at buf[pos] (delete/right).
 * Returns 1-4 for valid UTF-8, 1 for ASCII or malformed sequences. */
static int utf8_char_len_fwd(const char *buf, int pos, int len) {
    if (pos >= len) return 0;
    unsigned char c = (unsigned char)buf[pos];
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return (pos + 2 <= len) ? 2 : 1;
    if ((c & 0xF0) == 0xE0) return (pos + 3 <= len) ? 3 : 1;
    if ((c & 0xF8) == 0xF0) return (pos + 4 <= len) ? 4 : 1;
    return 1;  /* continuation byte or invalid — treat as single byte */
}

void ui_state_input_backspace(ui_state_t *ui) {
    if (!ui || ui->cursor_pos <= 0) return;
    int nb = utf8_char_len_back(ui->input_buffer, ui->cursor_pos);
    memmove(ui->input_buffer + ui->cursor_pos - nb,
            ui->input_buffer + ui->cursor_pos,
            (size_t)(ui->input_len - ui->cursor_pos + 1));
    ui->cursor_pos -= nb;
    ui->input_len -= nb;
    ui->dirty = 1;
}

void ui_state_input_delete(ui_state_t *ui) {
    if (!ui || ui->cursor_pos >= ui->input_len) return;
    int nb = utf8_char_len_fwd(ui->input_buffer, ui->cursor_pos, ui->input_len);
    memmove(ui->input_buffer + ui->cursor_pos,
            ui->input_buffer + ui->cursor_pos + nb,
            (size_t)(ui->input_len - ui->cursor_pos - nb + 1));
    ui->input_len -= nb;
    ui->dirty = 1;
}

void ui_state_input_left(ui_state_t *ui) {
    if (!ui || ui->cursor_pos <= 0) return;
    int nb = utf8_char_len_back(ui->input_buffer, ui->cursor_pos);
    ui->cursor_pos -= nb;
    ui->dirty = 1;
}
void ui_state_input_right(ui_state_t *ui) {
    if (!ui || ui->cursor_pos >= ui->input_len) return;
    int nb = utf8_char_len_fwd(ui->input_buffer, ui->cursor_pos, ui->input_len);
    ui->cursor_pos += nb;
    ui->dirty = 1;
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

/* Auto-scroll: request deferred scroll to bottom.
 * The actual scroll computation is done in render_main() (tui.c) AFTER
 * md_render() has set doc->total_lines and link render_lines.
 * Before this fix, scroll_y was computed here with stale data (total_lines=0,
 * render_line=-1) because md_parse doesn't call md_render. */
static void auto_scroll_bottom(ui_state_t *ui) {
    if (!ui->doc) return;
    if (ui->user_scrolled) return;          /* user took control */
    ui->needs_auto_scroll = 1;
    ui->dirty = 1;
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

        /* Reset cumulative stats and user_scrolled on first step of a
         * new react loop so stats start fresh and auto-scroll is active. */
        if (ev->step == 0) {
            ui->user_scrolled = 0;
            ui->cum_prompt_tokens = 0;
            ui->cum_completion_tokens = 0;
            ui->cum_predicted_per_second = 0;
            ui->cum_prompt_per_second = 0;
            ui->react_total_elapsed = 0;
            ui->cum_llm_steps = 0;
            ui->react_done = 0;
        }

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

            char rpath[NASH_PATH_MAX];
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
        /* Accumulate token stats for the react loop summary */
        if (ev->type == REACT_EVENT_STEP_COMPLETE &&
            (ev->stats.prompt_tokens > 0 || ev->stats.completion_tokens > 0)) {
            ui->cum_prompt_tokens += ev->stats.prompt_tokens;
            ui->cum_completion_tokens += ev->stats.completion_tokens;
            if (ev->stats.predicted_per_second > 0)
                ui->cum_predicted_per_second = ev->stats.predicted_per_second;
            if (ev->stats.prompt_per_second > 0)
                ui->cum_prompt_per_second = ev->stats.prompt_per_second;
            ui->cum_llm_steps++;
        }
        if (ev->total_elapsed > 0)
            ui->react_total_elapsed = ev->total_elapsed;
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
        /* Accumulate final step's token stats */
        if (ev->stats.prompt_tokens > 0 || ev->stats.completion_tokens > 0) {
            ui->cum_prompt_tokens += ev->stats.prompt_tokens;
            ui->cum_completion_tokens += ev->stats.completion_tokens;
            if (ev->stats.predicted_per_second > 0)
                ui->cum_predicted_per_second = ev->stats.predicted_per_second;
            if (ev->stats.prompt_per_second > 0)
                ui->cum_prompt_per_second = ev->stats.prompt_per_second;
            ui->cum_llm_steps++;
        }
        if (ev->total_elapsed > 0)
            ui->react_total_elapsed = ev->total_elapsed;
        ui->react_done = 1;

        /* React loop finished — clear user_scrolled so final
         * auto-scroll shows the completed result. */
        ui->user_scrolled = 0;

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
