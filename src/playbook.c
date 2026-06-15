/*
 * Playbook system for nash — multi-pass agentic workflows.
 *
 * A playbook is a YAML file defining a sequence of react_run() passes
 * with shared scratchpad, configurable react loop flags, and template
 * variable expansion. Dream is the first built-in playbook.
 */

#include "playbook.h"
#include "yaml_parse.h"
#include "nash_limits.h"
#include "nash_log.h"
#include "str.h"
#include "frontend_tui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

/* Forward declaration — defined in main.c.\n * Weak symbol so tests can link without main.o. */
char *create_session_dir(const char *nash_dir) __attribute__((weak));

/* ── YAML → Playbook parsing ────────────────────────── */

static void parse_react_overrides(yaml_node_t *react_node, pb_react_overrides_t *ro) {
    if (!react_node) return;

    yaml_node_t *n;
    if ((n = yaml_get(react_node, "max_steps")))
        ro->max_steps = yaml_int(n, 0);
    if ((n = yaml_get(react_node, "inject_memory")))
        ro->inject_memory = yaml_bool(n, -1);
    if ((n = yaml_get(react_node, "inject_prev_result")))
        ro->inject_prev_result = yaml_bool(n, -1);
    if ((n = yaml_get(react_node, "enable_reflection")))
        ro->enable_reflection = yaml_bool(n, -1);
    if ((n = yaml_get(react_node, "enable_pruning")))
        ro->enable_pruning = yaml_bool(n, -1);
    if ((n = yaml_get(react_node, "enable_compaction")))
        ro->enable_compaction = yaml_bool(n, -1);
    if ((n = yaml_get(react_node, "enable_scoring")))
        ro->enable_scoring = yaml_bool(n, -1);

    /* Tool filter */
    yaml_node_t *tools = yaml_get(react_node, "tools");
    if (tools) {
        yaml_node_t *allow = yaml_get(tools, "allow");
        if (allow && allow->type == YAML_SEQUENCE) {
            ro->n_tools_allow = yaml_len(allow);
            ro->tools_allow = calloc(ro->n_tools_allow, sizeof(char *));
            for (int i = 0; i < ro->n_tools_allow; i++) {
                const char *s = yaml_str(yaml_item(allow, i));
                ro->tools_allow[i] = s ? strdup(s) : strdup("");
            }
        }
        yaml_node_t *block = yaml_get(tools, "block");
        if (block && block->type == YAML_SEQUENCE) {
            ro->n_tools_block = yaml_len(block);
            ro->tools_block = calloc(ro->n_tools_block, sizeof(char *));
            for (int i = 0; i < ro->n_tools_block; i++) {
                const char *s = yaml_str(yaml_item(block, i));
                ro->tools_block[i] = s ? strdup(s) : strdup("");
            }
        }
    }
}

playbook_t *playbook_load(const char *path) {
    yaml_node_t *root = yaml_parse_file(path);
    if (!root) return NULL;

    playbook_t *pb = calloc(1, sizeof(playbook_t));
    if (!pb) { yaml_free(root); return NULL; }

    pb->filepath = strdup(path);

    /* Initialize react defaults to inherit */
    pb->react_defaults = (pb_react_overrides_t)PB_REACT_INHERIT;

    /* Top-level fields */
    const char *s;
    if ((s = yaml_str(yaml_get(root, "name"))))
        pb->name = strdup(s);
    else
        pb->name = strdup("unnamed");

    if ((s = yaml_str(yaml_get(root, "description"))))
        pb->description = strdup(s);

    /* Session mode */
    s = yaml_str(yaml_get(root, "session_mode"));
    if (s && strcmp(s, "shared") == 0)
        pb->session_mode = PB_SESSION_SHARED;
    else
        pb->session_mode = PB_SESSION_PER_PASS;

    /* Scratchpad mode */
    s = yaml_str(yaml_get(root, "scratchpad_mode"));
    if (s && strcmp(s, "isolated") == 0)
        pb->scratch_mode = PB_SCRATCH_ISOLATED;
    else
        pb->scratch_mode = PB_SCRATCH_SHARED;

    /* Pause between */
    pb->pause_between = yaml_bool(yaml_get(root, "pause_between"), 0);

    /* Template variables */
    yaml_node_t *vars = yaml_get(root, "vars");
    if (vars && vars->type == YAML_MAPPING) {
        pb->n_vars = vars->n_children;
        pb->var_keys = calloc(pb->n_vars, sizeof(char *));
        pb->var_values = calloc(pb->n_vars, sizeof(char *));
        for (int i = 0; i < pb->n_vars; i++) {
            pb->var_keys[i] = strdup(vars->keys[i]);
            const char *v = yaml_str(vars->values[i]);
            pb->var_values[i] = v ? strdup(v) : strdup("");
        }
    }

    /* Post hooks */
    yaml_node_t *post = yaml_get(root, "post");
    if (post) {
        pb->post_prune = yaml_bool(yaml_get(post, "prune_memory"), 0);
        pb->post_commit = yaml_bool(yaml_get(post, "commit"), 0);
    }

    /* React defaults */
    parse_react_overrides(yaml_get(root, "react"), &pb->react_defaults);

    /* Passes */
    yaml_node_t *passes = yaml_get(root, "passes");
    if (passes && passes->type == YAML_SEQUENCE) {
        pb->n_passes = yaml_len(passes);
        pb->passes = calloc(pb->n_passes, sizeof(pb_pass_t));
        for (int i = 0; i < pb->n_passes; i++) {
            yaml_node_t *pass = yaml_item(passes, i);
            if (!pass) continue;

            pb->passes[i].react = (pb_react_overrides_t)PB_REACT_INHERIT;

            const char *label = yaml_str(yaml_get(pass, "label"));
            pb->passes[i].label = label ? strdup(label) : strdup("(unnamed)");

            const char *prompt = yaml_str(yaml_get(pass, "prompt"));
            pb->passes[i].prompt_template = prompt ? strdup(prompt) : strdup("");

            /* Per-pass react overrides */
            parse_react_overrides(yaml_get(pass, "react"), &pb->passes[i].react);
        }
    }

    yaml_free(root);
    return pb;
}

static void free_react_overrides(pb_react_overrides_t *ro) {
    for (int i = 0; i < ro->n_tools_allow; i++) free(ro->tools_allow[i]);
    free(ro->tools_allow);
    for (int i = 0; i < ro->n_tools_block; i++) free(ro->tools_block[i]);
    free(ro->tools_block);
}

void playbook_free(playbook_t *pb) {
    if (!pb) return;
    free(pb->name);
    free(pb->description);
    free(pb->filepath);
    for (int i = 0; i < pb->n_vars; i++) {
        free(pb->var_keys[i]);
        free(pb->var_values[i]);
    }
    free(pb->var_keys);
    free(pb->var_values);
    free_react_overrides(&pb->react_defaults);
    for (int i = 0; i < pb->n_passes; i++) {
        free(pb->passes[i].label);
        free(pb->passes[i].prompt_template);
        free_react_overrides(&pb->passes[i].react);
    }
    free(pb->passes);
    free(pb);
}

/* ── Flag resolution (3-level cascade) ───────────────── */

react_flags_t playbook_resolve_flags(const playbook_t *pb, int pass_idx,
                                      const config_t *cfg) {
    const pb_react_overrides_t *pass = &pb->passes[pass_idx].react;
    const pb_react_overrides_t *def = &pb->react_defaults;

    /* 3-level cascade: pass override → playbook default → profile/global.
     * Profile values of -1 mean "not set" (inherit compile-time default of 1).
     * cfg may be NULL in test contexts — fall back to 1. */
    #define PROFILE_OR_1(field) \
        (cfg && cfg->field >= 0 ? cfg->field : 1)

    #define RESOLVE(field, global_default) \
        (pass->field != -1 ? pass->field : \
         (def->field != -1 ? def->field : global_default))

    react_flags_t f;
    f.inject_memory       = RESOLVE(inject_memory, PROFILE_OR_1(profile_inject_memory));
    f.inject_prev_result  = RESOLVE(inject_prev_result, PROFILE_OR_1(profile_inject_prev_result));
    f.enable_reflection   = RESOLVE(enable_reflection, PROFILE_OR_1(profile_enable_reflection));
    f.enable_pruning      = RESOLVE(enable_pruning, PROFILE_OR_1(profile_enable_pruning));
    f.enable_compaction   = RESOLVE(enable_compaction, PROFILE_OR_1(profile_enable_compaction));
    f.enable_scoring      = RESOLVE(enable_scoring, PROFILE_OR_1(profile_enable_scoring));

    #undef RESOLVE
    #undef PROFILE_OR_1
    return f;
}

tool_filter_t playbook_resolve_tools(const playbook_t *pb, int pass_idx,
                                      const config_t *cfg) {
    tool_filter_t tf = {0};
    const pb_react_overrides_t *pass = &pb->passes[pass_idx].react;
    const pb_react_overrides_t *def = &pb->react_defaults;

    /* 3-level cascade: pass → playbook default → profile.
     * Pass-level overrides take highest precedence. */
    if (pass->tools_allow) {
        tf.allowed = (const char **)pass->tools_allow;
        tf.n_allowed = pass->n_tools_allow;
    } else if (def->tools_allow) {
        tf.allowed = (const char **)def->tools_allow;
        tf.n_allowed = def->n_tools_allow;
    } else if (cfg && cfg->n_profile_tools_allow > 0) {
        tf.allowed = (const char **)cfg->profile_tools_allow;
        tf.n_allowed = cfg->n_profile_tools_allow;
    }

    if (pass->tools_block) {
        tf.blocked = (const char **)pass->tools_block;
        tf.n_blocked = pass->n_tools_block;
    } else if (def->tools_block) {
        tf.blocked = (const char **)def->tools_block;
        tf.n_blocked = def->n_tools_block;
    } else if (cfg && cfg->n_profile_tools_block > 0) {
        tf.blocked = (const char **)cfg->profile_tools_block;
        tf.n_blocked = cfg->n_profile_tools_block;
    }

    /* Inherit profile description overrides (Gap #7).
     * Playbook passes don't define their own desc overrides,
     * so always inherit from profile if available. */
    if (cfg && cfg->n_profile_tool_descs > 0) {
        tf.desc_names = cfg->profile_tool_desc_names;
        tf.desc_values = cfg->profile_tool_desc_values;
        tf.n_descs = cfg->n_profile_tool_descs;
    }

    return tf;
}

/* ── Template expansion ──────────────────────────────── */

/* Replace all occurrences of {{key}} with value in a string */
static char *str_replace_all(const char *src, const char *key, const char *val) {
    if (!src || !key || !val) return src ? strdup(src) : NULL;

    char pattern[256];
    snprintf(pattern, sizeof(pattern), "{{%s}}", key);
    int plen = (int)strlen(pattern);
    int vlen = (int)strlen(val);

    /* Count occurrences */
    int count = 0;
    const char *p = src;
    while ((p = strstr(p, pattern)) != NULL) { count++; p += plen; }

    if (count == 0) return strdup(src);

    int slen = (int)strlen(src);
    int new_len = slen + count * (vlen - plen);
    char *result = malloc(new_len + 1);
    char *dst = result;
    p = src;
    while (*p) {
        if (strncmp(p, pattern, plen) == 0) {
            memcpy(dst, val, vlen);
            dst += vlen;
            p += plen;
        } else {
            *dst++ = *p++;
        }
    }
    *dst = '\0';
    return result;
}

char *playbook_expand(const playbook_t *pb, const char *tmpl,
                      int pass_idx, const char *prev_result,
                      const char *memory_dir, const char *model,
                      const char *session_dir, const char *nash_dir) {
    if (!tmpl) return strdup("");

    char *result = strdup(tmpl);

    /* Built-in variables */
    char pass_num[16], total_passes[16];
    snprintf(pass_num, sizeof(pass_num), "%d", pass_idx + 1);
    snprintf(total_passes, sizeof(total_passes), "%d", pb->n_passes);

        char datebuf[32];
    time_t now = time(NULL);
    struct tm *utc = gmtime(&now);
    strftime(datebuf, sizeof(datebuf), "%Y-%m-%d", utc);

    char cwdbuf[NASH_PATH_MAX];
    if (!getcwd(cwdbuf, sizeof(cwdbuf)))
        snprintf(cwdbuf, sizeof(cwdbuf), ".");

    /* Change 6: Load previous scratchpad from state dir */
    char *prev_scratch_text = NULL;
    if (nash_dir && pb->name) {
        char sp_path[NASH_PATH_MAX];
        snprintf(sp_path, sizeof(sp_path), "%s/playbooks/.state/%s/scratchpad.md",
                 nash_dir, pb->name);
        prev_scratch_text = slurp_file(sp_path, NULL);
    }

    struct { const char *key; const char *val; } builtins[] = {
        {"memory_dir",      memory_dir ? memory_dir : ""},
        {"model",           model ? model : "unknown"},
        {"session_dir",     session_dir ? session_dir : ""},
        {"nash_dir",        nash_dir ? nash_dir : ""},
        {"cwd",             cwdbuf},
        {"date",            datebuf},
        {"pass_number",     pass_num},
        {"total_passes",    total_passes},
        {"prev_result",     prev_result ? prev_result : ""},
        {"prev_scratchpad", prev_scratch_text ? prev_scratch_text : ""},
        {NULL, NULL},
    };

    for (int i = 0; builtins[i].key; i++) {
        char *next = str_replace_all(result, builtins[i].key, builtins[i].val);
        free(result);
        result = next;
    }

    /* User-defined variables */
    for (int i = 0; i < pb->n_vars; i++) {
        char *next = str_replace_all(result, pb->var_keys[i], pb->var_values[i]);
        free(result);
        result = next;
    }

    free(prev_scratch_text);
    return result;
}

/* ── Playbook listing ────────────────────────────────── */

playbook_t **playbook_list(const char *nash_dir, int *count) {
    *count = 0;
    char pb_dir[NASH_PATH_MAX];
    snprintf(pb_dir, sizeof(pb_dir), "%s/playbooks", nash_dir);

    DIR *d = opendir(pb_dir);
    if (!d) return NULL;

    playbook_t **list = NULL;
    int cap = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        int nlen = (int)strlen(ent->d_name);
        if (nlen < 5 || strcmp(ent->d_name + nlen - 5, ".yaml") != 0)
            continue;

        char path[NASH_PATH_MAX + 256];
        snprintf(path, sizeof(path), "%s/%s", pb_dir, ent->d_name);
        playbook_t *pb = playbook_load(path);
        if (!pb) continue;

        if (*count >= cap) {
            cap = cap ? cap * 2 : 8;
            list = realloc(list, cap * sizeof(playbook_t *));
        }
        list[(*count)++] = pb;
    }
    closedir(d);
    return list;
}

/* ── Default dream.yaml (embedded from playbooks/dream.yaml at build time) ── */

#include "dream_yaml.inc"

int playbook_write_default_dream(const char *path) {
    /* Create parent directory if needed */
    char dir[NASH_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        mkdir(dir, 0755);
    }

    return write_file(path, (const char *)playbooks_dream_yaml, playbooks_dream_yaml_len);
}

/* ── Worker thread ───────────────────────────────────── */

/* Event callback for playbook passes — routes to TUI */
typedef struct {
    ui_state_t  *ui;
    const char  *pass_dir;    /* session directory for this pass */
    int          react_loop;  /* react loop number within the pass session */
    int          pass_index;  /* 0-based pass index */
    const char  *pass_label;  /* pass label string */
} pb_event_ctx_t;

static void pb_event_cb(const react_event_t *ev, void *userdata) {
    pb_event_ctx_t *ctx = (pb_event_ctx_t *)userdata;
    if (!ctx || !ctx->ui) {
        /* Headless mode: print events to stderr */
        tui_on_event(ev, NULL);
        return;
    }
    /* Shallow-copy and enrich with provenance */
    react_event_t enriched = *ev;
    enriched.session_dir = ctx->pass_dir;
    enriched.react_loop  = ctx->react_loop;
    enriched.pass_index  = ctx->pass_index;
    enriched.pass_label  = ctx->pass_label;
    pthread_mutex_lock(&ctx->ui->mtx);
    ui_state_on_event(&enriched, (void *)ctx->ui);
    pthread_mutex_unlock(&ctx->ui->mtx);
}

void *playbook_worker(void *arg) {
    playbook_args_t *pa = arg;
    playbook_t *pb = pa->playbook;
    pb_event_ctx_t ev_ctx = {
        .ui = pa->ui,
        .pass_dir = NULL,
        .react_loop = 0,
        .pass_index = 0,
        .pass_label = NULL,
    };

    /* Clear any previous playbook pass tracking on the UI */
    if (pa->ui) {
        pthread_mutex_lock(&pa->ui->mtx);
        for (int i = 0; i < pa->ui->pb_pass_count; i++) {
            free(pa->ui->pb_passes[i].session_dir);
            free(pa->ui->pb_passes[i].pass_label);
        }
        pa->ui->pb_pass_count = 0;
        pthread_mutex_unlock(&pa->ui->mtx);
    }

    scratchpad_t shared_scratch;
    scratchpad_init(&shared_scratch);

    /* Change 5: Load persisted scratchpad from previous runs */
    char state_dir[NASH_PATH_MAX];
    snprintf(state_dir, sizeof(state_dir), "%s/playbooks/.state/%s",
             pa->nash_dir, pb->name);
    scratchpad_load(&shared_scratch, state_dir);

    char *prev_result = NULL;
    char *shared_session_dir = NULL;

    if (pb->session_mode == PB_SESSION_SHARED) {
        shared_session_dir = create_session_dir(pa->nash_dir);
    }

    int playbook_ok = 1;

    /* Suppress inline consolidation and defer embeddings during dream.
     * Dream IS the consolidation — triggering inline consolidation on
     * each memory_store during dream passes would:
     *   (a) fire redundant LLM calls for entries being merged in the same pass
     *   (b) generate embeddings that are immediately invalidated by the next merge
     * The consolidating flag is checked by tool_memory_store() (skips
     * memory_try_consolidate) and memory_store() (skips memory_embed_entry).
     * Embeddings are regenerated in bulk via memory_embed_all() after
     * all passes complete (below). */
    int suppress_consolidation = (pa->memory && pb->name &&
                                   strcmp(pb->name, "dream") == 0);
    if (suppress_consolidation)
        atomic_store(&pa->memory->consolidating, 1);

    /* ── Run log: append-only JSONL tracking orchestration ── */
    char runs_dir[NASH_PATH_MAX];
    snprintf(runs_dir, sizeof(runs_dir), "%s/runs", pa->nash_dir);
    mkdir(runs_dir, 0755);

    struct timespec run_tp;
    clock_gettime(CLOCK_REALTIME, &run_tp);
    char run_path[NASH_PATH_MAX + 64];
    snprintf(run_path, sizeof(run_path), "%s/%ld.%05ld.jsonl",
             runs_dir, (long)run_tp.tv_sec, run_tp.tv_nsec / 10000);

    FILE *run_log = fopen(run_path, "a");
    if (run_log) {
        fprintf(run_log,
            "{\"e\":\"start\",\"pb\":\"%s\",\"ts\":%ld.%05ld,\"n\":%d}\n",
            pb->name, (long)run_tp.tv_sec, run_tp.tv_nsec / 10000,
            pb->n_passes);
        fflush(run_log);
    }

    for (int pass = 0; pass < pb->n_passes; pass++) {
        pa->current_pass = pass;

        /* Update TUI status */
        char status[256];
        snprintf(status, sizeof(status), "%s %d/%d: %s",
                 pb->name, pass + 1, pb->n_passes,
                 pb->passes[pass].label);
        if (pa->ui) {
            pthread_mutex_lock(&pa->ui->mtx);
            ui_state_set_status(pa->ui, STATUS_RUNNING, status);
            pthread_mutex_unlock(&pa->ui->mtx);
        } else {
            fprintf(stderr, "[play] %s\n", status);
        }

        /* Inter-pass pause */
        if (pass > 0 && pb->pause_between) {
            pa->waiting_for_user = 1;
            pa->inter_pass_message = pb->passes[pass].label;
            while (pa->waiting_for_user) {
                struct timespec ts = {0, 50000000};
                nanosleep(&ts, NULL);
            }
        }

        /* Expand template */
        const char *mdir = pa->memory ? pa->memory->dir : "";
        const char *model = pa->server_model ? pa->server_model : "unknown";
        char *prompt = playbook_expand(pb, pb->passes[pass].prompt_template,
                                       pass, prev_result,
                                       mdir, model,
                                       NULL, pa->nash_dir);

        /* Create session */
        char *pass_dir;
        if (pb->session_mode == PB_SESSION_PER_PASS) {
            pass_dir = create_session_dir(pa->nash_dir);
        } else {
            pass_dir = strdup(shared_session_dir);
        }

        /* Run log: emit pass start */
        if (run_log) {
            const char *sid = strrchr(pass_dir, '/');
            sid = sid ? sid + 1 : pass_dir;
            struct timespec pass_tp;
            clock_gettime(CLOCK_REALTIME, &pass_tp);
            fprintf(run_log,
                "{\"e\":\"pass\",\"i\":%d,\"l\":\"%s\",\"sid\":\"%s\",\"ts\":%ld.%05ld}\n",
                pass, pb->passes[pass].label, sid,
                (long)pass_tp.tv_sec, pass_tp.tv_nsec / 10000);
            fflush(run_log);
        }

        /* Setup per-pass tool_ctx */
        journal_t *pass_journal = journal_new(pass_dir);
        tool_filter_t tf = playbook_resolve_tools(pb, pass, pa->cfg);
        /* Change 4: correct react_loop numbering for shared sessions */
        int pass_react_loop = 0;
        if (pb->session_mode == PB_SESSION_SHARED) {
            int max_rl = journal_max_react_loop(pass_journal);
            pass_react_loop = (max_rl >= 0) ? max_rl + 1 : 0;
        }

        tool_ctx_t pass_tools = {
            .store = pa->store,
            .journal = pass_journal,
            .memory = pa->memory,
            .session_dir = pass_dir,
            .cfg = pa->cfg,
            .provider = pa->provider,
            .react_loop = pass_react_loop,
            .aliases = alias_map_new(),
            .tool_filter = tf,
        };

        /* Transfer shared scratchpad */
        if (pb->scratch_mode == PB_SCRATCH_SHARED) {
            scratchpad_move(&pass_tools.scratch, &shared_scratch);
        } else {
            scratchpad_init(&pass_tools.scratch);
        }

        /* Resolve react flags for this pass */
        react_flags_t flags = playbook_resolve_flags(pb, pass, pa->cfg);
        int max_steps = pb->passes[pass].react.max_steps > 0
                        ? pb->passes[pass].react.max_steps
                        : (pb->react_defaults.max_steps > 0
                           ? pb->react_defaults.max_steps
                           : pa->cfg->max_react_steps);

        react_ctx_t pass_react = {
            .provider = pa->provider,
            .tools = &pass_tools,
            .max_steps = max_steps,
            .verbose = 1,
            .flags = flags,
        };

        /* Change 2: Update event context with pass provenance */
        ev_ctx.pass_dir   = pass_dir;
        ev_ctx.react_loop = pass_react_loop;
        ev_ctx.pass_index = pass;
        ev_ctx.pass_label = pb->passes[pass].label;

        /* Run the pass */
        char *result = react_run(&pass_react, prompt, pb_event_cb, &ev_ctx);

        /* Harvest scratchpad */
        if (pb->scratch_mode == PB_SCRATCH_SHARED) {
            scratchpad_move(&shared_scratch, &pass_tools.scratch);
        }

        int pass_failed = (result == NULL);

        /* Run log: emit pass done */
        if (run_log) {
            struct timespec done_tp;
            clock_gettime(CLOCK_REALTIME, &done_tp);
            fprintf(run_log,
                "{\"e\":\"done\",\"i\":%d,\"st\":\"%s\",\"ts\":%ld.%05ld}\n",
                pass, pass_failed ? "fail" : "ok",
                (long)done_tp.tv_sec, done_tp.tv_nsec / 10000);
            fflush(run_log);
        }

        /* Cleanup */
        free(prev_result);
        prev_result = result;
        free(prompt);

        /* Free section-based scratchpad.
         * For PB_SCRATCH_SHARED: already moved back to shared_scratch
         * (struct zeroed by scratchpad_move), so this is a no-op.
         * For PB_SCRATCH_ISOLATED: sections must be freed here. */
        scratchpad_free(&pass_tools.scratch);
        alias_map_free(pass_tools.aliases);
        journal_free(pass_journal);
        free(pass_dir);

        if (pass_failed) {
            playbook_ok = 0;
            break;
        }
    }

    /* Run log: emit end */
    if (run_log) {
        struct timespec end_tp;
        clock_gettime(CLOCK_REALTIME, &end_tp);
        fprintf(run_log,
            "{\"e\":\"end\",\"st\":\"%s\",\"ts\":%ld.%05ld}\n",
            playbook_ok ? "ok" : "fail",
            (long)end_tp.tv_sec, end_tp.tv_nsec / 10000);
        fclose(run_log);
    }

    /* Restore consolidation guard and regenerate embeddings.
     * Inline consolidation was suppressed during dream passes to avoid
     * redundant LLM calls and throwaway embedding generation.  Now that
     * all merges are complete, regenerate embeddings for entries that
     * were stored without them (memory_embed_all is idempotent — skips
     * entries that already have up-to-date .emb files). */
    if (suppress_consolidation) {
        atomic_store(&pa->memory->consolidating, 0);
        memory_embed_all(pa->memory);
    }

    /* Post-playbook hooks */
    if (pb->post_prune && pa->memory) {
        memory_prune(pa->memory, pa->cfg->prune_min_score,
                     pa->cfg->prune_min_evidence);
    }

    /* Clean up ephemeral fact: entries created during playbook execution.
     * Playbook passes (especially dream) may store working state as
     * fact:* entries (inventory reports, merge plans, logs). These are
     * intermediate artifacts, not reusable knowledge, and pollute the
     * memory store. Delete any fact: entries created after the run started. */
    if (pa->memory && pa->memory->dir) {
        double run_ts = (double)run_tp.tv_sec +
                        (double)run_tp.tv_nsec / 1e9;
        DIR *mdir = opendir(pa->memory->dir);
        if (mdir) {
            /* Phase 1: collect keys to delete (can't delete while iterating) */
            char **del_keys = NULL;
            int n_del = 0, del_cap = 0;
            struct dirent *de;
            while ((de = readdir(mdir)) != NULL) {
                /* Match fact_*.json files */
                if (strncmp(de->d_name, "fact_", 5) != 0) continue;
                size_t nlen = strlen(de->d_name);
                if (nlen < 6 || strcmp(de->d_name + nlen - 5, ".json") != 0)
                    continue;

                /* Read created_at from the JSON file */
                char fpath[NASH_PATH_MAX];
                snprintf(fpath, sizeof(fpath), "%s/%s",
                         pa->memory->dir, de->d_name);
                FILE *fp = fopen(fpath, "r");
                if (!fp) continue;
                char buf[8192];
                size_t rd = fread(buf, 1, sizeof(buf) - 1, fp);
                fclose(fp);
                buf[rd] = '\0';

                /* Quick parse: find "key" and "created_at" values.
                 * Read key from JSON directly (avoids filename→key mapping bugs). */
                const char *ca = strstr(buf, "\"created_at\"");
                if (!ca) continue;
                ca = strchr(ca + 12, ':');
                if (!ca) continue;
                ca++;
                while (*ca == ' ' || *ca == '"') ca++;
                double entry_ts = strtod(ca, NULL);
                if (entry_ts < run_ts) continue;

                /* Extract key from the "key" field in JSON */
                const char *kp = strstr(buf, "\"key\"");
                if (!kp) continue;
                kp = strchr(kp + 4, ':');
                if (!kp) continue;
                kp++;
                while (*kp == ' ') kp++;
                if (*kp != '"') continue;
                kp++;  /* skip opening quote */
                const char *ke = strchr(kp, '"');
                if (!ke || ke - kp >= 256) continue;
                char key[256];
                snprintf(key, sizeof(key), "%.*s", (int)(ke - kp), kp);
                /* Verify it starts with "fact:" */
                if (strncmp(key, "fact:", 5) != 0) continue;

                if (n_del >= del_cap) {
                    del_cap = del_cap ? del_cap * 2 : 8;
                    del_keys = realloc(del_keys, sizeof(char *) * (size_t)del_cap);
                }
                del_keys[n_del++] = strdup(key);
            }
            closedir(mdir);

            /* Phase 2: batch delete collected keys.
             * Uses memory_delete_batch() for a single gc_refs pass
             * and single git commit instead of N individual deletes. */
            if (n_del > 0) {
                memory_delete_batch(pa->memory,
                                    (const char **)del_keys, n_del);
            }
            for (int i = 0; i < n_del; i++)
                free(del_keys[i]);
            free(del_keys);
        }
    }

    /* Touch .last_dream timestamp file after successful dream run.
     * The dream reminder (main.c) counts entries created since this file's
     * mtime.  Without this touch, the reminder counter never resets. */
    if (playbook_ok && pa->memory && pa->memory->dir &&
        pb->name && strcmp(pb->name, "dream") == 0) {
        char dream_ts[NASH_PATH_MAX];
        snprintf(dream_ts, sizeof(dream_ts), "%s/.last_dream",
                 pa->memory->dir);
        FILE *fp = fopen(dream_ts, "w");
        if (fp) fclose(fp);  /* creates or updates mtime */
    }

    /* Change 5: Persist scratchpad for next run */
    if (pb->scratch_mode == PB_SCRATCH_SHARED) {
        /* Create state directory (mkdir -p equivalent) */
        char state_parent[NASH_PATH_MAX];
        snprintf(state_parent, sizeof(state_parent), "%s/playbooks/.state",
                 pa->nash_dir);
        mkdir(state_parent, 0755);
        mkdir(state_dir, 0755);
        scratchpad_save(&shared_scratch, state_dir);
    }

    /* Change 3: Clear playbook session dir on UI when done */
    if (pa->ui) {
        pthread_mutex_lock(&pa->ui->mtx);
        free(pa->ui->playbook_session_dir);
        pa->ui->playbook_session_dir = NULL;
        /* Regenerate session.md so completed passes show final ✓ status */
        ui_state_generate_session_md(pa->ui);
        pthread_mutex_unlock(&pa->ui->mtx);
    }

    free(prev_result);
    free(shared_session_dir);
    scratchpad_free(&shared_scratch);
    pa->playbook_ok = playbook_ok;
    pa->done = 1;
    return NULL;
}
