/*
 * ui_md_gen.c — Markdown generation for session.md and reactRX.md
 *
 * Extracted from ui_state.c.  Contains the two largest functions:
 *   - ui_state_generate_session_md()  (session overview with query tree)
 *   - ui_state_generate_react_md()    (per-react-loop step log)
 * Plus supporting helpers for text extraction and file I/O.
 */

#include "ui_state_internal.h"

/* ── Local helpers ───────────────────────────────────────── */

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



/* Write string to file atomically, delegates to write_file() (str.h). */
static void write_md_file(const char *path, const char *content) {
    if (content)
        write_file(path, content, strlen(content));
}

/* Extract tool description from journal params (for step display). */
static const char *extract_desc(const char *tool, cJSON *params) {
    if (!params) return "";
    cJSON *path = cJSON_GetObjectItem(params, "path");
    cJSON *pat  = cJSON_GetObjectItem(params, "pattern");
    cJSON *res  = cJSON_GetObjectItem(params, "result");
    cJSON *url  = cJSON_GetObjectItem(params, "url");

    if (strcmp(tool, "grep_search") == 0 && pat && pat->valuestring) {
        static char grep_desc[256];
        if (path && path->valuestring && path->valuestring[0])
            snprintf(grep_desc, sizeof(grep_desc), "%s %s",
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
    if ((strcmp(tool, "file_edit") == 0 || strcmp(tool, "file_write") == 0) &&
        path && path->valuestring)
        return path->valuestring;
    if ((strcmp(tool, "web_fetch") == 0 || strcmp(tool, "web_search") == 0) &&
        url && url->valuestring)
        return url->valuestring;
    if (strcmp(tool, "shell_exec") == 0) {
        cJSON *cmd = cJSON_GetObjectItem(params, "command");
        if (cmd && cmd->valuestring)
            return cmd->valuestring;
        return "";
    }
    /* Context injection sections: ctx:memory, ctx:temporal, etc.
     * Show human-readable size and section-specific labels. */
    if (strncmp(tool, "ctx:", 4) == 0) {
        static char ctx_desc[128];
        cJSON *sz = cJSON_GetObjectItem(params, "size");
        int size = sz ? (int)cJSON_GetNumberValue(sz) : 0;
        const char *section = tool + 4;
        if (size > 0) {
            const char *unit = "chars";
            int display = size;
            if (size >= 1000) { display = size / 1000; unit = "K"; }
            snprintf(ctx_desc, sizeof(ctx_desc), "%s (%d%s)", section, display, unit);
        } else {
            snprintf(ctx_desc, sizeof(ctx_desc), "%s", section);
        }
        return ctx_desc;
    }
    /* Legacy context entry (pre-split) */
    if (strcmp(tool, "context") == 0) {
        cJSON *nm = cJSON_GetObjectItem(params, "n_messages");
        if (nm) {
            static char ctx_desc_legacy[64];
            snprintf(ctx_desc_legacy, sizeof(ctx_desc_legacy), "%d messages",
                     (int)cJSON_GetNumberValue(nm));
            return ctx_desc_legacy;
        }
        return "full LLM context";
    }
    /* System log entries: show the message */
    if (strcmp(tool, "log") == 0) {
        cJSON *msg = cJSON_GetObjectItem(params, "message");
        if (msg && msg->valuestring) return msg->valuestring;
        return "(system log)";
    }
    /* Server error entries: show the error message */
    if (strcmp(tool, "server_error") == 0) {
        cJSON *err = cJSON_GetObjectItem(params, "error");
        if (err && err->valuestring) return err->valuestring;
        cJSON *sm = cJSON_GetObjectItem(params, "server_message");
        if (sm && sm->valuestring) return sm->valuestring;
        return "LLM server error";
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
    /* Generic fallback: show params compactly (skip "thought").
     * - Single visible param: show just value, no key= prefix
     * - Multiple visible params: show key=value pairs
     * - Skip default-valued params (false booleans)
     * Truncate individual values at 60 chars. */
    {
        static char generic_desc[512];
        int pos = 0;

        /* First pass: count visible (non-thought, non-action, non-default) params */
        int n_visible = 0;
        cJSON *child = params->child;
        while (child) {
            if (child->string &&
                strcmp(child->string, "thought") != 0 &&
                strcmp(child->string, "action") != 0) {
                /* Skip false booleans (default values) */
                if (!(cJSON_IsBool(child) && !cJSON_IsTrue(child)))
                    n_visible++;
            }
            child = child->next;
        }
        int single_param = (n_visible == 1);

        child = params->child;
        while (child && pos < (int)sizeof(generic_desc) - 2) {
            if (!child->string) { child = child->next; continue; }
            /* Skip thought — rendered separately */
            if (strcmp(child->string, "thought") == 0) {
                child = child->next;
                continue;
            }
            /* Skip action — redundant with tool name column */
            if (strcmp(child->string, "action") == 0) {
                child = child->next;
                continue;
            }
            /* Skip false booleans (default values) */
            if (cJSON_IsBool(child) && !cJSON_IsTrue(child)) {
                child = child->next;
                continue;
            }
            /* Add separator */
            if (pos > 0)
                generic_desc[pos++] = ' ';
            /* key= prefix (omit for single-param tools) */
            if (!single_param) {
                int klen = (int)strlen(child->string);
                int room = (int)sizeof(generic_desc) - 1 - pos;
                if (room < klen + 2) break;
                memcpy(generic_desc + pos, child->string, (size_t)klen);
                pos += klen;
                generic_desc[pos++] = '=';
            }
            if (child->valuestring) {
                int vlen = (int)strlen(child->valuestring);
                int trunc = (vlen > 60);
                if (trunc) vlen = 60;
                int room = (int)sizeof(generic_desc) - 4 - pos;
                if (vlen > room) { vlen = room; trunc = 1; }
                if (vlen > 0) {
                    memcpy(generic_desc + pos, child->valuestring, (size_t)vlen);
                    pos += vlen;
                }
                if (trunc && pos < (int)sizeof(generic_desc) - 4) {
                    memcpy(generic_desc + pos, "...", 3);
                    pos += 3;
                }
            } else if (cJSON_IsNumber(child)) {
                int room = (int)sizeof(generic_desc) - 1 - pos;
                int n = snprintf(generic_desc + pos, (size_t)room, "%g",
                                 cJSON_GetNumberValue(child));
                if (n > 0 && n < room) pos += n;
            } else if (cJSON_IsBool(child)) {
                /* Only true booleans reach here (false already skipped) */
                int room = (int)sizeof(generic_desc) - 1 - pos;
                if (room >= 4) {
                    memcpy(generic_desc + pos, "true", 4);
                    pos += 4;
                }
            }
            child = child->next;
        }
        generic_desc[pos] = '\0';
        if (pos > 0) return generic_desc;
    }
    return "";
}

/* Extract thought from params, unwrapping nested JSON if needed.
 * Delegates to the shared unwrap_thought() in journal.c. */
static char *extract_thought(cJSON *params) {
    if (!params) return NULL;
    cJSON *th = cJSON_GetObjectItem(params, "thought");
    if (!th || !th->valuestring || !th->valuestring[0]) return NULL;

    /* Skip whitespace-only thoughts (e.g. "\n\n" emitted before tool calls) */
    if (is_whitespace_only(th->valuestring)) return NULL;

    char *clean = unwrap_thought(th->valuestring);
    if (clean) return clean;
    /* unwrap_thought returned NULL — if the raw value is JSON (starts with
     * '{'), it's a garbled echo with no extractable thought; suppress it.
     * Otherwise it's plain text — use as-is. */
    if (th->valuestring[0] == '{') return NULL;
    return strdup(th->valuestring);
}

/* ── Session MD generation ───────────────────────────────── */

void ui_state_generate_session_md(ui_state_t *ui) {
    if (!ui) return;

    /* Determine the session directory to generate session.md for.
     * During /agent run, generate session.md for the agent's session dir. */
    const char *session_dir = ui->session_dir;
    if (ui->agent_view && ui->playbook_session_dir) {
        session_dir = ui->playbook_session_dir;
    }
    if (!session_dir) return;

    str_t md = str_new(8192);

    /* Banner */
    if (ui->banner && ui->banner[0]) {
        str_append_cstr(&md, ui->banner);
        str_append_cstr(&md, "\n---\n\n");
    }

    /* Read journal for query list */
    char jpath[NASH_PATH_MAX];
    snprintf(jpath, sizeof(jpath), "%s/journal.jsonl", session_dir);
    FILE *f = fopen(jpath, "r");
    if (!f) {
        if (md.len == 0)
            str_append_cstr(&md, "# Nash\n\n*No session loaded*\n");
        goto write_out;
    }

    /* Collect query info (with tree structure support) */
    typedef struct {
        char *text;
        double ts;
        int react_loop;
        int parent_loop;   /* -1 = root, else parent react_loop ID */
        int step_count;
        char *result;
        int done;
        char *session_dir; /* NULL = main session, else playbook pass dir */
        char *pass_label;  /* NULL = normal query, else playbook pass label */
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
            /* Check if a placeholder was already created by memory_context */
            qinfo_t *qi = NULL;
            for (int i = 0; i < qcount; i++) {
                if (qinfos[i].react_loop == loop) { qi = &qinfos[i]; break; }
            }
            if (!qi) {
                if (qcount >= qcap) {
                    qcap = qcap ? qcap * 2 : 16;
                    qinfos = realloc(qinfos, (size_t)qcap * sizeof(qinfo_t));
                }
                qi = &qinfos[qcount++];
                memset(qi, 0, sizeof(*qi));
            }
            cJSON *params = cJSON_GetObjectItem(entry, "params");
            cJSON *text = params ? cJSON_GetObjectItem(params, "text") : NULL;
            free(qi->text);  /* free any placeholder text from memory_context */
            qi->text = (text && text->valuestring) ? strdup(text->valuestring) : strdup("?");
            cJSON *ts = cJSON_GetObjectItem(entry, "ts");
            qi->ts = ts && ts->valuestring ? atof(ts->valuestring) : 0;
            qi->react_loop = loop;
            /* Parse parent_loop from journal (backward compat: default -1 = root) */
            cJSON *pl = params ? cJSON_GetObjectItem(params, "parent_loop") : NULL;
            qi->parent_loop = (pl && cJSON_IsNumber(pl)) ? (int)pl->valuedouble : -1;
        } else if (tool && strcmp(tool, "memory_context") == 0) {
            /* Fallback: if a react loop was interrupted after memory_context
             * was logged but before the "query" entry was written, use the
             * memory_context's "query" field to make the loop visible. */
            int already_known = 0;
            for (int i = 0; i < qcount; i++) {
                if (qinfos[i].react_loop == loop) { already_known = 1; break; }
            }
            if (!already_known) {
                if (qcount >= qcap) {
                    qcap = qcap ? qcap * 2 : 16;
                    qinfos = realloc(qinfos, (size_t)qcap * sizeof(qinfo_t));
                }
                qinfo_t *qi = &qinfos[qcount++];
                memset(qi, 0, sizeof(*qi));
                cJSON *params = cJSON_GetObjectItem(entry, "params");
                cJSON *qtext = params ? cJSON_GetObjectItem(params, "query") : NULL;
                qi->text = (qtext && qtext->valuestring) ? strdup(qtext->valuestring) : strdup("(interrupted)");
                cJSON *ts = cJSON_GetObjectItem(entry, "ts");
                qi->ts = ts && ts->valuestring ? atof(ts->valuestring) : 0;
                qi->react_loop = loop;
                qi->parent_loop = -1;  /* unknown parent — treat as root */
            }
        } else if (tool && strcmp(tool, "query") != 0 && strcmp(tool, "system") != 0
                        && strncmp(tool, "ctx:", 4) != 0) {
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

    /* ── Read playbook pass journals ──────────────────────── */
    for (int pi = 0; pi < ui->pb_pass_count; pi++) {
        pb_pass_info_t *pbi = &ui->pb_passes[pi];
        if (!pbi->session_dir) continue;

        char pjpath[NASH_PATH_MAX];
        snprintf(pjpath, sizeof(pjpath), "%s/journal.jsonl", pbi->session_dir);
        FILE *pf = fopen(pjpath, "r");
        if (!pf) continue;

        while (fgets(line, sizeof(line), pf)) {
            cJSON *entry = cJSON_Parse(line);
            if (!entry) continue;

            const char *tool = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "tool"));
            int loop = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(entry, "react_loop"));

            if (tool && strcmp(tool, "query") == 0) {
                /* Check if already known (shared session mode: same journal) */
                qinfo_t *qi = NULL;
                for (int i = 0; i < qcount; i++) {
                    if (qinfos[i].react_loop == loop &&
                        qinfos[i].session_dir &&
                        strcmp(qinfos[i].session_dir, pbi->session_dir) == 0) {
                        qi = &qinfos[i]; break;
                    }
                }
                if (!qi) {
                    if (qcount >= qcap) {
                        qcap = qcap ? qcap * 2 : 16;
                        qinfos = realloc(qinfos, (size_t)qcap * sizeof(qinfo_t));
                    }
                    qi = &qinfos[qcount++];
                    memset(qi, 0, sizeof(*qi));
                }
                cJSON *params = cJSON_GetObjectItem(entry, "params");
                cJSON *text = params ? cJSON_GetObjectItem(params, "text") : NULL;
                free(qi->text);
                qi->text = (text && text->valuestring) ? strdup(text->valuestring) : strdup("?");
                cJSON *ts = cJSON_GetObjectItem(entry, "ts");
                qi->ts = ts && ts->valuestring ? atof(ts->valuestring) : 0;
                qi->react_loop = loop;
                qi->parent_loop = -1;  /* playbook passes are always roots */
                free(qi->session_dir);
                qi->session_dir = strdup(pbi->session_dir);
                free(qi->pass_label);
                qi->pass_label = pbi->pass_label ? strdup(pbi->pass_label) : NULL;
            } else if (tool && strcmp(tool, "query") != 0 && strcmp(tool, "system") != 0
                            && strncmp(tool, "ctx:", 4) != 0) {
                /* Count steps and detect done — match by session_dir + loop */
                for (int i = qcount - 1; i >= 0; i--) {
                    if (qinfos[i].react_loop == loop &&
                        qinfos[i].session_dir &&
                        strcmp(qinfos[i].session_dir, pbi->session_dir) == 0) {
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
        fclose(pf);
    }

    str_append_cstr(&md, "## Session History\n\n");

    /* ── Tree-order rendering via DFS ── */
    {
        /* Build render order via iterative DFS.
         * Nodes without a matching parent in qinfos are treated as roots.
         * Uses visited[] to prevent cycles from causing infinite loops. */
        size_t qalloc = qcount > 0 ? (size_t)qcount : 1;
        int *render_order = malloc(qalloc * sizeof(int));
        int *render_depth = malloc(qalloc * sizeof(int));
        int *visited = calloc(qalloc, sizeof(int));
        int rcount = 0;

        /* Stack sized 2*qcount to handle branching safely */
        int stack_cap = qcount > 0 ? qcount * 2 : 1;
        int *dfs_stack = malloc((size_t)stack_cap * sizeof(int));
        int *dfs_depth = malloc((size_t)stack_cap * sizeof(int));
        int stop = 0;

        /* Find roots: parent_loop == -1, self-referencing (parent == self),
         * or parent not found in qinfos.
         * Push in reverse order so first root is processed first. */
        for (int i = qcount - 1; i >= 0; i--) {
            int is_root = (qinfos[i].parent_loop < 0 ||
                           qinfos[i].parent_loop == qinfos[i].react_loop);
            if (!is_root) {
                /* Check if parent exists in qinfos */
                int found = 0;
                for (int j = 0; j < qcount; j++) {
                    if (qinfos[j].react_loop == qinfos[i].parent_loop) {
                        found = 1;
                        break;
                    }
                }
                if (!found) is_root = 1;  /* orphan → treat as root */
            }
            if (is_root && stop < stack_cap) {
                dfs_stack[stop] = i;
                dfs_depth[stop] = 0;
                stop++;
            }
        }

        while (stop > 0) {
            stop--;
            int idx = dfs_stack[stop];
            int depth = dfs_depth[stop];

            if (visited[idx]) continue;  /* cycle guard */
            visited[idx] = 1;

            render_order[rcount] = idx;
            render_depth[rcount] = depth;
            rcount++;

            /* Push children (reverse order for correct DFS traversal) */
            for (int i = qcount - 1; i >= 0; i--) {
                if (!visited[i] && i != idx &&
                    qinfos[i].parent_loop == qinfos[idx].react_loop &&
                    stop < stack_cap) {
                    dfs_stack[stop] = i;
                    dfs_depth[stop] = depth + 1;
                    stop++;
                }
            }
        }

        /* Render in DFS order with tree indentation */
        for (int ri = 0; ri < rcount; ri++) {
            int i = render_order[ri];
            int depth = render_depth[ri];
            qinfo_t *qi = &qinfos[i];

            /* Format timestamp */
            char ts_buf[32] = "";
            if (qi->ts > 0) {
                time_t t = (time_t)qi->ts;
                struct tm *tm = localtime(&t);
                if (tm) strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%d %H:%M:%S", tm);
            }

            /* Effective session dir for this query */
            const char *qi_dir = qi->session_dir ? qi->session_dir : ui->session_dir;

            /* Status icon */
            int is_active = (ui->status == STATUS_RUNNING &&
                             qi->react_loop == ui->current_react_loop &&
                             (!qi->session_dir || (ui->playbook_session_dir &&
                              strcmp(qi->session_dir, ui->playbook_session_dir) == 0)));
            const char *icon = is_active ? "⟳" : (qi->done ? "✓" : "▶");

            /* Tree indentation (2 spaces per depth level) */
            for (int d = 0; d < depth; d++)
                str_append_cstr(&md, "  ");

            /* Build display text: include pass label for playbook entries.
             * For playbook passes, show just the label (the full prompt
             * template is too verbose for the session overview). */
            char display_text[512];
            if (qi->pass_label) {
                snprintf(display_text, sizeof(display_text), "%s",
                         qi->pass_label);
            } else {
                snprintf(display_text, sizeof(display_text), "%s",
                         sanitize_md_link(qi->text));
            }

            /* Query as hyperlink to reactRX.md.
             * For playbook passes in different dirs, use absolute path. */
            if (qi->session_dir) {
                str_appendf(&md, "[%s %s  %s](%s/reactR%d.md)\n",
                            icon, ts_buf, display_text,
                            qi->session_dir, qi->react_loop);
            } else {
                str_appendf(&md, "[%s %s  %s](reactR%d.md)\n",
                            icon, ts_buf, display_text, qi->react_loop);
            }

            /* Preview: show for the ACTIVE react loop, or if user toggled
             * with 'c' key (URI in expanded_uris). */
            char react_uri[NASH_PATH_MAX];
            if (qi->session_dir)
                snprintf(react_uri, sizeof(react_uri), "%s/reactR%d.md",
                         qi->session_dir, qi->react_loop);
            else
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
                         qi_dir, qi->react_loop);
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

        free(render_order);
        free(render_depth);
        free(visited);
        free(dfs_stack);
        free(dfs_depth);
    }

    for (int i = 0; i < qcount; i++) {
        free(qinfos[i].text);
        free(qinfos[i].result);
        free(qinfos[i].session_dir);
        free(qinfos[i].pass_label);
    }
    free(qinfos);

write_out:;
    char *md_str = str_steal(&md);
    char spath[NASH_PATH_MAX];
    snprintf(spath, sizeof(spath), "%s/session.md", session_dir);
    write_md_file(spath, md_str);
    free(md_str);
}

/* ── React MD generation ─────────────────────────────────── */

void ui_state_generate_react_md(ui_state_t *ui, int react_loop) {
    if (!ui || !ui->session_dir) return;

    /* Use playbook session dir if active, else main session dir */
    const char *eff_dir = ui->playbook_session_dir
                        ? ui->playbook_session_dir
                        : ui->session_dir;

    str_t md = str_new(8192);

    /* Read journal for this react loop's entries */
    char jpath[NASH_PATH_MAX];
    snprintf(jpath, sizeof(jpath), "%s/journal.jsonl", eff_dir);
    FILE *f = fopen(jpath, "r");
    if (!f) return;

    /* Collect all entries for this react loop into an array */
    typedef struct {
        char *tool;
        char *desc;
        char *thought;
        char *ref;
        char *compact_desc;  /* non-NULL = compaction separator, not a normal step */
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

        if (strcmp(tool, "memory_context") == 0) {
            /* Fallback query text for interrupted loops (no "query" entry) */
            if (!query_text) {
                cJSON *params = cJSON_GetObjectItem(entry, "params");
                cJSON *qtext = params ? cJSON_GetObjectItem(params, "query") : NULL;
                if (qtext && qtext->valuestring)
                    query_text = strdup(qtext->valuestring);
            }
            cJSON_Delete(entry);
            continue;
        }

        if (strcmp(tool, "system") == 0) {
            cJSON_Delete(entry);
            continue;
        }

        /* Skip internal-only entries that belong in the journal audit
         * trail but not in the user-facing reactRX.md display. */
        if (strcmp(tool, "checkpoint_restore") == 0 ||
            strcmp(tool, "spec") == 0) {
            cJSON_Delete(entry);
            continue;
        }

        /* B1 FIX: Compaction entries → collect as separator (not a normal step) */
        if (strcmp(tool, "compaction") == 0) {
            cJSON *params = cJSON_GetObjectItem(entry, "params");
            journal_compaction_stats_t cs;
            journal_parse_compaction_stats(params, &cs);
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
            char cdesc[128];
            snprintf(cdesc, sizeof(cdesc),
                "\xe2\x9c\x82 context compacted: %d\xe2\x86\x92%d msgs, %d%%\xe2\x86\x92%d%%",
                cs.before_msgs, cs.after_msgs, cs.before_pct, cs.after_pct);
            si->compact_desc = strdup(cdesc);
            cJSON_Delete(entry);
            continue;
        }

        /* Skip internal provider log entries — they clutter the TUI
         * with debug info (token counts, timing) that belongs in the
         * journal audit trail but not in the user-facing display.
         * Exception: error/failure messages ARE shown so the user
         * can see connection problems, auth failures, etc. */
        if (strcmp(tool, "log") == 0) {
            cJSON *params_log = cJSON_GetObjectItem(entry, "params");
            cJSON *msg = params_log ? cJSON_GetObjectItem(params_log, "message") : NULL;
            const char *m = (msg && msg->valuestring) ? msg->valuestring : "";
            if (!ui_ci_strstr(m, "error") && !ui_ci_strstr(m, "failed") &&
                !ui_ci_strstr(m, "timed out")) {
                cJSON_Delete(entry);
                continue;
            }
            /* Fall through: error log entries are displayed as steps */
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

        /* Compaction separator — render as visual divider, not a step */
        if (si->compact_desc) {
            str_append_cstr(&md, "---\n");
            str_append_cstr(&md, si->compact_desc);
            str_append_cstr(&md, "\n---\n");
            continue;
        }

        /* Server error — render as prominent error banner so the user
         * can immediately see the session died and why. */
        if (strcmp(si->tool, "server_error") == 0) {
            str_append_cstr(&md, "\n---\n");
            /* \xe2\x9d\x8c = ❌ */
            str_appendf(&md, "## \xe2\x9d\x8c LLM Error\n\n");
            if (si->desc && si->desc[0])
                str_appendf(&md, "%s\n", si->desc);
            else
                str_append_cstr(&md, "LLM server returned an unrecoverable error.\n");
            str_append_cstr(&md, "\n---\n");
            continue;
        }

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

        /* Format HH:MM from timestamp (first column) */
        #define TIME_COL_WIDTH 6  /* "HH:MM " */
        char time_col[8] = "     ";  /* 5 spaces fallback */
        if (si->ts > 0) {
            time_t tt = (time_t)si->ts;
            struct tm *tm = localtime(&tt);
            if (tm)
                snprintf(time_col, sizeof(time_col), "%02d:%02d",
                         tm->tm_hour, tm->tm_min);
        }

        /* Available display columns for inline text after the prefix.
         * Prefix (rendered): "HH:MM RXSY_pad tool_pad " */
        int term_cols = ui->visible_cols > 0 ? ui->visible_cols : 120;
        int prefix_cols = TIME_COL_WIDTH + REF_COL_WIDTH + 1 + max_tool_len + 1;
        int suffix_cols = (int)strlen(elapsed_str);
        int avail = term_cols - prefix_cols - suffix_cols;
        if (avail < 10) avail = 10;

        int is_shell = (strcmp(si->tool, "shell_exec") == 0);

        /* Helper: emit the tool header line (without any text content) */
        #define EMIT_TOOL_HEADER(with_elapsed) do { \
            if (si->ref) { \
                str_appendf(&md, "%s %s [%s](%s)%s\n", \
                            time_col, ref_pad, \
                            tool_pad, link_uri, \
                            (with_elapsed) ? elapsed_str : ""); \
            } else { \
                str_appendf(&md, "%s %s %s%s\n", \
                            time_col, ref_pad, \
                            tool_pad, \
                            (with_elapsed) ? elapsed_str : ""); \
            } \
        } while (0)

        /* Helper: emit the tool header with inline text */
        #define EMIT_TOOL_WITH_TEXT(text, with_elapsed) do { \
            if (si->ref) { \
                str_appendf(&md, "%s %s [%s](%s) `%s`%s\n", \
                            time_col, ref_pad, \
                            tool_pad, link_uri, \
                            (text), (with_elapsed) ? elapsed_str : ""); \
            } else { \
                str_appendf(&md, "%s %s %s `%s`%s\n", \
                            time_col, ref_pad, \
                            tool_pad, (text), \
                            (with_elapsed) ? elapsed_str : ""); \
            } \
        } while (0)

        /* Helper: emit continuation line with text (indented to match tool column) */
        #define EMIT_CONTINUATION(text, with_elapsed) do { \
            str_appendf(&md, "%*s `%s`%s\n", \
                        TIME_COL_WIDTH + REF_COL_WIDTH + 1 + max_tool_len, "", \
                        (text), (with_elapsed) ? elapsed_str : ""); \
        } while (0)

        /* Helper: emit the tool header with inline thought (plain text, no backticks) */
        #define EMIT_TOOL_WITH_THOUGHT(text, with_elapsed) do { \
            if (si->ref) { \
                str_appendf(&md, "%s %s [%s](%s) %s%s\n", \
                            time_col, ref_pad, \
                            tool_pad, link_uri, \
                            (text), (with_elapsed) ? elapsed_str : ""); \
            } else { \
                str_appendf(&md, "%s %s %s %s%s\n", \
                            time_col, ref_pad, \
                            tool_pad, (text), \
                            (with_elapsed) ? elapsed_str : ""); \
            } \
        } while (0)

        /* Helper: emit thought text as a normal paragraph (using ~> prefix).
         * Each line of the thought becomes a separate ~> line.
         * If thought is a single long line, emit it as one ~> line
         * (md_render.c will word-wrap it). */
        #define EMIT_THOUGHT_PARAGRAPH() do { \
            const char *p = thought_start; \
            int remaining = tlen; \
            while (remaining > 0) { \
                /* Find next newline */ \
                const char *nl = NULL; \
                for (int k = 0; k < remaining; k++) { \
                    if (p[k] == '\n' || p[k] == '\r') { \
                        nl = p + k; \
                        break; \
                    } \
                } \
                if (nl) { \
                    int line_len = (int)(nl - p); \
                    if (line_len > 0) { \
                        str_append_cstr(&md, "~> "); \
                        str_append(&md, p, (size_t)line_len); \
                        str_append_cstr(&md, "\n"); \
                    } \
                    p = nl + 1; \
                    remaining = tlen - (int)(p - thought_start); \
                    /* Skip \r\n pairs */ \
                    if (remaining > 0 && *p == '\n') { \
                        p++; remaining--; \
                    } \
                } else { \
                    /* Last (or only) line */ \
                    if (remaining > 0) { \
                        str_append_cstr(&md, "~> "); \
                        str_append(&md, p, (size_t)remaining); \
                        str_append_cstr(&md, "\n"); \
                    } \
                    break; \
                } \
            } \
        } while (0)

        /* Thought always goes ABOVE the tool header as a green ~> paragraph */
        if (tlen > 0) {
            EMIT_THOUGHT_PARAGRAPH();
        }

        /* Tool header line with arguments (desc) inline */
        if (is_shell && desc_clean) {
            /* shell_exec: command always on same line as tool;
             * the md renderer word-wraps long code spans across multiple lines */
            EMIT_TOOL_WITH_TEXT(desc_clean, 1);
        } else if (desc_clean && desc_len <= avail) {
            /* Desc fits on the tool line */
            EMIT_TOOL_WITH_TEXT(desc_clean, 1);
        } else if (desc_clean) {
            /* Desc too long: tool header then desc on continuation */
            EMIT_TOOL_HEADER(0);
            EMIT_CONTINUATION(desc_clean, 1);
        } else {
            EMIT_TOOL_HEADER(1);
        }

        #undef EMIT_TOOL_HEADER
        #undef EMIT_TOOL_WITH_TEXT
        #undef EMIT_CONTINUATION
        #undef EMIT_TOOL_WITH_THOUGHT
        #undef EMIT_THOUGHT_PARAGRAPH
        #undef TIME_COL_WIDTH

        free(desc_clean);

        /* Preview: show for last step or explicitly expanded steps.
         * Plan tool always shows full preview rendered as markdown.
         * file_edit always shows its diff preview. */
        int is_plan = (strcmp(si->tool, "plan") == 0);
        int is_file_edit = (strcmp(si->tool, "file_edit") == 0);
        int show_preview = is_last || is_plan || is_file_edit;
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
            snprintf(rpath, sizeof(rpath), "%s/%s", eff_dir, si->ref);

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
                    if (is_file_edit)
                        str_append_cstr(&md, "```diff\n");
                    else
                        str_append_cstr(&md, "```\n");
                    char cbuf[NASH_PATH_MAX];
                    int line_count = 0;
                    size_t total = 0;
                    size_t n;
                    /* file_edit: show full diff without truncation */
                    int max_lines = is_file_edit ? INT_MAX : 5;
                    size_t max_bytes = is_file_edit ? SIZE_MAX : 8000;
                    while ((n = fread(cbuf, 1, sizeof(cbuf)-1, cf)) > 0
                           && total < max_bytes && line_count < max_lines) {
                        cbuf[n] = '\0';
                        for (size_t k = 0; k < n && line_count < max_lines; k++) {
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

    /* Streaming indicator if actively running — show live progress */
    if (ui->status == STATUS_RUNNING &&
        ui->current_react_loop == react_loop) {
        static const char spin[] = "|/-\\";
        char sc = spin[ui->spinner_phase % 4];
        ui->spinner_phase++;

        /* Build progress string based on streaming state */
        char progress[128];
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        if (ui->stream_first_token_seen && ui->stream_token_count > 0) {
            /* Tokens are flowing — show generation progress */
            double gen_elapsed = (now.tv_sec - ui->stream_first_token.tv_sec) +
                                 (now.tv_nsec - ui->stream_first_token.tv_nsec) / 1e9;
            if (gen_elapsed > 0.1 && ui->stream_token_count > 1) {
                double tps = (ui->stream_token_count - 1) / gen_elapsed;
                snprintf(progress, sizeof(progress),
                         "generating... %d tokens (%.1f t/s)",
                         ui->stream_token_count, tps);
            } else {
                snprintf(progress, sizeof(progress),
                         "generating... %d tokens",
                         ui->stream_token_count);
            }
        } else {
            /* No tokens yet — prompt is being processed */
            double pp_elapsed = (now.tv_sec - ui->stream_step_start.tv_sec) +
                                (now.tv_nsec - ui->stream_step_start.tv_nsec) / 1e9;
            if (ui->prompt_progress_total > 0) {
                /* Server-reported progress (llama.cpp return_progress) */
                int pct = (int)(100.0 * ui->prompt_progress_processed /
                                ui->prompt_progress_total);
                if (pct > 100) pct = 100;
                if (pp_elapsed >= 0.5)
                    snprintf(progress, sizeof(progress),
                             "prompt processing... %d%% (%.1fs)", pct, pp_elapsed);
                else
                    snprintf(progress, sizeof(progress),
                             "prompt processing... %d%%", pct);
            } else if (pp_elapsed >= 0.5) {
                snprintf(progress, sizeof(progress),
                         "prompt processing... (%.1fs)", pp_elapsed);
            } else {
                snprintf(progress, sizeof(progress), "prompt processing...");
            }
        }

        if (ui->max_steps > 0)
            str_appendf(&md, "  %c %3d %s %-13s %s\n",
                        sc, ui->current_step, "", "", progress);
        else
            str_appendf(&md, "  %c %3d        %-13s %s\n",
                        sc, ui->current_step, "", progress);
        if (ui->stream_tokens && ui->stream_len > 0) {
            /* Suppress display of raw JSON action objects (e.g.
             * {"thought":"","action":"file_read","path":"R1S31"}).
             * Local models emit the entire JSON response as streamed
             * tokens — showing it raw is ugly and distracting.
             * The progress indicator above still shows token count
             * and generation speed, giving the user feedback.
             * Only suppress content that looks like a JSON object
             * (starts with '{' after whitespace); genuine text
             * responses (thinking, errors) are still displayed. */
            const char *p = ui->stream_tokens;
            while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
            if (*p != '{') {
                str_append_cstr(&md, "```\n");
                str_append(&md, ui->stream_tokens, (size_t)ui->stream_len);
                str_append_cstr(&md, "\n```\n");
            }
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
        free(steps[i].compact_desc);
    }
    free(steps);
    free(query_text);

    char *md_str = str_steal(&md);
    char rpath[NASH_PATH_MAX];
    snprintf(rpath, sizeof(rpath), "%s/reactR%d.md",
             eff_dir, react_loop);
    write_md_file(rpath, md_str);
    free(md_str);
}
