#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

#include "commands.h"
#include "agents.h"
#include "cJSON.h"
#include "str.h"
#include "nash_limits.h"
#include "scratchpad.h"
#include "tui.h"
#include "ui_state_internal.h"
#include "session_search.h"
#include "embedding.h"
#include <time.h>

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
    session_lock_release(tools->session_lock_fd);  /* release old session lock */
    journal_free(*ctx->journal);
    free(session_dir);
    *ctx->session_dir = new_dir;
    *ctx->journal = journal_new(new_dir);
    tools->journal = *ctx->journal;
    tools->session_dir = new_dir;
    tools->session_lock_fd = session_lock_acquire(new_dir);  /* lock new session */
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
    char resolved[NASH_PATH_MAX];
    if (!getcwd(resolved, sizeof(resolved)))
        snprintf(resolved, sizeof(resolved), "%s", dir);
    char status_msg[NASH_PATH_MAX + 16];
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
        ui_state_push_content(ui, "playbooks", banner);
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
        ui_state_push_content(ui, "run-detail", banner);
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
        ui_state_push_content(ui, "runs", banner);
        ui_state_set_status(ui, STATUS_READY, "Run list");
        pthread_mutex_unlock(&ui->mtx);
        free(banner);
        tui_render(ui);
    }
    return CMD_CONTINUE;
}

/* ── Argument parser for /memory_search (Option C: short+long flags) ── */

/* Extract a quoted or unquoted token starting at *p.
 * Advances *p past the token (and any trailing whitespace).
 * Returns a pointer into a static buffer (overwritten each call). */
static const char *ms_next_token(const char **p) {
    static char tokbuf[NASH_PATH_MAX];
    const char *s = *p;
    while (*s == ' ') s++;
    if (!*s) { *p = s; return NULL; }

    int i = 0;
    if (*s == '"') {
        s++; /* skip opening quote */
        while (*s && *s != '"' && i < NASH_PATH_MAX - 1)
            tokbuf[i++] = *s++;
        if (*s == '"') s++; /* skip closing quote */
    } else if (*s == '\'') {
        s++;
        while (*s && *s != '\'' && i < NASH_PATH_MAX - 1)
            tokbuf[i++] = *s++;
        if (*s == '\'') s++;
    } else {
        while (*s && *s != ' ' && i < NASH_PATH_MAX - 1)
            tokbuf[i++] = *s++;
    }
    tokbuf[i] = '\0';
    while (*s == ' ') s++;
    *p = s;
    return tokbuf;
}

typedef struct {
    char query[NASH_PATH_MAX];
    char key[NASH_PATH_MAX];
    char pattern[NASH_PATH_MAX];
    int  use_regex;
    int  max_results;   /* 0 = default */
    int  days;           /* 0 = no limit */
} ms_args_t;

static int ms_parse_args(const char *input, ms_args_t *args) {
    memset(args, 0, sizeof(*args));
    const char *p = input;
    while (*p == ' ') p++;
    if (!*p) return -1; /* empty */

    /* Collect bare words (no flag prefix) into query */
    str_t bare = str_new(256);

    while (*p) {
        if (*p == '-') {
            const char *flag_start = p;
            p++; /* skip first '-' */
            if (*p == '-') p++; /* skip second '-' for long flags */
            /* Read flag name */
            char flag[32] = {0};
            int fi = 0;
            while (*p && *p != ' ' && *p != '=' && fi < 30)
                flag[fi++] = *p++;
            if (*p == '=') p++;
            while (*p == ' ') p++;

            if (strcmp(flag, "q") == 0 || strcmp(flag, "query") == 0) {
                const char *tok = ms_next_token(&p);
                if (tok) snprintf(args->query, sizeof(args->query), "%s", tok);
            } else if (strcmp(flag, "k") == 0 || strcmp(flag, "key") == 0) {
                const char *tok = ms_next_token(&p);
                if (tok) snprintf(args->key, sizeof(args->key), "%s", tok);
            } else if (strcmp(flag, "p") == 0 || strcmp(flag, "pattern") == 0) {
                const char *tok = ms_next_token(&p);
                if (tok) snprintf(args->pattern, sizeof(args->pattern), "%s", tok);
            } else if (strcmp(flag, "r") == 0 || strcmp(flag, "regex") == 0) {
                args->use_regex = 1;
            } else if (strcmp(flag, "n") == 0 || strcmp(flag, "max") == 0) {
                const char *tok = ms_next_token(&p);
                if (tok) {
                    args->max_results = atoi(tok);
                    if (args->max_results < 1) args->max_results = 1;
                    if (args->max_results > 100) args->max_results = 100;
                }
            } else if (strcmp(flag, "d") == 0 || strcmp(flag, "days") == 0) {
                const char *tok = ms_next_token(&p);
                if (tok) {
                    args->days = atoi(tok);
                    if (args->days < 0) args->days = 0;
                }
            } else {
                /* Unknown flag — treat from flag_start as bare word */
                const char *tok = flag_start;
                while (*tok && *tok != ' ') tok++;
                if (bare.len > 0) str_append_cstr(&bare, " ");
                str_append(&bare, flag_start, tok - flag_start);
                p = tok;
                while (*p == ' ') p++;
            }
        } else {
            /* Bare word — accumulate into query */
            const char *tok = ms_next_token(&p);
            if (tok) {
                if (bare.len > 0) str_append_cstr(&bare, " ");
                str_append_cstr(&bare, tok);
            }
        }
    }

    /* If we got bare words and no explicit -q, use them as query */
    if (bare.len > 0 && args->query[0] == '\0') {
        snprintf(args->query, sizeof(args->query), "%s",
                 bare.data ? bare.data : "");
    }
    str_free(&bare);

    /* Must have at least one search parameter */
    if (args->query[0] == '\0' && args->key[0] == '\0' &&
        args->pattern[0] == '\0')
        return -1;

    return 0;
}

/* ── /memory_search — full hybrid search (curated memory + sessions) ── */
static int cmd_memory_query(command_ctx_t *ctx, const char *input) {
    ui_state_t *ui = ctx->ui;

    ms_args_t args;
    if (ms_parse_args(input, &args) < 0) {
        pthread_mutex_lock(&ui->mtx);
        ui_state_set_status(ui, STATUS_ERROR,
            "Usage: /ms [-q query] [-k key] [-p pattern] [-r] [-n max] [-d days]");
        pthread_mutex_unlock(&ui->mtx);
        tui_render(ui);
        return CMD_CONTINUE;
    }

    const char *query   = args.query[0]   ? args.query   : NULL;
    const char *key     = args.key[0]     ? args.key     : NULL;
    const char *pattern = args.pattern[0] ? args.pattern : NULL;
    int max_results     = args.max_results;
    int days            = args.days;
    int use_regex       = args.use_regex;

    int mem_count = 0, ses_count = 0;
    memory_results_t mem_results = {0};
    ss_results_t ses_results = {0};

    static const char *conf_labels[] = {"LOW", "MEDIUM", "HIGH"};

    /* ── Exact key lookup (bypasses scoring) ──────── */
    if (key && !query && !pattern) {
        if (ctx->memory || ctx->ws) {
            mem_results = ctx->ws
                ? workspace_recall(ctx->ws, key, 1)
                : memory_query(ctx->memory, key, 1);
            mem_count = mem_results.count;
        }
        if (mem_count == 0) {
            pthread_mutex_lock(&ui->mtx);
            ui_state_set_status(ui, STATUS_READY,
                "No memory found for that key");
            pthread_mutex_unlock(&ui->mtx);
            tui_render(ui);
            return CMD_CONTINUE;
        }
        /* Fall through to display */
    }

    /* ── L4: Curated memory search ──────────────── */
    if (!key && query && (ctx->memory || ctx->ws)) {
        int mem_max = max_results > 0 ? (max_results < 10 ? max_results : 10) : 5;
        mem_results = ctx->ws
            ? workspace_recall(ctx->ws, query, mem_max)
            : memory_query(ctx->memory, query, mem_max);
        mem_count = mem_results.count;
    }

    /* ── L3: Session history search ─────────────── */
    if (!key && (query || pattern)) {
        char sessions_dir[NASH_PATH_MAX] = {0};
        if (ctx->session_dir && *ctx->session_dir) {
            snprintf(sessions_dir, sizeof(sessions_dir), "%s", *ctx->session_dir);
            char *last_slash = strrchr(sessions_dir, '/');
            if (last_slash) *last_slash = '\0';
        }

        embed_ctx_t *embed = NULL;
        if (ctx->memory && memory_has_embeddings(ctx->memory))
            embed = memory_embed_ctx(ctx->memory);

        int ses_max = max_results > 0 ? max_results : 5;
        if (!pattern && mem_count >= 3 && ses_max > 2) ses_max = 2;

        ses_results = session_search(
            ctx->tools ? ctx->tools->session_idx : NULL,
            embed, query, pattern,
            use_regex, ses_max, days,
            sessions_dir[0] ? sessions_dir : NULL);
        ses_count = ses_results.count;
    }

    /* ── Build md_file (search.md with hyperlinks) ── */
    str_t md_file = str_new(4096);

    str_append_cstr(&md_file, "# Memory Search Results\n\n");
    if (query)   str_appendf(&md_file, "**Query:** %s\n", query);
    if (key)     str_appendf(&md_file, "**Key:** %s\n", key);
    if (pattern) str_appendf(&md_file, "**Pattern:** `%s`%s\n",
                             pattern, use_regex ? " (regex)" : "");
    if (days > 0) str_appendf(&md_file, "**Days:** %d\n", days);
    str_appendf(&md_file, "**Results:** %d memory, %d session\n\n",
                mem_count, ses_count);

    if (mem_count == 0 && ses_count == 0)
        str_append_cstr(&md_file, "*No matches found.*\n");

    /* ── Interleave results by score ────────────── */
    {
        int mi = 0, si = 0;
        int total_emitted = 0;
        int emit_limit = max_results > 0 ? max_results : 20;
        int rank = 1;

        while (total_emitted < emit_limit &&
               (mi < mem_count || si < ses_count)) {
            double mem_score = (mi < mem_count)
                ? mem_results.entries[mi].relevance : -1.0;
            double ses_score = (si < ses_count)
                ? ses_results.results[si].composite_score : -1.0;

            if (mem_score >= ses_score && mi < mem_count) {
                /* Emit memory result */
                memory_entry_t *e = &mem_results.entries[mi];
                str_appendf(&md_file,
                    "### %d. 🧠 %s  (score: %.3f)\n\n",
                    rank, e->key, e->relevance);
                str_append_cstr(&md_file, e->value);
                str_append_cstr(&md_file, "\n\n");
                double vscore = (e->recall_hits + 1.0) /
                                (e->recall_hits + e->recall_misses + 2.0);
                str_appendf(&md_file,
                    "hits: %d misses: %d vscore: %.2f pinned: %s\n\n",
                    e->recall_hits, e->recall_misses,
                    vscore, e->pinned ? "yes" : "no");
                str_append_cstr(&md_file, "---\n\n");
                mi++;
            } else if (si < ses_count) {
                /* Emit session result */
                ss_result_t *r = &ses_results.results[si];
                time_t ts = (time_t)r->timestamp;
                struct tm tm_buf;
                struct tm *tm = gmtime_r(&ts, &tm_buf);
                char ts_buf[32];
                if (tm)
                    strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%d %H:%M", tm);
                else
                    snprintf(ts_buf, sizeof(ts_buf), "%.0f", r->timestamp);

                int has_lex = (r->n_matches > 0);
                int has_sem = (r->semantic_score > 0.01);

                str_appendf(&md_file,
                    "### %d. 📅 %s  %s  (score: %.3f",
                    rank, ts_buf, conf_labels[r->confidence],
                    r->composite_score);
                if (has_sem)
                    str_appendf(&md_file, " sem=%.2f", r->semantic_score);
                if (has_lex)
                    str_appendf(&md_file, " lex=%.2f matches=%d",
                                r->lexical_score, r->match_count);
                str_append_cstr(&md_file, ")\n\n");

                /* Session dir — rendered as OSC 8 clickable link in TUI */
                if (r->session_dir)
                    str_appendf(&md_file, "    [%s](%s/session.md)\n\n",
                                r->session_dir, r->session_dir);

                if (r->chunk_preview && r->chunk_preview[0])
                    str_appendf(&md_file, "%s\n\n", r->chunk_preview);

                /* Per-line lexical matches — RXSY as hyperlinks */
                for (int j = 0; j < r->n_matches; j++) {
                    ss_match_t *m = &r->matches[j];
                    if (r->session_dir) {
                        str_appendf(&md_file,
                            "  [R%dS%d](%s/R%dS%d) [%s]: %s\n",
                            m->react_loop, m->step,
                            r->session_dir,
                            m->react_loop, m->step,
                            m->tool,
                            m->snippet ? m->snippet : "");
                    } else {
                        str_appendf(&md_file, "  R%dS%d [%s]: %s\n",
                                    m->react_loop, m->step, m->tool,
                                    m->snippet ? m->snippet : "");
                    }
                }
                if (r->match_count > r->n_matches)
                    str_appendf(&md_file, "  ... and %d more match%s\n",
                                r->match_count - r->n_matches,
                                (r->match_count - r->n_matches) == 1
                                    ? "" : "es");
                str_append_cstr(&md_file, "\n---\n\n");
                si++;
            }
            total_emitted++;
            rank++;
        }
    }

    /* ── Write search.md to session directory ──── */
    if (ctx->session_dir && *ctx->session_dir) {
        char md_path[NASH_PATH_MAX];
        snprintf(md_path, sizeof(md_path), "%s/search.md", *ctx->session_dir);
        FILE *fp = fopen(md_path, "w");
        if (fp) {
            const char *md_data = str_cstr(&md_file);
            if (md_data)
                fputs(md_data, fp);
            fclose(fp);
        }
    }

    /* ── Pass markdown with links to TUI for OSC 8 rendering ── */
    char *banner = strdup(str_cstr(&md_file));
    str_free(&md_file);
    pthread_mutex_lock(&ui->mtx);
    ui_state_push_content(ui, "memory-search", banner);
    ui_state_set_status(ui, STATUS_READY,
        "Memory search complete");
    pthread_mutex_unlock(&ui->mtx);
    free(banner);
    memory_results_free(&mem_results);
    if (ses_count > 0) ss_results_free(&ses_results);
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

/* ── /todo — per-workspace TODO list ───────────────────────────── */

/* Derive todo.md path from workspace (same logic as tool_todo.c) */
static int cmd_todo_path(command_ctx_t *ctx, char *buf, size_t sz) {
    const char *mdir = NULL;
    if (ctx->ws && ctx->ws->workspace)
        mdir = memory_dir(ctx->ws->workspace);
    else if (ctx->ws && ctx->ws->global)
        mdir = memory_dir(ctx->ws->global);
    else if (ctx->memory)
        mdir = memory_dir(ctx->memory);
    if (!mdir) return -1;

    size_t mlen = strlen(mdir);
    const char *suffix = "/memory";
    size_t slen = strlen(suffix);
    if (mlen > slen && strcmp(mdir + mlen - slen, suffix) == 0) {
        size_t rlen = mlen - slen;
        if (rlen + sizeof("/todo.md") > sz) return -1;
        memcpy(buf, mdir, rlen);
        memcpy(buf + rlen, "/todo.md", sizeof("/todo.md"));
    } else {
        snprintf(buf, sz, "%s/../todo.md", mdir);
    }
    return 0;
}

/* Check if user is currently viewing todo.md */
static int viewing_todo(ui_state_t *ui) {
    if (!ui->current_filepath) return 0;
    const char *base = strrchr(ui->current_filepath, '/');
    base = base ? base + 1 : ui->current_filepath;
    return strcmp(base, "todo.md") == 0;
}

/* Regenerate the rendered todo.md in session_dir from workspace todo.md.
 * If the user is viewing todo.md, reload it so the change is visible. */
static void cmd_todo_refresh(ui_state_t *ui, const char *ws_todo_path,
                             const char *ws_name) {
    if (!ui->session_dir) return;

    char tpath[NASH_PATH_MAX];
    snprintf(tpath, sizeof(tpath), "%s/todo.md", ui->session_dir);

    /* Build rendered content */
    str_t out = str_new(1024);
    str_append_cstr(&out, "# TODO List");
    if (ws_name) str_appendf(&out, " (%s)", ws_name);
    str_append_cstr(&out, "\n\n");

    FILE *f = fopen(ws_todo_path, "r");
    if (f) {
        int num = 0, open = 0, done_n = 0;
        char linebuf[4096];
        while (fgets(linebuf, sizeof(linebuf), f)) {
            size_t len = strlen(linebuf);
            while (len > 0 && (linebuf[len-1] == '\n' || linebuf[len-1] == '\r'))
                linebuf[--len] = '\0';
            if (len == 0) continue;
            num++;
            str_appendf(&out, "%d. %s\n", num, linebuf);
            if (strstr(linebuf, "- [ ]")) open++;
            else if (strstr(linebuf, "- [x]")) done_n++;
        }
        fclose(f);
        if (num == 0)
            str_append_cstr(&out, "*No items yet.* Use `/todo add <text>` to add one.\n");
        else
            str_appendf(&out, "\n**%d open, %d done** — `/todo add|done|remove|purge`\n",
                        open, done_n);
    } else {
        str_append_cstr(&out, "*No items yet.* Use `/todo add <text>` to add one.\n");
    }

    FILE *tf = fopen(tpath, "w");
    if (tf) { fputs(str_cstr(&out), tf); fclose(tf); }
    str_free(&out);

    pthread_mutex_lock(&ui->mtx);
    if (viewing_todo(ui))
        ui_state_reload_file(ui);
    pthread_mutex_unlock(&ui->mtx);
}

static int cmd_todo(command_ctx_t *ctx, const char *args) {
    ui_state_t *ui = ctx->ui;
    char fpath[NASH_PATH_MAX];

    if (cmd_todo_path(ctx, fpath, sizeof(fpath)) != 0) {
        pthread_mutex_lock(&ui->mtx);
        ui_state_set_status(ui, STATUS_ERROR,
            "Cannot determine todo.md path (no workspace or memory)");
        pthread_mutex_unlock(&ui->mtx);
        tui_render(ui);
        return CMD_CONTINUE;
    }

    /* Skip leading whitespace */
    while (*args == ' ') args++;

    /* /todo add <text> */
    if (strncmp(args, "add ", 4) == 0) {
        const char *text = args + 4;
        while (*text == ' ') text++;
        if (!*text) {
            pthread_mutex_lock(&ui->mtx);
            ui_state_set_status(ui, STATUS_ERROR, "/todo add <text>");
            pthread_mutex_unlock(&ui->mtx);
            tui_render(ui);
            return CMD_CONTINUE;
        }
        /* Load existing lines, append new one, save atomically */
        char **lines = NULL;
        int count = 0, cap = 0;
        char linebuf[4096];
        FILE *f = fopen(fpath, "r");
        if (f) {
            while (fgets(linebuf, sizeof(linebuf), f)) {
                size_t len = strlen(linebuf);
                while (len > 0 && (linebuf[len-1] == '\n' || linebuf[len-1] == '\r'))
                    linebuf[--len] = '\0';
                if (len == 0) continue;
                if (count >= cap) {
                    cap = cap ? cap * 2 : 16;
                    char **tmp2 = realloc(lines, sizeof(char*) * (size_t)cap);
                    if (!tmp2) {
                        for (int i = 0; i < count; i++) free(lines[i]);
                        free(lines); fclose(f);
                        pthread_mutex_lock(&ui->mtx);
                        ui_state_set_status(ui, STATUS_ERROR, "Out of memory");
                        pthread_mutex_unlock(&ui->mtx); tui_render(ui);
                        return CMD_CONTINUE;
                    }
                    lines = tmp2;
                }
                char *dup = strdup(linebuf);
                if (!dup) {
                    for (int i = 0; i < count; i++) free(lines[i]);
                    free(lines); fclose(f);
                    pthread_mutex_lock(&ui->mtx);
                    ui_state_set_status(ui, STATUS_ERROR, "Out of memory");
                    pthread_mutex_unlock(&ui->mtx); tui_render(ui);
                    return CMD_CONTINUE;
                }
                lines[count++] = dup;
            }
            fclose(f);
        }
        /* Ensure capacity for new line */
        if (count >= cap) {
            cap = cap ? cap * 2 : 16;
            char **tmp2 = realloc(lines, sizeof(char*) * (size_t)cap);
            if (!tmp2) {
                for (int i = 0; i < count; i++) free(lines[i]);
                free(lines);
                pthread_mutex_lock(&ui->mtx);
                ui_state_set_status(ui, STATUS_ERROR, "Out of memory");
                pthread_mutex_unlock(&ui->mtx); tui_render(ui);
                return CMD_CONTINUE;
            }
            lines = tmp2;
        }
        char new_line[4096];
        snprintf(new_line, sizeof(new_line), "- [ ] %s", text);
        char *dup = strdup(new_line);
        if (!dup) {
            for (int i = 0; i < count; i++) free(lines[i]);
            free(lines);
            pthread_mutex_lock(&ui->mtx);
            ui_state_set_status(ui, STATUS_ERROR, "Out of memory");
            pthread_mutex_unlock(&ui->mtx); tui_render(ui);
            return CMD_CONTINUE;
        }
        lines[count++] = dup;

        /* Atomic save */
        char tmp[NASH_PATH_MAX + 8];
        snprintf(tmp, sizeof(tmp), "%s.tmp", fpath);
        f = fopen(tmp, "w");
        if (!f) {
            /* Try creating parent dir */
            char *slash = strrchr(fpath, '/');
            if (slash) { *slash = '\0'; mkdir(fpath, 0755); *slash = '/'; }
            f = fopen(tmp, "w");
        }
        if (!f) {
            for (int i = 0; i < count; i++) free(lines[i]);
            free(lines);
            pthread_mutex_lock(&ui->mtx);
            ui_state_set_status(ui, STATUS_ERROR, "Cannot write todo.md");
            pthread_mutex_unlock(&ui->mtx);
            tui_render(ui);
            return CMD_CONTINUE;
        }
        for (int i = 0; i < count; i++) fprintf(f, "%s\n", lines[i]);
        fclose(f);
        if (rename(tmp, fpath) != 0) unlink(tmp);
        for (int i = 0; i < count; i++) free(lines[i]);
        free(lines);

        char status[256];
        snprintf(status, sizeof(status), "Added: %s", text);
        cmd_todo_refresh(ui, fpath, ctx->ws ? ctx->ws->name : NULL);
        pthread_mutex_lock(&ui->mtx);
        ui_state_set_status(ui, STATUS_READY, status);
        pthread_mutex_unlock(&ui->mtx);
        tui_render(ui);
        return CMD_CONTINUE;
    }

    /* /todo done <N> */
    if (strncmp(args, "done ", 5) == 0) {
        const char *num_start = args + 5;
        while (*num_start == ' ') num_start++;
        char *endptr;
        long val = strtol(num_start, &endptr, 10);
        if (*endptr != '\0' || endptr == num_start) {
            pthread_mutex_lock(&ui->mtx);
            ui_state_set_status(ui, STATUS_ERROR, "/todo done <number>");
            pthread_mutex_unlock(&ui->mtx);
            tui_render(ui);
            return CMD_CONTINUE;
        }
        int idx = (int)val;
        if (idx < 1) {
            pthread_mutex_lock(&ui->mtx);
            ui_state_set_status(ui, STATUS_ERROR, "/todo done <number>");
            pthread_mutex_unlock(&ui->mtx);
            tui_render(ui);
            return CMD_CONTINUE;
        }
        /* Load, flip, save */
        FILE *f = fopen(fpath, "r");
        if (!f) {
            pthread_mutex_lock(&ui->mtx);
            ui_state_set_status(ui, STATUS_ERROR, "No todo.md found");
            pthread_mutex_unlock(&ui->mtx);
            tui_render(ui);
            return CMD_CONTINUE;
        }
        char **lines = NULL;
        int count = 0, cap = 0;
        char linebuf[4096];
        while (fgets(linebuf, sizeof(linebuf), f)) {
            size_t len = strlen(linebuf);
            while (len > 0 && (linebuf[len-1] == '\n' || linebuf[len-1] == '\r'))
                linebuf[--len] = '\0';
            if (len == 0) continue;
            if (count >= cap) {
                cap = cap ? cap * 2 : 16;
                char **tmp = realloc(lines, sizeof(char*) * (size_t)cap);
                if (!tmp) {
                    for (int i = 0; i < count; i++) free(lines[i]);
                    free(lines); fclose(f);
                    pthread_mutex_lock(&ui->mtx);
                    ui_state_set_status(ui, STATUS_ERROR, "Out of memory");
                    pthread_mutex_unlock(&ui->mtx); tui_render(ui);
                    return CMD_CONTINUE;
                }
                lines = tmp;
            }
            char *dup = strdup(linebuf);
            if (!dup) {
                for (int i = 0; i < count; i++) free(lines[i]);
                free(lines); fclose(f);
                pthread_mutex_lock(&ui->mtx);
                ui_state_set_status(ui, STATUS_ERROR, "Out of memory");
                pthread_mutex_unlock(&ui->mtx); tui_render(ui);
                return CMD_CONTINUE;
            }
            lines[count++] = dup;
        }
        fclose(f);
        if (idx > count) {
            for (int i = 0; i < count; i++) free(lines[i]);
            free(lines);
            pthread_mutex_lock(&ui->mtx);
            ui_state_set_status(ui, STATUS_ERROR, "Index out of range");
            pthread_mutex_unlock(&ui->mtx);
            tui_render(ui);
            return CMD_CONTINUE;
        }
        char *check = strstr(lines[idx-1], "- [ ]");
        if (!check) {
            for (int i = 0; i < count; i++) free(lines[i]);
            free(lines);
            pthread_mutex_lock(&ui->mtx);
            ui_state_set_status(ui, STATUS_ERROR, "Item is not open (already done?)");
            pthread_mutex_unlock(&ui->mtx);
            tui_render(ui);
            return CMD_CONTINUE;
        }
        check[3] = 'x';
        /* Atomic save */
        char tmp[NASH_PATH_MAX + 8];
        snprintf(tmp, sizeof(tmp), "%s.tmp", fpath);
        f = fopen(tmp, "w");
        if (f) {
            for (int i = 0; i < count; i++) fprintf(f, "%s\n", lines[i]);
            fclose(f);
            if (rename(tmp, fpath) != 0) unlink(tmp);
        } else {
            unlink(tmp);
        }
        char status[256];
        snprintf(status, sizeof(status), "Done: %s", lines[idx-1]);
        for (int i = 0; i < count; i++) free(lines[i]);
        free(lines);
        cmd_todo_refresh(ui, fpath, ctx->ws ? ctx->ws->name : NULL);
        pthread_mutex_lock(&ui->mtx);
        ui_state_set_status(ui, STATUS_READY, status);
        pthread_mutex_unlock(&ui->mtx);
        tui_render(ui);
        return CMD_CONTINUE;
    }

    /* /todo remove <N> */
    {
        const char *num_start = NULL;
        if (strncmp(args, "remove ", 7) == 0) num_start = args + 7;
        else if (strncmp(args, "rm ", 3) == 0) num_start = args + 3;
        else if (strcmp(args, "remove") == 0 || strcmp(args, "rm") == 0) {
            pthread_mutex_lock(&ui->mtx);
            ui_state_set_status(ui, STATUS_ERROR, "/todo remove <number>");
            pthread_mutex_unlock(&ui->mtx);
            tui_render(ui);
            return CMD_CONTINUE;
        }
        if (num_start) {
            while (*num_start == ' ') num_start++;
            char *endptr;
            long val = strtol(num_start, &endptr, 10);
            if (*endptr != '\0' || endptr == num_start) {
                pthread_mutex_lock(&ui->mtx);
                ui_state_set_status(ui, STATUS_ERROR, "/todo remove <number>");
                pthread_mutex_unlock(&ui->mtx);
                tui_render(ui);
                return CMD_CONTINUE;
            }
            int idx = (int)val;
            if (idx < 1) {
            pthread_mutex_lock(&ui->mtx);
            ui_state_set_status(ui, STATUS_ERROR, "/todo remove <number>");
            pthread_mutex_unlock(&ui->mtx);
            tui_render(ui);
            return CMD_CONTINUE;
        }
        FILE *f = fopen(fpath, "r");
        if (!f) {
            pthread_mutex_lock(&ui->mtx);
            ui_state_set_status(ui, STATUS_ERROR, "No todo.md found");
            pthread_mutex_unlock(&ui->mtx);
            tui_render(ui);
            return CMD_CONTINUE;
        }
        char **lines = NULL;
        int count = 0, cap = 0;
        char linebuf[4096];
        while (fgets(linebuf, sizeof(linebuf), f)) {
            size_t len = strlen(linebuf);
            while (len > 0 && (linebuf[len-1] == '\n' || linebuf[len-1] == '\r'))
                linebuf[--len] = '\0';
            if (len == 0) continue;
            if (count >= cap) {
                cap = cap ? cap * 2 : 16;
                char **tmp2 = realloc(lines, sizeof(char*) * (size_t)cap);
                if (!tmp2) {
                    for (int i = 0; i < count; i++) free(lines[i]);
                    free(lines); fclose(f);
                    pthread_mutex_lock(&ui->mtx);
                    ui_state_set_status(ui, STATUS_ERROR, "Out of memory");
                    pthread_mutex_unlock(&ui->mtx); tui_render(ui);
                    return CMD_CONTINUE;
                }
                lines = tmp2;
            }
            char *dup = strdup(linebuf);
            if (!dup) {
                for (int i = 0; i < count; i++) free(lines[i]);
                free(lines); fclose(f);
                pthread_mutex_lock(&ui->mtx);
                ui_state_set_status(ui, STATUS_ERROR, "Out of memory");
                pthread_mutex_unlock(&ui->mtx); tui_render(ui);
                return CMD_CONTINUE;
            }
            lines[count++] = dup;
        }
        fclose(f);
        if (idx > count) {
            for (int i = 0; i < count; i++) free(lines[i]);
            free(lines);
            pthread_mutex_lock(&ui->mtx);
            ui_state_set_status(ui, STATUS_ERROR, "Index out of range");
            pthread_mutex_unlock(&ui->mtx);
            tui_render(ui);
            return CMD_CONTINUE;
        }
        char status[256];
        snprintf(status, sizeof(status), "Removed: %s", lines[idx-1]);
        free(lines[idx-1]);
        for (int i = idx-1; i < count-1; i++) lines[i] = lines[i+1];
        count--;
        char tmp[NASH_PATH_MAX + 8];
        snprintf(tmp, sizeof(tmp), "%s.tmp", fpath);
        f = fopen(tmp, "w");
        if (f) {
            for (int i = 0; i < count; i++) fprintf(f, "%s\n", lines[i]);
            fclose(f);
            if (rename(tmp, fpath) != 0) unlink(tmp);
        } else {
            unlink(tmp);
        }
        for (int i = 0; i < count; i++) free(lines[i]);
        free(lines);
        cmd_todo_refresh(ui, fpath, ctx->ws ? ctx->ws->name : NULL);
        pthread_mutex_lock(&ui->mtx);
        ui_state_set_status(ui, STATUS_READY, status);
        pthread_mutex_unlock(&ui->mtx);
        tui_render(ui);
        return CMD_CONTINUE;
        }
    }

    /* /todo purge */
    if (strcmp(args, "purge") == 0) {
        FILE *f = fopen(fpath, "r");
        if (!f) {
            pthread_mutex_lock(&ui->mtx);
            ui_state_set_status(ui, STATUS_READY, "No todo.md found (nothing to purge)");
            pthread_mutex_unlock(&ui->mtx);
            tui_render(ui);
            return CMD_CONTINUE;
        }
        char **lines = NULL;
        int count = 0, cap = 0;
        char linebuf[4096];
        while (fgets(linebuf, sizeof(linebuf), f)) {
            size_t len = strlen(linebuf);
            while (len > 0 && (linebuf[len-1] == '\n' || linebuf[len-1] == '\r'))
                linebuf[--len] = '\0';
            if (len == 0) continue;
            if (count >= cap) {
                cap = cap ? cap * 2 : 16;
                char **tmp2 = realloc(lines, sizeof(char*) * (size_t)cap);
                if (!tmp2) {
                    for (int i = 0; i < count; i++) free(lines[i]);
                    free(lines); fclose(f);
                    pthread_mutex_lock(&ui->mtx);
                    ui_state_set_status(ui, STATUS_ERROR, "Out of memory");
                    pthread_mutex_unlock(&ui->mtx); tui_render(ui);
                    return CMD_CONTINUE;
                }
                lines = tmp2;
            }
            char *dup = strdup(linebuf);
            if (!dup) {
                for (int i = 0; i < count; i++) free(lines[i]);
                free(lines); fclose(f);
                pthread_mutex_lock(&ui->mtx);
                ui_state_set_status(ui, STATUS_ERROR, "Out of memory");
                pthread_mutex_unlock(&ui->mtx); tui_render(ui);
                return CMD_CONTINUE;
            }
            lines[count++] = dup;
        }
        fclose(f);
        int kept = 0, purged = 0;
        for (int i = 0; i < count; i++) {
            if (strstr(lines[i], "- [x]")) { free(lines[i]); lines[i] = NULL; purged++; }
        }
        for (int i = 0; i < count; i++) { if (lines[i]) lines[kept++] = lines[i]; }
        char tmp[NASH_PATH_MAX + 8];
        snprintf(tmp, sizeof(tmp), "%s.tmp", fpath);
        f = fopen(tmp, "w");
        if (f) {
            for (int i = 0; i < kept; i++) fprintf(f, "%s\n", lines[i]);
            fclose(f);
            if (rename(tmp, fpath) != 0) unlink(tmp);
        } else {
            unlink(tmp);
        }
        for (int i = 0; i < kept; i++) free(lines[i]);
        free(lines);
        char status[128];
        snprintf(status, sizeof(status), "Purged %d completed items, %d remaining", purged, kept);
        cmd_todo_refresh(ui, fpath, ctx->ws ? ctx->ws->name : NULL);
        pthread_mutex_lock(&ui->mtx);
        ui_state_set_status(ui, STATUS_READY, status);
        pthread_mutex_unlock(&ui->mtx);
        tui_render(ui);
        return CMD_CONTINUE;
    }

    /* /todo  or  /todo list — display all items */
    {
        str_t out = str_new(1024);
        str_append_cstr(&out, "# TODO List");
        if (ctx->ws && ctx->ws->name)
            str_appendf(&out, " (%s)", ctx->ws->name);
        str_append_cstr(&out, "\n\n");

        FILE *f = fopen(fpath, "r");
        /* Check for unknown subcommand first */
        if (strcmp(args, "list") != 0 && args[0] != '\0') {
            if (f) fclose(f);
            str_free(&out);
            char status[256];
            snprintf(status, sizeof(status),
                "Unknown subcommand. Usage: /todo [list|add <text>|done <N>|remove <N>|purge]");
            pthread_mutex_lock(&ui->mtx);
            ui_state_set_status(ui, STATUS_ERROR, status);
            pthread_mutex_unlock(&ui->mtx);
            tui_render(ui);
            return CMD_CONTINUE;
        }

        if (!f) {
            str_append_cstr(&out, "*No items yet.* Use `/todo add <text>` to add one.\n");
        } else {
            int num = 0, open = 0, done_n = 0;
            char linebuf[4096];
            while (fgets(linebuf, sizeof(linebuf), f)) {
                size_t len = strlen(linebuf);
                while (len > 0 && (linebuf[len-1] == '\n' || linebuf[len-1] == '\r'))
                    linebuf[--len] = '\0';
                if (len == 0) continue;
                num++;
                str_appendf(&out, "%d. %s\n", num, linebuf);
                if (strstr(linebuf, "- [ ]")) open++;
                else if (strstr(linebuf, "- [x]")) done_n++;
            }
            fclose(f);
            if (num == 0) {
                str_append_cstr(&out, "*No items yet.* Use `/todo add <text>` to add one.\n");
            } else {
                str_appendf(&out, "\n**%d open, %d done** — `/todo add|done|remove|purge`\n",
                            open, done_n);
            }
        }

        /* Write rendered TODO to {session_dir}/todo.md and navigate there.
         * This keeps session.md clean — TODO gets its own file. */
        if (ui->session_dir) {
            char tpath[NASH_PATH_MAX];
            snprintf(tpath, sizeof(tpath), "%s/todo.md", ui->session_dir);
            FILE *tf = fopen(tpath, "w");
            if (tf) {
                fputs(str_cstr(&out), tf);
                fclose(tf);
            }
            str_free(&out);
            pthread_mutex_lock(&ui->mtx);
            ui_state_set_status(ui, STATUS_READY, "TODO list");
            free(ui->current_filepath);
            ui->current_filepath = NULL;
            char *new_path = strdup(tpath);
            if (!new_path) {
                pthread_mutex_unlock(&ui->mtx);
                return CMD_CONTINUE;
            }
            ui->current_filepath = new_path;
            ui->scroll_y = 0;
            ui->scroll_x = 0;
            ui->cursor_link = 0;
            ui->search_active = 0;
            ui_state_reload_file(ui);
            pthread_mutex_unlock(&ui->mtx);
        } else {
            str_free(&out);
        }
        tui_render(ui);
        return CMD_CONTINUE;
    }
}

/* ── /agent [list|show|run|history|due|result] ────────────────── */

/* Helper: find an agent by exact or suffix match.
 * Returns pointer into q->agents[] or NULL. */
static const agent_entry_t *agent_find(const agent_queue_t *q, const char *id) {
    /* Exact match */
    for (int i = 0; i < q->n_agents; i++)
        if (strcmp(q->agents[i].id, id) == 0) return &q->agents[i];
    /* Suffix match: "daily-ai-news" matches "ai-news/daily-ai-news" */
    int id_len = (int)strlen(id);
    const agent_entry_t *match = NULL;
    int n_matches = 0;
    for (int i = 0; i < q->n_agents; i++) {
        int aid_len = (int)strlen(q->agents[i].id);
        if (aid_len > id_len &&
            q->agents[i].id[aid_len - id_len - 1] == '/' &&
            strcmp(q->agents[i].id + aid_len - id_len, id) == 0) {
            match = &q->agents[i];
            n_matches++;
        }
    }
    return (n_matches == 1) ? match : NULL;
}

static int cmd_agents_list(command_ctx_t *ctx) {
    ui_state_t *ui = ctx->ui;

    agent_queue_t *q = agent_scan(ctx->nash_dir);
    if (!q) {
        pthread_mutex_lock(&ui->mtx);
        ui_state_set_status(ui, STATUS_ERROR, "/agent: scan failed");
        pthread_mutex_unlock(&ui->mtx);
        tui_render(ui);
        return CMD_CONTINUE;
    }
    agent_queue_load(q, ctx->nash_dir);
    agent_queue_schedule(q, time(NULL));

    str_t display = str_new(2048);
    str_appendf(&display, "# Agents\n\n");

    if (q->n_agents == 0) {
        str_appendf(&display,
            "No agents found.\n\n"
            "Create agent definitions in "
            "`~/.nash/workspaces/<name>/agent/<agent>.yaml`\n");
    } else {
        str_appendf(&display,
            "| Agent | Schedule | Last Run | Status | Due |\n"
            "|-------|----------|----------|--------|-----|\n");

        time_t now = time(NULL);
        for (int i = 0; i < q->n_agents; i++) {
            const agent_entry_t *a = &q->agents[i];

            char last_run_str[64];
            if (a->last_run == 0)
                snprintf(last_run_str, sizeof(last_run_str), "never");
            else {
                char durbuf[32];
                fmt_duration((double)(now - a->last_run), durbuf, sizeof(durbuf));
                snprintf(last_run_str, sizeof(last_run_str), "%s ago", durbuf);
            }

            char status_str[64];
            if (a->last_run == 0)
                snprintf(status_str, sizeof(status_str), "-");
            else {
                char durbuf[32];
                fmt_duration((double)a->last_duration, durbuf, sizeof(durbuf));
                snprintf(status_str, sizeof(status_str), "%s %s (%s)",
                         (a->last_status && strcmp(a->last_status, "ok") == 0) ? "ok" : "FAIL",
                         a->last_status ? a->last_status : "?", durbuf);
            }

            str_appendf(&display, "| %s | `%s` | %s | %s | %s |\n",
                        a->id, a->schedule_str,
                        last_run_str, status_str,
                        a->is_due ? "**yes**" : "no");
        }

        str_appendf(&display,
            "\n**%d agents**, %d due now\n\n"
            "Commands: `/agent show ID`, `/agent run ID`, "
            "`/agent history`, `/agent result ID`\n",
            q->n_agents, q->n_due);
    }

    char *banner = str_steal(&display);
    pthread_mutex_lock(&ui->mtx);
    ui_state_push_content(ui, "agents", banner);
    ui_state_set_status(ui, STATUS_READY, "Agent list");
    pthread_mutex_unlock(&ui->mtx);
    free(banner);
    tui_render(ui);

    agent_queue_free(q);
    return CMD_CONTINUE;
}

static int cmd_agents_show(command_ctx_t *ctx, const char *id) {
    ui_state_t *ui = ctx->ui;
    while (*id == ' ') id++;

    agent_queue_t *q = agent_scan(ctx->nash_dir);
    if (!q) {
        pthread_mutex_lock(&ui->mtx);
        ui_state_set_status(ui, STATUS_ERROR, "/agent show: scan failed");
        pthread_mutex_unlock(&ui->mtx);
        tui_render(ui);
        return CMD_CONTINUE;
    }
    agent_queue_load(q, ctx->nash_dir);
    agent_queue_schedule(q, time(NULL));

    const agent_entry_t *found = agent_find(q, id);
    if (!found) {
        pthread_mutex_lock(&ui->mtx);
        ui_state_set_status(ui, STATUS_ERROR, "/agent show: agent not found");
        pthread_mutex_unlock(&ui->mtx);
        tui_render(ui);
        agent_queue_free(q);
        return CMD_CONTINUE;
    }

    str_t display = str_new(2048);
    str_appendf(&display, "# Agent: %s\n\n", found->id);
    str_appendf(&display, "**Workspace**: %s  \n", found->workspace_name);
    str_appendf(&display, "**File**: `%s`  \n", found->agent_file);
    str_appendf(&display, "**Schedule**: `%s`  \n", found->schedule_str);
    str_appendf(&display, "**Timeout**: %ds  \n", found->timeout);
    str_appendf(&display, "**Enabled**: %s  \n", found->enabled ? "yes" : "no");
    str_appendf(&display, "**Due now**: %s  \n\n", found->is_due ? "**yes**" : "no");

    if (found->last_run > 0) {
        char timebuf[64];
        struct tm tm;
        localtime_r(&found->last_run, &tm);
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm);
        char durbuf[32];
        fmt_duration((double)found->last_duration, durbuf, sizeof(durbuf));
        str_appendf(&display,
            "## Last Run\n"
            "**Time**: %s  \n"
            "**Duration**: %s  \n"
            "**Status**: %s  \n\n",
            timebuf, durbuf,
            found->last_status ? found->last_status : "?");
    } else {
        str_appendf(&display, "## Last Run\nNever executed\n\n");
    }

    if (found->next_due > 0 && !found->schedule.is_startup) {
        char timebuf[64];
        struct tm tm;
        localtime_r(&found->next_due, &tm);
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm);
        str_appendf(&display, "**Next due**: %s\n\n", timebuf);
    }

    /* Latest result snippet */
    char result_path[NASH_PATH_MAX];
    snprintf(result_path, sizeof(result_path),
             "%s/agent/results/%s/latest.md", ctx->nash_dir, found->id);
    size_t rlen = 0;
    char *result_content = slurp_file(result_path, &rlen);
    if (result_content) {
        str_appendf(&display, "## Latest Result\n\n");
        if (rlen > 500) {
            result_content[500] = '\0';
            str_appendf(&display, "%s\n\n*...truncated. Use `/agent result %s` for full output.*\n",
                        result_content, found->id);
        } else {
            str_appendf(&display, "%s\n", result_content);
        }
        free(result_content);
    }

    str_appendf(&display, "\n---\n`/agent run %s` to execute now\n", found->id);

    char *banner = str_steal(&display);
    pthread_mutex_lock(&ui->mtx);
    ui_state_push_content(ui, "agent-detail", banner);
    ui_state_set_status(ui, STATUS_READY, "Agent detail");
    pthread_mutex_unlock(&ui->mtx);
    free(banner);
    tui_render(ui);

    agent_queue_free(q);
    return CMD_CONTINUE;
}

static int cmd_agents_run(command_ctx_t *ctx, const char *id) {
    ui_state_t *ui = ctx->ui;
    while (*id == ' ') id++;

    if (*ctx->inferring) {
        pthread_mutex_lock(&ui->mtx);
        ui_state_set_status(ui, STATUS_ERROR,
            "Wait for inference to finish before running an agent");
        pthread_mutex_unlock(&ui->mtx);
        tui_render(ui);
        return CMD_CONTINUE;
    }

    agent_queue_t *q = agent_scan(ctx->nash_dir);
    if (!q) {
        pthread_mutex_lock(&ui->mtx);
        ui_state_set_status(ui, STATUS_ERROR, "/agent run: scan failed");
        pthread_mutex_unlock(&ui->mtx);
        tui_render(ui);
        return CMD_CONTINUE;
    }
    agent_queue_load(q, ctx->nash_dir);

    const agent_entry_t *found = agent_find(q, id);
    if (!found) {
        pthread_mutex_lock(&ui->mtx);
        ui_state_set_status(ui, STATUS_ERROR, "/agent run: agent not found");
        pthread_mutex_unlock(&ui->mtx);
        tui_render(ui);
        agent_queue_free(q);
        return CMD_CONTINUE;
    }

    playbook_t *pb = playbook_load(found->agent_file);
    if (!pb) {
        pthread_mutex_lock(&ui->mtx);
        ui_state_set_status(ui, STATUS_ERROR,
            "/agent run: cannot load agent playbook");
        pthread_mutex_unlock(&ui->mtx);
        tui_render(ui);
        agent_queue_free(q);
        return CMD_CONTINUE;
    }

    /* Inject agent-specific template variables */
    int new_nvars = pb->n_vars + 3;
    pb->var_keys   = realloc(pb->var_keys,   (size_t)new_nvars * sizeof(char *));
    pb->var_values = realloc(pb->var_values,  (size_t)new_nvars * sizeof(char *));
    pb->var_keys[pb->n_vars]       = strdup("workspace_name");
    pb->var_values[pb->n_vars]     = strdup(found->workspace_name);
    pb->var_keys[pb->n_vars + 1]   = strdup("workspace_dir");
    pb->var_values[pb->n_vars + 1] = strdup(found->workspace_dir);
    pb->var_keys[pb->n_vars + 2]   = strdup("agent_id");
    pb->var_values[pb->n_vars + 2] = strdup(found->id);
    pb->n_vars = new_nvars;

    *ctx->pargs = (playbook_args_t){
        .playbook     = pb,
        .nash_dir     = (char *)ctx->nash_dir,
        .store        = ctx->store,
        .memory       = ctx->memory,
        .cfg          = ctx->cfg,
        .provider     = ctx->provider,
        .server_model = (char *)ctx->server_model,
        .ui           = ui,
        .playbook_ok  = 0,
        .done         = 0,
    };

    ctx->provider->abort_retry = 0;
    pthread_create(ctx->infer_tid, NULL, playbook_worker, ctx->pargs);
    *ctx->inferring = 3;

    pthread_mutex_lock(&ui->mtx);
    ui->agent_view = 1;     /* Don't overwrite main session.md during agent run */
    ui->agent_running = 1;   /* Prevent Escape from clearing agent_view mid-run */
    char msg[256];
    snprintf(msg, sizeof(msg), "Running agent: %s", found->id);
    ui_state_set_status(ui, STATUS_RUNNING, msg);
    pthread_mutex_unlock(&ui->mtx);
    tui_render(ui);

    agent_queue_free(q);
    return CMD_CONTINUE;
}

static int cmd_agents_history(command_ctx_t *ctx, const char *filter_id) {
    ui_state_t *ui = ctx->ui;
    if (filter_id) while (*filter_id == ' ') filter_id++;

    char path[NASH_PATH_MAX];
    snprintf(path, sizeof(path), "%s/agent/history.jsonl", ctx->nash_dir);

    FILE *f = fopen(path, "r");
    if (!f) {
        str_t display = str_new(256);
        str_appendf(&display, "# Agent History\n\nNo history yet.\n");
        char *banner = str_steal(&display);
        pthread_mutex_lock(&ui->mtx);
        ui_state_push_content(ui, "agent-history", banner);
        ui_state_set_status(ui, STATUS_READY, "Agent history");
        pthread_mutex_unlock(&ui->mtx);
        free(banner);
        tui_render(ui);
        return CMD_CONTINUE;
    }

    str_t display = str_new(4096);
    if (filter_id && *filter_id)
        str_appendf(&display, "# Agent History: %s\n\n", filter_id);
    else
        str_appendf(&display, "# Agent History\n\n");

    str_appendf(&display,
        "| Time | Agent | Status | Duration |\n"
        "|------|-------|--------|----------|\n");

    char line[4096];
    char *lines[256];
    int n_lines = 0;
    while (fgets(line, sizeof(line), f) && n_lines < 256) {
        if (filter_id && *filter_id) {
            if (!strstr(line, filter_id)) continue;
        }
        lines[n_lines++] = strdup(line);
    }
    fclose(f);

    /* Display newest first (last 50) */
    int start = n_lines > 50 ? n_lines - 50 : 0;
    for (int i = n_lines - 1; i >= start; i--) {
        cJSON *ev = cJSON_Parse(lines[i]);
        if (!ev) { free(lines[i]); continue; }

        const char *aid = cJSON_GetStringValue(cJSON_GetObjectItem(ev, "id"));
        const char *st  = cJSON_GetStringValue(cJSON_GetObjectItem(ev, "st"));
        double ts_d = 0;
        cJSON *ts_item = cJSON_GetObjectItem(ev, "ts");
        if (ts_item) ts_d = cJSON_GetNumberValue(ts_item);
        int dur = 0;
        cJSON *dur_item = cJSON_GetObjectItem(ev, "dur");
        if (dur_item) dur = (int)cJSON_GetNumberValue(dur_item);

        char timebuf[64];
        time_t t = (time_t)ts_d;
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(timebuf, sizeof(timebuf), "%m-%d %H:%M", &tm);

        char durbuf[32];
        fmt_duration((double)dur, durbuf, sizeof(durbuf));

        str_appendf(&display, "| %s | %s | %s %s | %s |\n",
                    timebuf,
                    aid ? aid : "?",
                    (st && strcmp(st, "ok") == 0) ? "ok" : "FAIL",
                    st ? st : "?",
                    durbuf);

        cJSON_Delete(ev);
        free(lines[i]);
    }
    for (int i = 0; i < start; i++) free(lines[i]);

    if (n_lines == 0)
        str_appendf(&display, "\nNo history entries%s\n",
                    (filter_id && *filter_id) ? " for this agent" : "");
    else
        str_appendf(&display, "\nShowing %d of %d entries\n",
                    n_lines - start, n_lines);

    char *banner = str_steal(&display);
    pthread_mutex_lock(&ui->mtx);
    ui_state_push_content(ui, "agent-history", banner);
    ui_state_set_status(ui, STATUS_READY, "Agent history");
    pthread_mutex_unlock(&ui->mtx);
    free(banner);
    tui_render(ui);

    return CMD_CONTINUE;
}

static int cmd_agents_due(command_ctx_t *ctx) {
    ui_state_t *ui = ctx->ui;

    agent_queue_t *q = agent_scan(ctx->nash_dir);
    if (!q) {
        pthread_mutex_lock(&ui->mtx);
        ui_state_set_status(ui, STATUS_ERROR, "/agent due: scan failed");
        pthread_mutex_unlock(&ui->mtx);
        tui_render(ui);
        return CMD_CONTINUE;
    }
    agent_queue_load(q, ctx->nash_dir);
    agent_queue_schedule(q, time(NULL));

    str_t display = str_new(1024);
    str_appendf(&display, "# Agents Due Now\n\n");

    int n = 0;
    for (int i = 0; i < q->n_agents; i++) {
        if (!q->agents[i].is_due) continue;
        n++;
        const agent_entry_t *a = &q->agents[i];
        char since[64] = "never run";
        if (a->last_run > 0) {
            char durbuf[32];
            fmt_duration((double)(time(NULL) - a->last_run), durbuf, sizeof(durbuf));
            snprintf(since, sizeof(since), "%s ago", durbuf);
        }
        str_appendf(&display, "%d. **%s** — schedule: `%s`, last: %s, timeout: %ds\n",
                    n, a->id, a->schedule_str, since, a->timeout);
    }

    if (n == 0)
        str_appendf(&display, "No agents are due right now.\n");
    else
        str_appendf(&display,
            "\n`/agent run ID` to run one, or `nash --agent` from CLI to run all.\n");

    char *banner = str_steal(&display);
    pthread_mutex_lock(&ui->mtx);
    ui_state_push_content(ui, "agents-due", banner);
    ui_state_set_status(ui, STATUS_READY, n > 0 ? "Agents due" : "No agents due");
    pthread_mutex_unlock(&ui->mtx);
    free(banner);
    tui_render(ui);

    agent_queue_free(q);
    return CMD_CONTINUE;
}

static int cmd_agents_result(command_ctx_t *ctx, const char *id) {
    ui_state_t *ui = ctx->ui;
    while (*id == ' ') id++;

    /* Try exact ID first, then resolve via scan */
    char path[NASH_PATH_MAX];
    snprintf(path, sizeof(path),
             "%s/agent/results/%s/latest.md", ctx->nash_dir, id);

    size_t clen = 0;
    char *content = slurp_file(path, &clen);
    if (!content) {
        /* Suffix resolve: scan to find full agent ID */
        agent_queue_t *q = agent_scan(ctx->nash_dir);
        if (q) {
            const agent_entry_t *found = agent_find(q, id);
            if (found) {
                snprintf(path, sizeof(path),
                         "%s/agent/results/%s/latest.md", ctx->nash_dir, found->id);
                content = slurp_file(path, &clen);
            }
            agent_queue_free(q);
        }
    }

    if (!content) {
        pthread_mutex_lock(&ui->mtx);
        ui_state_set_status(ui, STATUS_ERROR,
            "/agent result: no result found (agent never run or ID wrong)");
        pthread_mutex_unlock(&ui->mtx);
        tui_render(ui);
        return CMD_CONTINUE;
    }

    pthread_mutex_lock(&ui->mtx);
    ui_state_push_content(ui, "agent-result", content);
    ui_state_set_status(ui, STATUS_READY, "Agent result");
    pthread_mutex_unlock(&ui->mtx);
    free(content);
    tui_render(ui);

    return CMD_CONTINUE;
}

static int cmd_agents(command_ctx_t *ctx, const char *args) {
    while (*args == ' ') args++;

    /* Default: /agent with no args → list */
    if (*args == '\0' || strcmp(args, "list") == 0) {
        return cmd_agents_list(ctx);
    }
    if (strncmp(args, "show ", 5) == 0) {
        return cmd_agents_show(ctx, args + 5);
    }
    if (strncmp(args, "run ", 4) == 0) {
        return cmd_agents_run(ctx, args + 4);
    }
    if (strcmp(args, "history") == 0) {
        return cmd_agents_history(ctx, NULL);
    }
    if (strncmp(args, "history ", 8) == 0) {
        return cmd_agents_history(ctx, args + 8);
    }
    if (strcmp(args, "due") == 0) {
        return cmd_agents_due(ctx);
    }
    if (strncmp(args, "result ", 7) == 0) {
        return cmd_agents_result(ctx, args + 7);
    }

    ui_state_t *ui = ctx->ui;
    pthread_mutex_lock(&ui->mtx);
    ui_state_set_status(ui, STATUS_ERROR,
        "/agent: unknown subcommand (list|show|run|history|due|result)");
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
    if (strncmp(sq, "/memory_search ", 15) == 0) {
        int rc = cmd_memory_query(ctx, sq + 15);
        free(sq);
        *submitted_query = NULL;
        return rc;
    }
    if (strncmp(sq, "/ms ", 4) == 0) {
        int rc = cmd_memory_query(ctx, sq + 4);
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
    if (strcmp(sq, "/todo") == 0 ||
        strncmp(sq, "/todo ", 6) == 0) {
        int rc = cmd_todo(ctx, strlen(sq) > 5 ? sq + 6 : "");
        free(sq);
        *submitted_query = NULL;
        return rc;
    }
    if (strcmp(sq, "/agent") == 0 ||
        strncmp(sq, "/agent ", 7) == 0) {
        int rc = cmd_agents(ctx, strlen(sq) > 6 ? sq + 7 : "");
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
