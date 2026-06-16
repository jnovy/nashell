#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

#include "commands.h"
#include "cJSON.h"
#include "str.h"
#include "nash_limits.h"
#include "scratchpad.h"
#include "tui.h"

/* Comparator for qsort — descending string order (newest first) */
static int cmp_str_desc(const void *a, const void *b) {
    return strcmp(*(const char **)b, *(const char **)a);
}

/* ── /fork <step> ──────────────────────────────────────────────── */
static int cmd_fork(command_ctx_t *ctx, const char *arg) {
    int fork_step = atoi(arg);
    if (fork_step <= 0) return CMD_CONTINUE;

    char *session_dir = *ctx->session_dir;
    tool_ctx_t *tools = ctx->tools;
    react_ctx_t *react = ctx->react;
    ui_state_t *ui = ctx->ui;

    char *new_dir = create_session_dir(ctx->nash_dir);
    /* Copy journal lines where step <= fork_step */
    char src_j[NASH_PATH_MAX], dst_j[NASH_PATH_MAX];
    snprintf(src_j, sizeof(src_j), "%s/journal.jsonl", session_dir);
    snprintf(dst_j, sizeof(dst_j), "%s/journal.jsonl", new_dir);
    FILE *sf = fopen(src_j, "r");
    FILE *df = fopen(dst_j, "w");
    if (sf && df) {
        char jl[NASH_LINE_MAX];
        while (fgets(jl, sizeof(jl), sf)) {
            cJSON *e = cJSON_Parse(jl);
            if (e) {
                int s = (int)cJSON_GetNumberValue(
                    cJSON_GetObjectItem(e, "step"));
                if (s <= fork_step) fputs(jl, df);
                cJSON_Delete(e);
            }
        }
    }
    if (sf) fclose(sf);
    if (df) fclose(df);
    /* Copy symlinks from all react loops.
     * Use the alias map's next_seq as the upper bound —
     * it tracks the actual number of aliases created. */
    int max_alias = tools->aliases ? tools->aliases->next_seq : fork_step + 5;
    for (int loop = 0; loop <= tools->react_loop; loop++) {
        for (int i = 0; i <= max_alias; i++) {
            char ref[32], sl[NASH_PATH_MAX], tgt[NASH_PATH_MAX], dl[NASH_PATH_MAX];
            snprintf(ref, sizeof(ref), "R%dS%d", loop, i);
            snprintf(sl, sizeof(sl), "%s/%s", session_dir, ref);
            ssize_t n = readlink(sl, tgt, sizeof(tgt) - 1);
            if (n > 0) {
                tgt[n] = '\0';
                snprintf(dl, sizeof(dl), "%s/%s", new_dir, ref);
                symlink(tgt, dl);
            }
        }
    }
    /* Write checkpoint */
    cJSON *cp = cJSON_CreateObject();
    cJSON_AddNumberToObject(cp, "version", 1);
    cJSON_AddNumberToObject(cp, "step", fork_step);
    cJSON_AddNumberToObject(cp, "react_loop", tools->react_loop);
    if (react->last_query)
        cJSON_AddStringToObject(cp, "user_query", react->last_query);
    /* Legacy scratchpad string removed — checkpoint restore
     * falls back to scratchpad_parse from checkpoint JSON
     * if section files don't exist (very old sessions). */
    char *cpj = cJSON_Print(cp);
    char cp_path[NASH_PATH_MAX];
    snprintf(cp_path, sizeof(cp_path), "%s/checkpoint.json", new_dir);
    write_file(cp_path, cpj, strlen(cpj));
    free(cpj);
    cJSON_Delete(cp);
    /* Persist scratchpad to the forked session directory
     * so it survives resume. Without this, the forked
     * session starts with an empty scratchpad. */
    scratchpad_save(&tools->scratch, new_dir);
    /* Switch to forked session */
    journal_free(*ctx->journal);
    free(session_dir);
    *ctx->session_dir = new_dir;
    *ctx->journal = journal_new(new_dir);
    tools->journal = *ctx->journal;
    tools->session_dir = new_dir;
    alias_map_clear(tools->aliases);
    ui_state_set_status(ui, STATUS_READY, "Forked — ready for new query");
    tui_render(ui);
    return CMD_CONTINUE;
}

/* ── /name <name> ──────────────────────────────────────────────── */
static int cmd_name(command_ctx_t *ctx, const char *name) {
    ui_state_t *ui = ctx->ui;
    char *session_dir = *ctx->session_dir;

    if (strlen(name) == 0 || strlen(name) > 255 ||
        strchr(name, '/') != NULL || strchr(name, '\n') != NULL) {
        pthread_mutex_lock(&ui->mtx);
        ui_state_set_status(ui, STATUS_ERROR,
            "/name: invalid name (no slashes, max 255 chars)");
        pthread_mutex_unlock(&ui->mtx);
        tui_render(ui);
        return CMD_CONTINUE;
    }
    char sessions_base[1024], link_path[1088];
    snprintf(sessions_base, sizeof(sessions_base), "%s/sessions", ctx->nash_dir);
    snprintf(link_path, sizeof(link_path), "%s/%s", sessions_base, name);
    /* Remove existing symlink if present */
    unlink(link_path);
    /* Create symlink */
    if (symlink(session_dir, link_path) == 0) {
        /* Extract just the session ID (basename) for display */
        const char *session_id = strrchr(session_dir, '/');
        session_id = session_id ? session_id + 1 : session_dir;
        pthread_mutex_lock(&ui->mtx);
        char status[512];
        snprintf(status, sizeof(status),
            "Named session: %s → %s", name, session_id);
        ui_state_set_status(ui, STATUS_READY, status);
        pthread_mutex_unlock(&ui->mtx);
        tui_render(ui);
    } else {
        pthread_mutex_lock(&ui->mtx);
        ui_state_set_status(ui, STATUS_ERROR,
            "/name: failed to create symlink");
        pthread_mutex_unlock(&ui->mtx);
        tui_render(ui);
    }
    return CMD_CONTINUE;
}

/* ── /cwd <dir> ────────────────────────────────────────────────── */
static int cmd_cwd(command_ctx_t *ctx, const char *dir) {
    ui_state_t *ui = ctx->ui;

    while (*dir == ' ') dir++;
    if (*dir == '\0') {
        pthread_mutex_lock(&ui->mtx);
        ui_state_set_status(ui, STATUS_ERROR,
            "/cwd: missing directory argument");
        pthread_mutex_unlock(&ui->mtx);
        tui_render(ui);
        return CMD_CONTINUE;
    }
    /* Create directory if it doesn't exist */
    struct stat st;
    if (stat(dir, &st) != 0) {
        if (mkdir_p(dir, 0755) != 0) {
            char errbuf[512];
            snprintf(errbuf, sizeof(errbuf),
                "/cwd: failed to create '%s': %s", dir, strerror(errno));
            pthread_mutex_lock(&ui->mtx);
            ui_state_set_status(ui, STATUS_ERROR, errbuf);
            pthread_mutex_unlock(&ui->mtx);
            tui_render(ui);
            return CMD_CONTINUE;
        }
    }
    /* Change to the directory */
    if (chdir(dir) != 0) {
        char errbuf[512];
        snprintf(errbuf, sizeof(errbuf),
            "/cwd: failed to chdir to '%s': %s", dir, strerror(errno));
        pthread_mutex_lock(&ui->mtx);
        ui_state_set_status(ui, STATUS_ERROR, errbuf);
        pthread_mutex_unlock(&ui->mtx);
        tui_render(ui);
        return CMD_CONTINUE;
    }
    /* Show success with resolved path */
    char resolved[4096];
    if (!getcwd(resolved, sizeof(resolved)))
        snprintf(resolved, sizeof(resolved), "%s", dir);
    char status_msg[4112];
    snprintf(status_msg, sizeof(status_msg), "CWD: %s", resolved);
    pthread_mutex_lock(&ui->mtx);
    ui_state_set_status(ui, STATUS_READY, status_msg);
    pthread_mutex_unlock(&ui->mtx);
    tui_render(ui);
    return CMD_CONTINUE;
}

/* ── /dream ────────────────────────────────────────────────────── */
static int cmd_dream(command_ctx_t *ctx) {
    ui_state_t *ui = ctx->ui;

    if (*ctx->inferring) {
        pthread_mutex_lock(&ui->mtx);
        ui_state_set_status(ui, STATUS_ERROR,
                            "Wait for inference to finish");
        pthread_mutex_unlock(&ui->mtx);
        tui_render(ui);
        return CMD_CONTINUE;
    }

    /* Load dream playbook from ~/.nash/playbooks/dream.yaml.
     * If not found, write the default and load it. */
    char pb_path[NASH_PATH_MAX];
    snprintf(pb_path, sizeof(pb_path), "%s/playbooks/dream.yaml", ctx->nash_dir);
    playbook_t *dream_pb = playbook_load(pb_path);
    if (!dream_pb) {
        playbook_write_default_dream(pb_path);
        dream_pb = playbook_load(pb_path);
    }
    if (!dream_pb) {
        pthread_mutex_lock(&ui->mtx);
        ui_state_set_status(ui, STATUS_ERROR,
            "Cannot load dream playbook");
        pthread_mutex_unlock(&ui->mtx);
        tui_render(ui);
        return CMD_CONTINUE;
    }

    *ctx->pargs = (playbook_args_t){
        .playbook = dream_pb,
        .nash_dir = (char *)ctx->nash_dir,
        .store = ctx->store,
        .memory = ctx->memory,
        .cfg = ctx->cfg,
        .provider = ctx->provider,
        .server_model = (char *)ctx->server_model,
        .ui = ui,
        .playbook_ok = 0,
        .done = 0,
    };
    ctx->provider->abort_retry = 0;  /* reset before new inference */
    pthread_create(ctx->infer_tid, NULL, playbook_worker, ctx->pargs);
    *ctx->inferring = 3;
    tui_render(ui);
    return CMD_CONTINUE;
}

/* ── /play <arg> ───────────────────────────────────────────────── */
static int cmd_play(command_ctx_t *ctx, const char *arg) {
    ui_state_t *ui = ctx->ui;

    while (*arg == ' ') arg++;

    if (*ctx->inferring) {
        pthread_mutex_lock(&ui->mtx);
        ui_state_set_status(ui, STATUS_ERROR,
                            "Wait for inference to finish");
        pthread_mutex_unlock(&ui->mtx);
        tui_render(ui);
        return CMD_CONTINUE;
    }

    if (strcmp(arg, "list") == 0) {
        /* List available playbooks */
        int pb_count = 0;
        playbook_t **pbs = playbook_list(ctx->nash_dir, &pb_count);
        str_t display = str_new(1024);
        str_appendf(&display, "# Available Playbooks\n\n");
        if (pb_count == 0) {
            str_appendf(&display, "No playbooks found in %s/playbooks/\n", ctx->nash_dir);
        } else {
            for (int i = 0; i < pb_count; i++) {
                str_appendf(&display, "- **%s**: %s (%d passes)\n",
                    pbs[i]->name,
                    pbs[i]->description ? pbs[i]->description : "",
                    pbs[i]->n_passes);
                playbook_free(pbs[i]);
            }
            free(pbs);
        }
        char *banner = str_steal(&display);
        pthread_mutex_lock(&ui->mtx);
        ui_state_set_banner(ui, banner);
        ui_state_set_status(ui, STATUS_READY, "Playbook list");
        pthread_mutex_unlock(&ui->mtx);
        free(banner);
        tui_render(ui);
        return CMD_CONTINUE;
    }

    /* Load playbook by name or path */
    char pb_path[NASH_PATH_MAX];
    if (strchr(arg, '/') || strchr(arg, '.')) {
        snprintf(pb_path, sizeof(pb_path), "%s", arg);
    } else {
        snprintf(pb_path, sizeof(pb_path), "%s/playbooks/%s.yaml",
                 ctx->nash_dir, arg);
    }
    playbook_t *pb = playbook_load(pb_path);
    if (!pb) {
        pthread_mutex_lock(&ui->mtx);
        char errmsg[NASH_PATH_MAX + 64];
        snprintf(errmsg, sizeof(errmsg),
                 "/play: cannot load playbook '%.4080s'", pb_path);
        ui_state_set_status(ui, STATUS_ERROR, errmsg);
        pthread_mutex_unlock(&ui->mtx);
        tui_render(ui);
        return CMD_CONTINUE;
    }

    *ctx->pargs = (playbook_args_t){
        .playbook = pb,
        .nash_dir = (char *)ctx->nash_dir,
        .store = ctx->store,
        .memory = ctx->memory,
        .cfg = ctx->cfg,
        .provider = ctx->provider,
        .server_model = (char *)ctx->server_model,
        .ui = ui,
        .playbook_ok = 0,
        .done = 0,
    };
    ctx->provider->abort_retry = 0;  /* reset before new inference */
    pthread_create(ctx->infer_tid, NULL, playbook_worker, ctx->pargs);
    *ctx->inferring = 3;
    tui_render(ui);
    return CMD_CONTINUE;
}

/* ── /runs [list|show <id>] ────────────────────────────────────── */
static int cmd_runs(command_ctx_t *ctx, const char *sub) {
    ui_state_t *ui = ctx->ui;

    while (*sub == ' ') sub++;

    int show_detail = 0;
    const char *show_id = NULL;
    if (strncmp(sub, "show ", 5) == 0) {
        show_detail = 1;
        show_id = sub + 5;
        while (*show_id == ' ') show_id++;
    }

    char rdir[NASH_PATH_MAX];
    snprintf(rdir, sizeof(rdir), "%s/runs", ctx->nash_dir);

    if (show_detail && show_id && *show_id) {
        /* /runs show <id> — display a specific run log */
        char rpath[NASH_PATH_MAX + NASH_PATH_MAX];
        /* Try exact filename, or append .jsonl */
        if (strstr(show_id, ".jsonl"))
            snprintf(rpath, sizeof(rpath), "%s/%s", rdir, show_id);
        else
            snprintf(rpath, sizeof(rpath), "%s/%s.jsonl", rdir, show_id);

        FILE *rf = fopen(rpath, "r");
        if (!rf) {
            pthread_mutex_lock(&ui->mtx);
            ui_state_set_status(ui, STATUS_ERROR,
                "/runs show: run log not found");
            pthread_mutex_unlock(&ui->mtx);
            tui_render(ui);
            return CMD_CONTINUE;
        }

        str_t display = str_new(2048);
        str_appendf(&display, "# Run Log: %s\n\n", show_id);
        char line[NASH_LINE_MAX];
        while (fgets(line, sizeof(line), rf)) {
            cJSON *ev = cJSON_Parse(line);
            if (!ev) continue;
            const char *e = cJSON_GetStringValue(
                cJSON_GetObjectItem(ev, "e"));
            if (!e) { cJSON_Delete(ev); continue; }

            if (strcmp(e, "start") == 0) {
                str_appendf(&display, "**Playbook**: %s  \n",
                    cJSON_GetStringValue(
                        cJSON_GetObjectItem(ev, "pb")));
                str_appendf(&display, "**Passes**: %d\n\n",
                    (int)cJSON_GetNumberValue(
                        cJSON_GetObjectItem(ev, "n")));
            } else if (strcmp(e, "pass") == 0) {
                int idx = (int)cJSON_GetNumberValue(
                    cJSON_GetObjectItem(ev, "i"));
                const char *label = cJSON_GetStringValue(
                    cJSON_GetObjectItem(ev, "l"));
                const char *sid = cJSON_GetStringValue(
                    cJSON_GetObjectItem(ev, "sid"));
                str_appendf(&display,
                    "- **Pass %d**: %s\n  session: `%s`\n",
                    idx + 1, label ? label : "?",
                    sid ? sid : "?");
            } else if (strcmp(e, "done") == 0) {
                const char *st = cJSON_GetStringValue(
                    cJSON_GetObjectItem(ev, "st"));
                double ts = cJSON_GetNumberValue(
                    cJSON_GetObjectItem(ev, "ts"));
                str_appendf(&display, "  status: %s  (%.0f)\n",
                    st ? st : "?", ts);
            } else if (strcmp(e, "end") == 0) {
                const char *st = cJSON_GetStringValue(
                    cJSON_GetObjectItem(ev, "st"));
                str_appendf(&display, "\n**Result**: %s\n",
                    st ? st : "?");
            }
            cJSON_Delete(ev);
        }
        fclose(rf);

        char *banner = str_steal(&display);
        pthread_mutex_lock(&ui->mtx);
        ui_state_set_banner(ui, banner);
        ui_state_set_status(ui, STATUS_READY, "Run log");
        pthread_mutex_unlock(&ui->mtx);
        free(banner);
        tui_render(ui);
    } else {
        /* /runs or /runs list — list all run logs */
        DIR *d = opendir(rdir);
        str_t display = str_new(2048);
        str_appendf(&display, "# Playbook Runs\n\n");

        if (!d) {
            str_appendf(&display, "No runs yet (%s/runs/ not found)\n", ctx->nash_dir);
        } else {
            struct dirent *ent;
            int count = 0;
            /* FIX #16: Use dynamic array instead of fixed char *names[1024].
             * Previously, beyond 1024 runs entries were silently dropped.
             * Also replaced O(N²) bubble sort with qsort. */
            int names_cap = 128;
            char **names = malloc(sizeof(char *) * (size_t)names_cap);
            int nnames = 0;
            if (names) {
                while ((ent = readdir(d))) {
                    int nlen = (int)strlen(ent->d_name);
                    if (nlen > 6 && strcmp(ent->d_name + nlen - 6, ".jsonl") == 0) {
                        if (nnames >= names_cap) {
                            names_cap *= 2;
                            char **tmp = realloc(names, sizeof(char *) * (size_t)names_cap);
                            if (!tmp) break;  /* stop collecting on OOM */
                            names = tmp;
                        }
                        names[nnames++] = strdup(ent->d_name);
                    }
                }
            }
            closedir(d);
            /* Sort descending (newest first by epoch name) */
            if (nnames > 1)
                qsort(names, (size_t)nnames, sizeof(char *), cmp_str_desc);
            for (int i = 0; i < nnames; i++) {
                char fpath[NASH_PATH_MAX + NASH_PATH_MAX];
                snprintf(fpath, sizeof(fpath), "%s/%s", rdir, names[i]);
                FILE *rf = fopen(fpath, "r");
                if (rf) {
                    char line[NASH_LINE_MAX];
                    if (fgets(line, sizeof(line), rf)) {
                        cJSON *ev = cJSON_Parse(line);
                        if (ev) {
                            const char *pb_name = cJSON_GetStringValue(
                                cJSON_GetObjectItem(ev, "pb"));
                            int n = (int)cJSON_GetNumberValue(
                                cJSON_GetObjectItem(ev, "n"));
                            /* Check if run completed by scanning for end event */
                            const char *status_str = "running";
                            char lastline[NASH_LINE_MAX];
                            lastline[0] = '\0';
                            while (fgets(lastline, sizeof(lastline), rf));
                            if (lastline[0]) {
                                cJSON *last = cJSON_Parse(lastline);
                                if (last) {
                                    const char *le = cJSON_GetStringValue(
                                        cJSON_GetObjectItem(last, "e"));
                                    if (le && strcmp(le, "end") == 0) {
                                        const char *st = cJSON_GetStringValue(
                                            cJSON_GetObjectItem(last, "st"));
                                        status_str = (st && strcmp(st, "ok") == 0)
                                            ? "ok" : "fail";
                                    }
                                    cJSON_Delete(last);
                                }
                            }
                            /* Strip .jsonl for display */
                            char id[256];
                            snprintf(id, sizeof(id), "%s", names[i]);
                            char *dot = strstr(id, ".jsonl");
                            if (dot) *dot = '\0';
                            str_appendf(&display,
                                "%d. **%s** — %s (%d passes) [%s]\n",
                                ++count, id,
                                pb_name ? pb_name : "?",
                                n, status_str);
                            cJSON_Delete(ev);
                        }
                    }
                    fclose(rf);
                }
                free(names[i]);
            }
            free(names);  /* FIX #16: free dynamic array */
            if (count == 0)
                str_appendf(&display, "No runs found\n");
            else
                str_appendf(&display, "\nUse `/runs show <id>` for details\n");
        }

        char *banner = str_steal(&display);
        pthread_mutex_lock(&ui->mtx);
        ui_state_set_banner(ui, banner);
        ui_state_set_status(ui, STATUS_READY, "Run list");
        pthread_mutex_unlock(&ui->mtx);
        free(banner);
        tui_render(ui);
    }
    return CMD_CONTINUE;
}

/* ── /memory_recall <query> ────────────────────────────────────── */
static int cmd_memory_recall(command_ctx_t *ctx, const char *query) {
    ui_state_t *ui = ctx->ui;

    /* Strip optional quotes */
    int n = strlen(query);
    const char *q_start = query;
    const char *q_end = query + n;
    if (n >= 2 && q_start[0] == '"' && q_end[-1] == '"') {
        q_start++; q_end--;
    } else if (n >= 2 && q_start[0] == '\'' && q_end[-1] == '\'') {
        q_start++; q_end--;
    }
    int q_len = q_end - q_start;
    if (q_len == 0) {
        pthread_mutex_lock(&ui->mtx);
        ui_state_set_status(ui, STATUS_ERROR,
            "/memory_recall: usage: /memory_recall \"query\"");
        pthread_mutex_unlock(&ui->mtx);
        tui_render(ui);
        return CMD_CONTINUE;
    }
    char qbuf[NASH_PATH_MAX];
    memcpy(qbuf, q_start, q_len);
    qbuf[q_len] = '\0';

    memory_results_t results = ctx->ws
        ? workspace_recall(ctx->ws, qbuf, 10)
        : memory_recall(ctx->memory, qbuf, 10);
    if (results.count == 0) {
        pthread_mutex_lock(&ui->mtx);
        ui_state_set_status(ui, STATUS_READY,
            "No memories matched query");
        pthread_mutex_unlock(&ui->mtx);
        tui_render(ui);
        return CMD_CONTINUE;
    }

    /* Format results as a readable display string */
    str_t display = str_new(4096);
    str_appendf(&display, "# Memory Recall: \"%.*s\"\n\n", q_len, q_start);
    str_appendf(&display, "Found %d matching entries:\n\n", results.count);
    for (int i = 0; i < results.count; i++) {
        memory_entry_t *e = &results.entries[i];
        str_appendf(&display,
            "### %d. %s  (score: %.3f)\n\n",
            i + 1, e->key, e->relevance);
        str_append_cstr(&display, e->value);
        str_append_cstr(&display, "\n\n");
        /* Tags removed */
        (void)0;
        /* Validation score */
        double vscore = (e->recall_hits + 1.0) /
                        (e->recall_hits + e->recall_misses + 2.0);
        str_appendf(&display,
            "hits: %d misses: %d vscore: %.2f pinned: %s\n\n",
            e->recall_hits, e->recall_misses,
            vscore, e->pinned ? "yes" : "no");
        str_append_cstr(&display, "---\n\n");
    }

    char *banner = str_steal(&display);
    pthread_mutex_lock(&ui->mtx);
    ui_state_set_banner(ui, banner);
    ui_state_set_status(ui, STATUS_READY,
        "Memory recall complete");
    pthread_mutex_unlock(&ui->mtx);
    free(banner);
    memory_results_free(&results);
    tui_render(ui);
    return CMD_CONTINUE;
}

/* ── /? (search) ───────────────────────────────────────────────── */
static int cmd_search(command_ctx_t *ctx) {
    ui_state_t *ui = ctx->ui;
    pthread_mutex_lock(&ui->mtx);
    if (ui->search_active) {
        ui_state_search(ui, NULL);  /* clear search */
    }
    pthread_mutex_unlock(&ui->mtx);
    tui_render(ui);
    return CMD_CONTINUE;
}

/* ── Main dispatch ─────────────────────────────────────────────── */
int command_dispatch(command_ctx_t *ctx, char **submitted_query) {
    char *sq = *submitted_query;

    if (strncmp(sq, "/fork ", 6) == 0) {
        int rc = cmd_fork(ctx, sq + 6);
        free(sq);
        *submitted_query = NULL;
        return rc;
    }
    if (strncmp(sq, "/name ", 6) == 0) {
        int rc = cmd_name(ctx, sq + 6);
        free(sq);
        *submitted_query = NULL;
        return rc;
    }
    if (strncmp(sq, "/cwd ", 5) == 0) {
        int rc = cmd_cwd(ctx, sq + 5);
        free(sq);
        *submitted_query = NULL;
        return rc;
    }
    if (strcmp(sq, "/dream") == 0) {
        free(sq);
        *submitted_query = NULL;
        return cmd_dream(ctx);
    }
    if (strncmp(sq, "/play ", 6) == 0) {
        int rc = cmd_play(ctx, sq + 6);
        free(sq);
        *submitted_query = NULL;
        return rc;
    }
    if (strcmp(sq, "/runs") == 0 ||
        strcmp(sq, "/runs list") == 0 ||
        strncmp(sq, "/runs ", 6) == 0) {
        int rc = cmd_runs(ctx, sq + 5);
        free(sq);
        *submitted_query = NULL;
        return rc;
    }
    if (strncmp(sq, "/memory_recall ", 15) == 0) {
        int rc = cmd_memory_recall(ctx, sq + 15);
        free(sq);
        *submitted_query = NULL;
        return rc;
    }
    if (strncmp(sq, "/?", 2) == 0) {
        int rc = cmd_search(ctx);
        free(sq);
        *submitted_query = NULL;
        return rc;
    }

    /* Handle /continue: resolve to original query from checkpoint */
    if (strcmp(sq, "continue") == 0 ||
        strcmp(sq, "/continue") == 0) {
        char *orig = checkpoint_read_query(*ctx->session_dir);
        if (orig) {
            free(sq);
            *submitted_query = orig;
        }
        /* Fall through — let caller dispatch as regular query */
        return CMD_NOT_FOUND;
    }

    return CMD_NOT_FOUND;
}
