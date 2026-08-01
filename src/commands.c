#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

/* Weak definition so test binaries link without main.c */
int g_path_given __attribute__((weak)) = 0;

#include "commands.h"
#include "commands_internal.h"
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

/* ── /name <name> ──────────────────────────────────────────────── */
static int cmd_name(command_ctx_t *ctx, const char *name) {
    ui_state_t *ui = ctx->ui;
    char *session_dir = ctx->session_dir;

    if (strlen(name) == 0 || strlen(name) > 255 ||
        strchr(name, '/') != NULL || strchr(name, '\n') != NULL) {
        pthread_mutex_lock(&ui->mtx);
        ui_state_set_status(ui, STATUS_ERROR,
            "/name: invalid name (no slashes, max 255 chars)");
        pthread_mutex_unlock(&ui->mtx);
        tui_render(ui);
        return CMD_CONTINUE;
    }
    char *sb = sessions_base_dir(ctx->nash_dir, ctx->cfg->workspace);
    char link_path[1088];
    snprintf(link_path, sizeof(link_path), "%s/%s", sb, name);
    free(sb);
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
    /* Enable repo map for subsequent queries (same as CLI PATH arg) */
    extern int g_path_given;
    g_path_given = 1;
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
    playbook_resolve("dream", ctx->nash_dir, pb_path, sizeof(pb_path));
    playbook_t *dream_pb = playbook_load(pb_path);
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
        .ws_memory = ctx->ws ? ctx->ws->workspace : NULL,
        .cfg = ctx->cfg,
        .provider = ctx->provider,
        .consolidation_provider = ctx->consolidation_provider,
        .server_model = (char *)ctx->server_model,
        .ui = ui,
        .playbook_ok = 0,
        .done = 0,
    };
    ctx->provider->abort_retry = 0;  /* reset before new inference */
    pthread_create(ctx->infer_tid, NULL, playbook_worker, ctx->pargs);
    *ctx->inferring = INFER_PLAYBOOK;
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
        .consolidation_provider = ctx->consolidation_provider,
        .server_model = (char *)ctx->server_model,
        .ui = ui,
        .playbook_ok = 0,
        .done = 0,
    };
    ctx->provider->abort_retry = 0;  /* reset before new inference */
    pthread_create(ctx->infer_tid, NULL, playbook_worker, ctx->pargs);
    *ctx->inferring = INFER_PLAYBOOK;
    tui_render(ui);
    return CMD_CONTINUE;
}

/* Callback for jsonl_iterate: format run log events */
static int format_run_event_cb(cJSON *ev, void *user_data) {
    str_t *display = user_data;
    const char *e = cJSON_GetStringValue(cJSON_GetObjectItem(ev, "e"));
    if (!e) return 0;

    if (strcmp(e, "start") == 0) {
        str_appendf(display, "**Playbook**: %s  \n",
            cJSON_GetStringValue(cJSON_GetObjectItem(ev, "pb")));
        str_appendf(display, "**Passes**: %d\n\n",
            (int)cJSON_GetNumberValue(cJSON_GetObjectItem(ev, "n")));
    } else if (strcmp(e, "pass") == 0) {
        int idx = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(ev, "i"));
        const char *label = cJSON_GetStringValue(cJSON_GetObjectItem(ev, "l"));
        const char *sid = cJSON_GetStringValue(cJSON_GetObjectItem(ev, "sid"));
        str_appendf(display, "- **Pass %d**: %s\n  session: `%s`\n",
            idx + 1, label ? label : "?", sid ? sid : "?");
    } else if (strcmp(e, "done") == 0) {
        const char *st = cJSON_GetStringValue(cJSON_GetObjectItem(ev, "st"));
        double ts = cJSON_GetNumberValue(cJSON_GetObjectItem(ev, "ts"));
        str_appendf(display, "  status: %s  (%.0f)\n", st ? st : "?", ts);
    } else if (strcmp(e, "end") == 0) {
        const char *st = cJSON_GetStringValue(cJSON_GetObjectItem(ev, "st"));
        str_appendf(display, "\n**Result**: %s\n", st ? st : "?");
    }
    return 0;
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
        /* Validate run ID to prevent path traversal */
        if (!is_safe_path_component(show_id)) {
            pthread_mutex_lock(&ui->mtx);
            ui_state_set_status(ui, STATUS_ERROR,
                "/runs show: invalid run ID");
            pthread_mutex_unlock(&ui->mtx);
            tui_render(ui);
            return CMD_CONTINUE;
        }
        /* /runs show <id> — display a specific run log */
        char rpath[NASH_PATH_MAX + NASH_PATH_MAX];
        /* Try exact filename, or append .jsonl */
        if (strstr(show_id, ".jsonl"))
            snprintf(rpath, sizeof(rpath), "%s/%s", rdir, show_id);
        else
            snprintf(rpath, sizeof(rpath), "%s/%s.jsonl", rdir, show_id);

        str_t display = str_new(2048);
        str_appendf(&display, "# Run Log: %s\n\n", show_id);
        if (jsonl_iterate(rpath, format_run_event_cb, &display) < 0) {
            str_free(&display);
            pthread_mutex_lock(&ui->mtx);
            ui_state_set_status(ui, STATUS_ERROR,
                "/runs show: run log not found");
            pthread_mutex_unlock(&ui->mtx);
            tui_render(ui);
            return CMD_CONTINUE;
        }

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
                        VEC_PUSH(names, nnames, names_cap, strdup(ent->d_name));
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
        if (ctx->session_dir) {
            snprintf(sessions_dir, sizeof(sessions_dir), "%s", ctx->session_dir);
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
    if (ctx->session_dir) {
        char md_path[NASH_PATH_MAX];
        snprintf(md_path, sizeof(md_path), "%s/search.md", ctx->session_dir);
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

/* ── Main dispatch ─────────────────────────────────────────────── */
int command_dispatch(command_ctx_t *ctx, char **submitted_query) {
    char *sq = *submitted_query;

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
        /* /? with text -> memory/session search; bare /? -> clear UI search */
        const char *after = sq + 2;
        while (*after == ' ') after++;
        if (*after) {
            int rc = cmd_memory_query(ctx, after);
            free(sq);
            *submitted_query = NULL;
            return rc;
        }
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
    if (strcmp(sq, "/tool") == 0 ||
        strncmp(sq, "/tool ", 6) == 0) {
        int rc = cmd_tool(ctx, strlen(sq) > 5 ? sq + 6 : "");
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
        char *orig = checkpoint_read_query(ctx->session_dir);
        if (orig) {
            free(sq);
            *submitted_query = orig;
        }
        /* Fall through — let caller dispatch as regular query */
        return CMD_NOT_FOUND;
    }

    return CMD_NOT_FOUND;
}
