/*
 * ui_nav.c — Navigation, search, and breadcrumb for the UI state.
 *
 * Extracted from ui_state.c.  Contains:
 *   - Tab/up/down/enter/back/page navigation
 *   - Toggle preview (expand/collapse store refs)
 *   - Cross-session scratchpad search
 *   - Breadcrumb path generation
 */

#include "ui_state_internal.h"
#include "str.h"
#include <fcntl.h>
#include <sys/wait.h>

/* ── Navigation helpers ──────────────────────────────────── */

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

    /* Open http/https links in external browser via xdg-open */
    if (strncmp(uri, "http://", 7) == 0 || strncmp(uri, "https://", 8) == 0) {
        pid_t pid = fork();
        if (pid == 0) {
            /* Child: detach from terminal, redirect output to /dev/null */
            setsid();
            int devnull = open("/dev/null", O_RDWR);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                if (devnull > 2) close(devnull);
            }
            execlp("xdg-open", "xdg-open", uri, (char *)NULL);
            _exit(127);
        }
        /* Parent: reap zombie asynchronously (SIGCHLD default ignores) */
        if (pid > 0) {
            /* Non-blocking waitpid — xdg-open may take a while */
            waitpid(pid, NULL, WNOHANG);
        }
        return;
    }

    /* Handle #anchor links (same-document section navigation) */
    if (uri[0] == '#') {
        const char *fragment = uri + 1;
        int target_line = md_find_anchor(ui->doc, fragment);
        if (target_line >= 0) {
            ui->scroll_y = target_line;
        }
        return;
    }

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
        entry->label = ui->current_label;  /* transfer ownership */
        ui->current_label = NULL;
        entry->scroll_y = ui->scroll_y;
        entry->scroll_x = ui->scroll_x;
        entry->cursor_link = ui->cursor_link;
        /* Save virtual doc (search results) so back-nav can restore it */
        if (ui->search_active && ui->doc) {
            entry->saved_doc = ui->doc;
            ui->doc = NULL;  /* ownership transferred */
        } else {
            entry->saved_doc = NULL;
        }
        ui->nav_depth++;

        /* Resolve URI relative to current file's directory */
        char new_path[NASH_PATH_MAX + NASH_PATH_MAX];
        if (uri[0] == '/') {
            /* Absolute path */
            snprintf(new_path, sizeof(new_path), "%s", uri);
        } else {
            /* Relative to directory of current file (supports ../ traversal) */
            char base_dir[NASH_PATH_MAX];
            if (ui->current_filepath) {
                snprintf(base_dir, sizeof(base_dir), "%s", ui->current_filepath);
                char *slash = strrchr(base_dir, '/');
                if (slash) *slash = '\0';
                else snprintf(base_dir, sizeof(base_dir), "%s", ui->session_dir);
            } else {
                snprintf(base_dir, sizeof(base_dir), "%s", ui->session_dir);
            }
            snprintf(new_path, sizeof(new_path), "%s/%s", base_dir, uri);
        }
        /* Canonicalize to resolve ../  components */
        char resolved[NASH_PATH_MAX];
        if (realpath(new_path, resolved))
            snprintf(new_path, sizeof(new_path), "%s", resolved);

        free(ui->current_filepath);
        ui->current_filepath = strdup(new_path);
        ui->scroll_y = 0;
        ui->scroll_x = 0;
        ui->cursor_link = 0;

        /* Clear search_active so ui_state_reload_file() doesn't skip
         * the load (search results are a virtual doc, but we're now
         * navigating to a real file on disk). */
        ui->search_active = 0;

        /* Subtask on-demand reactRX.md generation.
         *
         * When a subtask finishes, the REACT_EVENT_DONE event sets
         * needs_react_regen=1, but the immediately following RESTORE
         * event switches playbook_session_dir back to the parent before
         * the main loop can process the flag.  Result: the final
         * reactR0.md (with the done result) is never written to the
         * subtask directory.  Fix: detect when navigating into a
         * subtask's reactRX.md and regenerate it on-demand from the
         * child journal which IS on disk. */
        {
            const char *base = strrchr(new_path, '/');
            const char *rname = base ? base + 1 : new_path;
            int rloop = -1;
            if (sscanf(rname, "reactR%d.md", &rloop) == 1 && rloop >= 0) {
                /* Extract directory containing the reactRX.md */
                char react_dir[NASH_PATH_MAX];
                if (base) {
                    size_t dlen = (size_t)(base - new_path);
                    if (dlen >= sizeof(react_dir)) dlen = sizeof(react_dir) - 1;
                    memcpy(react_dir, new_path, dlen);
                    react_dir[dlen] = '\0';
                } else {
                    snprintf(react_dir, sizeof(react_dir), "%s", ui->session_dir);
                }
                /* Temporarily override playbook_session_dir so
                 * generate_react_md reads the correct journal */
                char *saved_psd = ui->playbook_session_dir;
                ui->playbook_session_dir = react_dir;
                ui_state_generate_react_md(ui, rloop);
                ui->playbook_session_dir = saved_psd;
            }
        }

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

    /* Resolve relative to directory of current file */
    char raw_path[NASH_PATH_MAX * 2];
    if (uri_path[0] == '/') {
        snprintf(raw_path, sizeof(raw_path), "%s", uri_path);
    } else {
        char base_dir[NASH_PATH_MAX];
        if (ui->current_filepath) {
            snprintf(base_dir, sizeof(base_dir), "%s", ui->current_filepath);
            char *slash = strrchr(base_dir, '/');
            if (slash) *slash = '\0';
            else snprintf(base_dir, sizeof(base_dir), "%s", ui->session_dir);
        } else {
            snprintf(base_dir, sizeof(base_dir), "%s", ui->session_dir);
        }
        snprintf(raw_path, sizeof(raw_path), "%s/%s", base_dir, uri_path);
    }
    /* Canonicalize to resolve ../ components */
    {
        char resolved[NASH_PATH_MAX];
        if (realpath(raw_path, resolved))
            snprintf(raw_path, sizeof(raw_path), "%s", resolved);
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
    raw_entry->label = ui->current_label;  /* transfer ownership */
    ui->current_label = NULL;
    raw_entry->scroll_y = ui->scroll_y;
    raw_entry->scroll_x = ui->scroll_x;
    raw_entry->cursor_link = ui->cursor_link;
    /* Save virtual doc (search results) so back-nav can restore it */
    if (ui->search_active && ui->doc) {
        raw_entry->saved_doc = ui->doc;
        ui->doc = NULL;
    } else {
        raw_entry->saved_doc = NULL;
    }
    ui->nav_depth++;

    /* Read file content and wrap in MD */
    char *raw_content = slurp_file(raw_path, NULL);
    if (!raw_content) raw_content = strdup("*Empty*\n");

    /* Determine rendering mode based on tool hint.
     * Default is markdown -- most tool outputs are natural language.
     * Only a few tools produce raw/code output that needs a code fence. */
    int render_as_diff = 0;
    int render_as_code = 0;
    if (tool_hint) {
        if (strcmp(tool_hint, "file_edit") == 0) {
            render_as_diff = 1;
        } else if (strcmp(tool_hint, "shell_exec") == 0 ||
                   strcmp(tool_hint, "file_read") == 0 ||
                   strcmp(tool_hint, "file_write") == 0 ||
                   strcmp(tool_hint, "grep_search") == 0 ||
                   strcmp(tool_hint, "glob_search") == 0 ||
                   strcmp(tool_hint, "web_fetch") == 0) {
            render_as_code = 1;
        }
    }

    str_t wrapped = str_new(strlen(raw_content) + 256);
    /* Extract just the filename for the heading (strip fragment) */
    const char *fname = strrchr(uri_path, '/');
    fname = fname ? fname + 1 : uri_path;
    str_appendf(&wrapped, "# %s\n\n", fname);

    if (render_as_diff) {
        /* file_edit: render as diff */
        str_append_cstr(&wrapped, "```diff\n");
        str_append_cstr(&wrapped, raw_content);
        if (raw_content[0] && raw_content[strlen(raw_content)-1] != '\n')
            str_append_cstr(&wrapped, "\n");
        str_append_cstr(&wrapped, "```\n");
    } else if (render_as_code) {
        /* Tools with raw/code output: render as code */
        str_append_cstr(&wrapped, "```\n");
        str_append_cstr(&wrapped, raw_content);
        if (raw_content[0] && raw_content[strlen(raw_content)-1] != '\n')
            str_append_cstr(&wrapped, "\n");
        str_append_cstr(&wrapped, "```\n");
    } else {
        /* Default: render as markdown */
        str_append_cstr(&wrapped, raw_content);
    }
    str_append_cstr(&wrapped, "\n");
    free(raw_content);

    char *md_source = str_steal(&wrapped);
    md_doc_free(ui->doc);
    ui->doc = md_parse(md_source);
    free(md_source);

    /* Clear search mode — we're now viewing a real file */
    ui->search_active = 0;

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

        /* Leaving agent view — clear flag so session.md regen resumes,
         * and clear playbook provenance so react MD uses main session.
         * Only clear if the agent is no longer running; pressing Escape
         * during an active run should NOT drop the agent_view gate. */
        if (ui->agent_view && ui->nav_depth == 0 && !ui->agent_running) {
            ui->agent_view = 0;
            free(ui->playbook_session_dir);
            ui->playbook_session_dir = NULL;
        }

        free(ui->current_filepath);
        ui->current_filepath = entry->filepath;
        entry->filepath = NULL;
        free(ui->current_label);
        ui->current_label = entry->label;   /* restore ownership */
        entry->label = NULL;
        ui->scroll_y = entry->scroll_y;
        ui->scroll_x = entry->scroll_x;
        ui->cursor_link = entry->cursor_link;

        /* Restore virtual doc (search results) if the entry saved one */
        if (entry->saved_doc) {
            md_doc_free(ui->doc);
            ui->doc = entry->saved_doc;
            entry->saved_doc = NULL;
            ui->search_active = 1;  /* re-enable search mode */
        } else {
            /* Clear search state if we just popped back past the search level.
             * The search view uses the virtual path "/search-results.md".
             * If we're no longer at a search-results path, clear the flag. */
            if (ui->search_active && ui->current_filepath &&
                !strstr(ui->current_filepath, "search-results.md"))
                ui->search_active = 0;

            /* Just reload from disk — session.md is already there.
             * The 1-second timer refresh will regenerate it if the react
             * loop has progressed. Calling generate_session_md() here was
             * expensive: it re-reads journal.jsonl + all reactRX.md files
             * for previews, making Esc noticeably slow. */
            ui_state_reload_file(ui);
        }
    }
    ui->dirty = 1;
}

/* ── Push transient content onto nav stack ───────────────── */

/* Derive a display label from a push_content name.
 * "agents" → "Agents", "agent-detail" → "Agent Detail", etc. */
static char *prettify_label(const char *name) {
    if (!name) return NULL;
    size_t len = strlen(name);
    char *label = malloc(len + 1);
    if (!label) return NULL;
    int capitalize = 1;
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (name[i] == '-') {
            label[j++] = ' ';
            capitalize = 1;
        } else if (capitalize) {
            label[j++] = (char)toupper((unsigned char)name[i]);
            capitalize = 0;
        } else {
            label[j++] = name[i];
        }
    }
    label[j] = '\0';
    return label;
}

void ui_state_push_content(ui_state_t *ui, const char *name, const char *markdown) {
    if (!ui || !markdown || !ui->session_dir) return;

    /* 1. Write content to a dot-file (hidden from session listings) */
    char path[NASH_PATH_MAX];
    snprintf(path, sizeof(path), "%s/.cmd-%s.md", ui->session_dir, name);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs(markdown, f);
    fclose(f);

    /* 2. Push current view onto nav stack */
    if (ui->nav_depth >= ui->nav_cap) {
        ui->nav_cap = ui->nav_cap ? ui->nav_cap * 2 : 16;
        ui->nav_stack = realloc(ui->nav_stack,
                                (size_t)ui->nav_cap * sizeof(nav_entry_t));
    }
    nav_entry_t *entry = &ui->nav_stack[ui->nav_depth];
    entry->filepath = ui->current_filepath ? strdup(ui->current_filepath) : NULL;
    entry->label = ui->current_label;   /* transfer ownership */
    ui->current_label = NULL;
    entry->scroll_y = ui->scroll_y;
    entry->scroll_x = ui->scroll_x;
    entry->cursor_link = ui->cursor_link;
    entry->saved_doc = NULL;
    ui->nav_depth++;

    /* 3. Navigate to the new file */
    free(ui->current_filepath);
    ui->current_filepath = strdup(path);
    ui->current_label = prettify_label(name);
    ui->scroll_y = 0;
    ui->scroll_x = 0;
    ui->cursor_link = 0;
    ui_state_reload_file(ui);
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

/* ── Cross-session scratchpad search ─────────────────────── */

/* Compare session directory names in reverse order (newest first).
 * Session dirs are named <epoch>.<nanos>, so reverse strcmp = newest first. */
static int session_cmp_desc(const void *a, const void *b) {
    /* Compare by basename (epoch timestamp) for proper chronological sorting.
     * Handles both basenames and full paths. */
    const char *sa = *(const char **)a;
    const char *sb = *(const char **)b;
    const char *ba = strrchr(sa, '/');
    const char *bb = strrchr(sb, '/');
    return strcmp(bb ? bb + 1 : sb, ba ? ba + 1 : sa);
}

/* Collect session directory full paths from a sessions/ directory.
 * Appends to *names array, updating *count and *cap. */
static void collect_session_dirs(const char *sessions_dir,
                                 char ***names, int *count, int *cap) {
    DIR *dir = opendir(sessions_dir);
    if (!dir) return;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (ent->d_name[0] < '0' || ent->d_name[0] > '9') continue;
        if (*count >= *cap) {
            *cap = *cap ? *cap * 2 : 256;
            *names = realloc(*names, (size_t)*cap * sizeof(char *));
        }
        char full[NASH_PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", sessions_dir, ent->d_name);
        (*names)[(*count)++] = strdup(full);
    }
    closedir(dir);
}

void ui_state_search(ui_state_t *ui, const char *query) {
    if (!ui) return;

    /* Clear search: pop nav stack back if we pushed a search view */
    if (!query || !query[0]) {
        if (ui->search_active) {
            ui->search_active = 0;
            /* Pop the search results from nav stack */
            if (ui->nav_depth > 0) {
                ui->nav_depth--;
                nav_entry_t *entry = &ui->nav_stack[ui->nav_depth];
                free(ui->current_filepath);
                ui->current_filepath = entry->filepath;
                entry->filepath = NULL;
                free(ui->current_label);
                ui->current_label = entry->label;
                entry->label = NULL;
                ui->scroll_y = entry->scroll_y;
                ui->scroll_x = entry->scroll_x;
                ui->cursor_link = entry->cursor_link;
                md_doc_free(entry->saved_doc);
                entry->saved_doc = NULL;
                ui_state_reload_file(ui);
            }
        }
        return;
    }

    /* Build sessions directory path */
    const char *nash_dir = ui->nash_dir;
    if (!nash_dir) {
        /* Derive from session_dir: ~/.nash/sessions/XXX → ~/.nash */
        if (!ui->session_dir) return;
        static char derived[NASH_PATH_MAX];
        snprintf(derived, sizeof(derived), "%s", ui->session_dir);
        /* Go up two levels: sessions/XXX → sessions → nash_dir */
        char *slash = strrchr(derived, '/');
        if (slash) *slash = '\0';
        slash = strrchr(derived, '/');
        if (slash) *slash = '\0';
        nash_dir = derived;
    }

    char sessions_dir[NASH_PATH_MAX + 16];
    snprintf(sessions_dir, sizeof(sessions_dir), "%s/sessions", nash_dir);

    /* Collect session directories (full paths) from global + workspace */
    char **session_dirs = NULL;
    int session_count = 0;
    int session_cap = 0;

    collect_session_dirs(sessions_dir, &session_dirs, &session_count, &session_cap);

    /* Also collect from workspace sessions directory */
    if (ui->workspace_name && ui->workspace_name[0]) {
        char *ws_sessions = sessions_base_dir(nash_dir, ui->workspace_name);
        collect_session_dirs(ws_sessions, &session_dirs, &session_count, &session_cap);
        free(ws_sessions);
    }

    if (session_count == 0) { free(session_dirs); return; }

    /* Sort newest first (by basename = epoch timestamp) */
    qsort(session_dirs, (size_t)session_count, sizeof(char *), session_cmp_desc);

    /* Search each session's scratchpad (JSONL or legacy .md) and journal */
    str_t md = str_new(4096);
    str_appendf(&md, "# 🔍 Search: \"%s\"\n\n", query);

    int total_matches = 0;
    int max_matches = 150;

    for (int si = 0; si < session_count && total_matches < max_matches; si++) {
        int session_had_match = 0;
        char *current_section = NULL;

        /* Helper macro: emit session header on first match */
        #define EMIT_SESSION_HEADER() do { \
            if (!session_had_match) { \
                char ts_display[64]; \
                const char *_bn = strrchr(session_dirs[si], '/'); \
                _bn = _bn ? _bn + 1 : session_dirs[si]; \
                time_t epoch = (time_t)strtol(_bn, NULL, 10); \
                struct tm *tm_info = localtime(&epoch); \
                if (tm_info) \
                    strftime(ts_display, sizeof(ts_display), \
                             "%Y-%m-%d %H:%M:%S", tm_info); \
                else \
                    snprintf(ts_display, sizeof(ts_display), "%s", _bn); \
                /* Link to session.md for navigability */ \
                char link_path[NASH_PATH_MAX + 64]; \
                snprintf(link_path, sizeof(link_path), "%s/session.md", \
                         session_dirs[si]); \
                str_appendf(&md, "### [%s](%s)\n\n", ts_display, link_path); \
                session_had_match = 1; \
            } \
        } while (0)

        /* --- Phase 1: Search scratchpad (JSONL or legacy .md) --- */
        {
            char sp_path[NASH_PATH_MAX + 64];
            int is_jsonl = 0;

            snprintf(sp_path, sizeof(sp_path), "%s/scratchpad.jsonl",
                     session_dirs[si]);
            FILE *f = fopen(sp_path, "r");
            if (f) {
                is_jsonl = 1;
            } else {
                snprintf(sp_path, sizeof(sp_path), "%s/scratchpad.md",
                         session_dirs[si]);
                f = fopen(sp_path, "r");
            }

            if (f) {
                if (is_jsonl) {
                    /* Parse JSONL: each line is {"name":"..","content":"..","priority":N}
                     * or {"name":"..","op":"clear"}.  We search within content fields,
                     * using "name" as section context. */
                    char jsonl_line[65536];
                    while (fgets(jsonl_line, sizeof(jsonl_line), f) &&
                           total_matches < max_matches) {
                        cJSON *obj = cJSON_Parse(jsonl_line);
                        if (!obj) continue;

                        /* Skip clear entries */
                        cJSON *op = cJSON_GetObjectItem(obj, "op");
                        if (op && cJSON_IsString(op) &&
                            strcmp(op->valuestring, "clear") == 0) {
                            cJSON_Delete(obj);
                            continue;
                        }

                        cJSON *name = cJSON_GetObjectItem(obj, "name");
                        cJSON *content = cJSON_GetObjectItem(obj, "content");
                        if (!content || !cJSON_IsString(content)) {
                            cJSON_Delete(obj);
                            continue;
                        }

                        const char *sec_name = (name && cJSON_IsString(name))
                                               ? name->valuestring : NULL;

                        /* Search within content line by line */
                        char *text = strdup(content->valuestring);
                        char *saveptr = NULL;
                        char *cline = strtok_r(text, "\n", &saveptr);
                        while (cline && total_matches < max_matches) {
                            if (ui_ci_strstr(cline, query)) {
                                if (cline[0] != '\0') {
                                    EMIT_SESSION_HEADER();
                                    if (sec_name) {
                                        str_appendf(&md, "- **%s**: %s\n",
                                                    sec_name, cline);
                                    } else {
                                        str_appendf(&md, "- %s\n", cline);
                                    }
                                    total_matches++;
                                }
                            }
                            cline = strtok_r(NULL, "\n", &saveptr);
                        }
                        free(text);
                        cJSON_Delete(obj);
                    }
                } else {
                    /* Legacy scratchpad.md: read line by line */
                    char line[NASH_PATH_MAX];
                    while (fgets(line, sizeof(line), f)) {
                        /* Track section headers */
                        if (line[0] == '#' && line[1] == '#' && line[2] == ' ') {
                            free(current_section);
                            char *p = line + 3;
                            char *nl = strchr(p, '\n');
                            if (nl) *nl = '\0';
                            current_section = strdup(p);
                        }

                        if (ui_ci_strstr(line, query)) {
                            char *nl = strchr(line, '\n');
                            if (nl) *nl = '\0';
                            if (strstr(line, "<!--") != NULL) continue;
                            if (line[0] == '\0') continue;

                            EMIT_SESSION_HEADER();
                            if (current_section) {
                                str_appendf(&md, "- **%s**: %s\n",
                                            current_section, line);
                            } else {
                                str_appendf(&md, "- %s\n", line);
                            }
                            total_matches++;
                            if (total_matches >= max_matches) break;
                        }
                    }
                }
                fclose(f);
            }
        }

        /* --- Phase 2: Search journal.jsonl --- */
        if (total_matches < max_matches) {
            char jrnl_path[NASH_PATH_MAX + 64];
            snprintf(jrnl_path, sizeof(jrnl_path), "%s/journal.jsonl",
                     session_dirs[si]);
            FILE *jf = fopen(jrnl_path, "r");
            if (jf) {
                char jline[65536];
                while (fgets(jline, sizeof(jline), jf) &&
                       total_matches < max_matches) {
                    cJSON *obj = cJSON_Parse(jline);
                    if (!obj) continue;

                    cJSON *tool = cJSON_GetObjectItem(obj, "tool");
                    if (!tool || !cJSON_IsString(tool)) {
                        cJSON_Delete(obj);
                        continue;
                    }

                    const char *tname = tool->valuestring;
                    cJSON *params = cJSON_GetObjectItem(obj, "params");
                    if (!params) { cJSON_Delete(obj); continue; }

                    /* Extract searchable text based on tool type.
                     * Skip "notes" — already covered by scratchpad search. */
                    const char *search_text = NULL;
                    const char *label = NULL;

                    if (strcmp(tname, "query") == 0) {
                        cJSON *t = cJSON_GetObjectItem(params, "text");
                        if (t && cJSON_IsString(t)) {
                            search_text = t->valuestring;
                            label = "query";
                        }
                    } else if (strcmp(tname, "done") == 0) {
                        cJSON *r = cJSON_GetObjectItem(params, "result");
                        if (r && cJSON_IsString(r)) {
                            search_text = r->valuestring;
                            label = "result";
                        }
                    } else if (strcmp(tname, "plan") == 0) {
                        cJSON *r = cJSON_GetObjectItem(params, "result");
                        if (r && cJSON_IsString(r)) {
                            search_text = r->valuestring;
                            label = "plan";
                        }
                    } else if (strcmp(tname, "memory_store") == 0) {
                        cJSON *k = cJSON_GetObjectItem(params, "key");
                        cJSON *v = cJSON_GetObjectItem(params, "value");
                        if (k && cJSON_IsString(k) &&
                            ui_ci_strstr(k->valuestring, query)) {
                            search_text = k->valuestring;
                            label = "memory";
                        } else if (v && cJSON_IsString(v)) {
                            search_text = v->valuestring;
                            label = "memory";
                        }
                    } else if (strcmp(tname, "user_ask") == 0) {
                        cJSON *q = cJSON_GetObjectItem(params, "question");
                        if (q && cJSON_IsString(q)) {
                            search_text = q->valuestring;
                            label = "ask";
                        }
                    }

                    if (!search_text) { cJSON_Delete(obj); continue; }

                    /* Search line by line within the extracted text */
                    char *text = strdup(search_text);
                    char *saveptr = NULL;
                    char *cline = strtok_r(text, "\n", &saveptr);
                    while (cline && total_matches < max_matches) {
                        if (ui_ci_strstr(cline, query) && cline[0] != '\0') {
                            EMIT_SESSION_HEADER();
                            str_appendf(&md, "- **%s**: %s\n", label, cline);
                            total_matches++;
                        }
                        cline = strtok_r(NULL, "\n", &saveptr);
                    }
                    free(text);
                    cJSON_Delete(obj);
                }
                fclose(jf);
            }
        }

        #undef EMIT_SESSION_HEADER

        if (session_had_match)
            str_append_cstr(&md, "\n");

        free(current_section);
    }

    if (total_matches == 0) {
        str_append_cstr(&md, "*No matches found.*\n");
    } else {
        char summary[128];
        snprintf(summary, sizeof(summary), "\n---\n*%d match%s across %d sessions*\n",
                 total_matches, total_matches == 1 ? "" : "es", session_count);
        str_append_cstr(&md, summary);
    }

    /* Free session dirs */
    for (int i = 0; i < session_count; i++)
        free(session_dirs[i]);
    free(session_dirs);

    /* If this is the first search, push current view onto nav stack */
    if (!ui->search_active) {
        if (ui->nav_depth >= ui->nav_cap) {
            ui->nav_cap = ui->nav_cap ? ui->nav_cap * 2 : 16;
            ui->nav_stack = realloc(ui->nav_stack,
                                     (size_t)ui->nav_cap * sizeof(nav_entry_t));
        }
        nav_entry_t *entry = &ui->nav_stack[ui->nav_depth];
        entry->filepath = ui->current_filepath ? strdup(ui->current_filepath) : NULL;
        entry->label = ui->current_label;  /* transfer ownership */
        ui->current_label = NULL;
        entry->scroll_y = ui->scroll_y;
        entry->scroll_x = ui->scroll_x;
        entry->cursor_link = ui->cursor_link;
        entry->saved_doc = NULL;
        ui->nav_depth++;
        ui->search_active = 1;
    }

    /* Parse and display the search results MD */
    char *md_str = str_steal(&md);
    md_doc_free(ui->doc);
    ui->doc = md_parse(md_str);
    free(md_str);

    /* Set a virtual filepath for breadcrumb display */
    free(ui->current_filepath);
    ui->current_filepath = strdup("/search-results.md");

    ui->scroll_y = 0;
    ui->scroll_x = 0;
    ui->cursor_link = 0;
    ui->dirty = 1;
}

/* ── Breadcrumb ──────────────────────────────────────────── */

/* Check if a string is a 64-char hex SHA-256 hash. */
static int is_sha256_hash(const char *s) {
    int i;
    for (i = 0; s[i]; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return 0;
    }
    return i == 64;
}

/* Try to find a symlink alias (e.g. "R0S3") in session_dir that resolves
 * to the same store path as `filepath`.  Returns a malloc'd alias name
 * or NULL if none found. */
static char *resolve_store_alias(const char *session_dir, const char *filepath) {
    if (!session_dir || !filepath) return NULL;
    DIR *d = opendir(session_dir);
    if (!d) return NULL;

    /* Resolve the target filepath to a canonical path for comparison */
    char target_real[NASH_PATH_MAX];
    if (!realpath(filepath, target_real)) {
        closedir(d);
        return NULL;
    }

    char *result = NULL;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        /* Only check symlinks */
        if (ent->d_type != DT_LNK) continue;

        char link_path[NASH_PATH_MAX];
        snprintf(link_path, sizeof(link_path), "%s/%s", session_dir, ent->d_name);

        char link_real[NASH_PATH_MAX];
        if (realpath(link_path, link_real) && strcmp(link_real, target_real) == 0) {
            result = strdup(ent->d_name);
            break;
        }
    }
    closedir(d);
    return result;
}

/* Append a breadcrumb segment for `filepath`, resolving store hashes
 * to their symlink aliases (e.g. "R0S3") or truncating to 15 chars.
 * If `label` is non-NULL, use it instead of deriving from filepath. */
static void breadcrumb_append(str_t *s, const char *filepath,
                              const char *session_dir, const char *label) {
    if (label) {
        str_append_cstr(s, label);
        return;
    }
    const char *base = strrchr(filepath, '/');
    base = base ? base + 1 : filepath;

    if (is_sha256_hash(base)) {
        /* Try to resolve to a friendly alias */
        char *alias = resolve_store_alias(session_dir, filepath);
        if (alias) {
            str_append_cstr(s, alias);
            free(alias);
        } else {
            /* Truncate: first 15 chars + ellipsis */
            str_append(s, base, 15);
            str_append_cstr(s, "\xe2\x80\xa6");  /* UTF-8 '…' */
        }
    } else {
        str_append_cstr(s, base);
    }
}

char *ui_state_breadcrumb(ui_state_t *ui) {
    if (!ui) return strdup("");

    str_t s = str_new(256);
    for (int i = 0; i < ui->nav_depth; i++) {
        if (ui->nav_stack[i].filepath) {
            breadcrumb_append(&s, ui->nav_stack[i].filepath,
                              ui->session_dir, ui->nav_stack[i].label);
            str_append_cstr(&s, " > ");
        }
    }
    if (ui->current_filepath) {
        breadcrumb_append(&s, ui->current_filepath,
                          ui->session_dir, ui->current_label);
    }
    return str_steal(&s);
}
