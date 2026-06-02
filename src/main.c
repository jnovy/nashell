#include <stdio.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include <errno.h>
#include <readline/readline.h>
#include <readline/history.h>

#include "config.h"
#include "llm.h"
#include "provider.h"
#include "tools.h"
#include "react.h"
#include "store.h"
#include "journal.h"
#include "frontend_tui.h"
#include "nash_limits.h"
#include "memory.h"
#include "ui_state.h"
#include "tui.h"
#include "nash_log.h"
#include "str.h"
#include "playbook.h"

/* Load a legacy scratchpad.md file. Returns malloc'd string or NULL.
 * Caps at 32KB to prevent memory explosion. */
static char *load_legacy_scratchpad(const char *session_dir) {
    char sp_path[NASH_PATH_MAX];
    snprintf(sp_path, sizeof(sp_path), "%s/scratchpad.md", session_dir);
    char *buf = slurp_file(sp_path, NULL);
    if (!buf || strlen(buf) >= NASH_INITIAL_BUF) {
        free(buf);
        return NULL;
    }
    return buf;
}

/* Get the nash data directory: ~/.nash/ or config override */
static char *get_nash_dir(const config_t *cfg) {
    if (cfg->data_dir && cfg->data_dir[0]) {
        mkdir(cfg->data_dir, 0755);
        return strdup(cfg->data_dir);
    }
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    char path[512];
    snprintf(path, sizeof(path), "%s/.nash", home);
    mkdir(path, 0755);
    return strdup(path);
}

/* Create session directory: <nash_dir>/sessions/<epoch.NNNNN>/ */
char *create_session_dir(const char *nash_dir) {
    struct timespec tp;
    clock_gettime(CLOCK_REALTIME, &tp);

    char sessions_base[1024];
    snprintf(sessions_base, sizeof(sessions_base), "%s/sessions", nash_dir);
    mkdir(sessions_base, 0755);

    char path[1088];  /* sessions_base (1024) + "/" + epoch.nanos (~30) */
    snprintf(path, sizeof(path), "%s/%ld.%05ld",
             sessions_base, (long)tp.tv_sec, tp.tv_nsec / 10000);
    mkdir(path, 0755);
    return strdup(path);
}

/* Recursive mkdir: create all path components (like mkdir -p).
 * Returns 0 on success, -1 on failure (errno set). */
static int mkdir_p(const char *path, mode_t mode) {
    char tmp[4096];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp)) { errno = ENAMETOOLONG; return -1; }
    memcpy(tmp, path, len + 1);
    /* Strip trailing slash */
    if (tmp[len - 1] == '/') tmp[--len] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
    return 0;
}

/* Check if a directory is empty (no files other than . and ..).
 * Returns 1 if empty, 0 if not empty or on error. */
static int is_dir_empty(const char *path) {
    DIR *d = opendir(path);
    if (!d) return 0;
    struct dirent *ent;
    int empty = 1;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0) {
            empty = 0;
            break;
        }
    }
    closedir(d);
    return empty;
}

/* Helper: get JSON string or default */
static const char *jstr(cJSON *obj, const char *key, const char *def) {
    cJSON *item = cJSON_GetObjectItem(obj, key);
    return (item && cJSON_IsString(item)) ? item->valuestring : def;
}
static double jnum(cJSON *obj, const char *key, double def) {
    cJSON *item = cJSON_GetObjectItem(obj, key);
    return (item && cJSON_IsNumber(item)) ? cJSON_GetNumberValue(item) : def;
}
static int jbool(cJSON *obj, const char *key, int def) {
    cJSON *item = cJSON_GetObjectItem(obj, key);
    return item ? cJSON_IsTrue(item) : def;
}

/* Print banner: header art + server props + client overrides */
static void print_banner(const config_t *cfg, const char *props_json,
                         const char *nash_dir) {
    /* ASCII art header with gradient green ANSI colors */
    printf("\n");
    printf("  \033[1m\033[38;2;80;255;120m _  _    __   ____  _  _  ____  __    __   \033[0m\n");
    printf("  \033[1m\033[38;2;60;220;100m( \\| |  / _\\ / ___\\/ )/ \\(  __)(  )  (  )  \033[0m\n");
    printf("  \033[1m\033[38;2;40;190;80m ) \\  |/    \\\\___ \\) __ ( ) _) / (_/\\/ (_/\\ \033[0m\n");
    printf("  \033[1m\033[38;2;30;160;60m(___)_)\\_/\\_/(____/\\_)\\_/(____\\\\____/\\____/ \033[0m\n");
    printf("\n");
    printf("  \033[38;2;70;200;90m--------- * New Agentic Shell * ---------\033[0m\n");
    printf("\n");

    /* 1. Provider / server info */
    const char *ptype = cfg->provider.type;
    int is_api = ptype && (strcmp(ptype, "vertex") == 0 ||
                           strcmp(ptype, "anthropic") == 0 ||
                           strcmp(ptype, "openai") == 0);

    if (is_api) {
        /* API provider: show provider type, model, and relevant details */
        printf("provider: %s\n", ptype);
        printf("  model:    %s\n",
               cfg->provider.model_id ? cfg->provider.model_id : "(not set)");
        if (cfg->provider.project_id)
            printf("  project:  %s\n", cfg->provider.project_id);
        if (cfg->provider.region)
            printf("  region:   %s\n", cfg->provider.region);
        if (cfg->provider.context_size > 0)
            printf("  ctx:      %d tok (%dk)\n",
                   cfg->provider.context_size,
                   cfg->provider.context_size / 1024);
        else
            printf("  ctx:      unknown\n");
        printf("\n");
    } else if (!props_json) {
        printf("server: %s (props unavailable)\n\n",
               cfg->api_base ? cfg->api_base : "(none)");
    } else {
        cJSON *props = cJSON_Parse(props_json);
        if (!props) {
            printf("server: %s (props parse error)\n\n",
                   cfg->api_base ? cfg->api_base : "(none)");
        } else {
            cJSON *gs = cJSON_GetObjectItem(props, "default_generation_settings");
            cJSON *params = gs ? cJSON_GetObjectItem(gs, "params") : NULL;
            cJSON *caps = cJSON_GetObjectItem(props, "chat_template_caps");
            cJSON *mods = cJSON_GetObjectItem(props, "modalities");
            int n_ctx = (int)jnum(gs, "n_ctx", 0);

            printf("server: %s\n", cfg->api_base ? cfg->api_base : "(none)");
            printf("  model:    %s\n", jstr(props, "model_alias", "(unknown)"));
            printf("  build:    %s\n", jstr(props, "build_info", "?"));
            printf("  ctx:      %d tok (%dk)", n_ctx, n_ctx / 1024);
            printf(" | slots: %d\n", (int)jnum(props, "total_slots", 0));

            if (params) {
                printf("  defaults: temp=%.1f top_k=%d top_p=%.2f min_p=%.2f",
                       jnum(params, "temperature", 0),
                       (int)jnum(params, "top_k", 0),
                       jnum(params, "top_p", 0),
                       jnum(params, "min_p", 0));
                double rp = jnum(params, "repeat_penalty", 1.0);
                if (rp != 1.0) printf(" rep=%.1f", rp);
                printf("\n");
            }

            printf("  caps:     tools=%s vision=%s reasoning=%s\n",
                   caps && jbool(caps, "supports_tools", 0) ? "yes" : "no",
                   mods && jbool(mods, "vision", 0) ? "yes" : "no",
                   params ? jstr(params, "reasoning_format", "none") : "?");

            printf("\n");
            cJSON_Delete(props);
        }
    }

    /* 2. Client config */
    const char *think_str;
    if (cfg->thinking.mode == THINKING_ON)
        think_str = "yes";
    else if (cfg->thinking.mode == THINKING_EDRM)
        think_str = is_api ? "off (edrm n/a)" : "edrm";
    else
        think_str = "no";
    printf("client: temp=%.1f max_tokens=%d thinking=%s stream=%s\n",
           cfg->temperature, cfg->max_tokens,
           think_str,
           cfg->stream ? "on" : "off");
    printf("data:   %s\n", nash_dir);
    char cwd_buf[NASH_PATH_MAX];
    if (getcwd(cwd_buf, sizeof(cwd_buf)))
        printf("cwd:    %s\n", cwd_buf);
    printf("\n");
}

/* Build banner as a string for ncurses TUI (no ANSI escapes) */
static char *build_banner_string(const config_t *cfg, const char *props_json,
                                  const char *nash_dir, const char *session_dir) {
    str_t s = str_new(2048);

    str_append_cstr(&s, "\n");
    str_append_cstr(&s, "   _   _   __   ____  _  _  ____  __    __   \n");
    str_append_cstr(&s, "  ( \\ | | / _\\ / ___\\/ )/ \\(  __)(  )  (  )  \n");
    str_append_cstr(&s, "   ) \\  |/    \\\\___ \\) __ ( ) _) / (_/\\/ (_/\\ \n");
    str_append_cstr(&s, "  (___)_)\\_/\\_/(____/\\_)\\_/(____\\\\____/\\____/ \n");
    str_append_cstr(&s, "\n");
    str_append_cstr(&s, "  --------- * New Agentic Shell * ---------\n");
    str_append_cstr(&s, "\n");

    const char *bptype = cfg->provider.type;
    int bis_api = bptype && (strcmp(bptype, "vertex") == 0 ||
                             strcmp(bptype, "anthropic") == 0 ||
                             strcmp(bptype, "openai") == 0);

    if (bis_api) {
        str_appendf(&s, "provider: %s\n", bptype);
        str_appendf(&s, "  model:    %s\n",
                    cfg->provider.model_id ? cfg->provider.model_id : "(not set)");
        if (cfg->provider.project_id)
            str_appendf(&s, "  project:  %s\n", cfg->provider.project_id);
        if (cfg->provider.region)
            str_appendf(&s, "  region:   %s\n", cfg->provider.region);
        if (cfg->provider.context_size > 0)
            str_appendf(&s, "  ctx:      %d tok (%dk)\n",
                        cfg->provider.context_size,
                        cfg->provider.context_size / 1024);
        else
            str_append_cstr(&s, "  ctx:      unknown\n");
        str_append_cstr(&s, "\n");
    } else if (!props_json) {
        str_appendf(&s, "server: %s (props unavailable)\n\n",
                    cfg->api_base ? cfg->api_base : "(none)");
    } else {
        cJSON *props = cJSON_Parse(props_json);
        if (!props) {
            str_appendf(&s, "server: %s (props parse error)\n\n",
                        cfg->api_base ? cfg->api_base : "(none)");
        } else {
            cJSON *gs = cJSON_GetObjectItem(props, "default_generation_settings");
            cJSON *params = gs ? cJSON_GetObjectItem(gs, "params") : NULL;
            cJSON *caps = cJSON_GetObjectItem(props, "chat_template_caps");
            cJSON *mods = cJSON_GetObjectItem(props, "modalities");
            int n_ctx = (int)jnum(gs, "n_ctx", 0);

            str_appendf(&s, "server: %s\n", cfg->api_base ? cfg->api_base : "(none)");
            str_appendf(&s, "  model:    %s\n", jstr(props, "model_alias", "(unknown)"));
            str_appendf(&s, "  build:    %s\n", jstr(props, "build_info", "?"));
            str_appendf(&s, "  ctx:      %d tok (%dk) | slots: %d\n",
                        n_ctx, n_ctx / 1024, (int)jnum(props, "total_slots", 0));

            if (params) {
                str_appendf(&s, "  defaults: temp=%.1f top_k=%d top_p=%.2f min_p=%.2f\n",
                            jnum(params, "temperature", 0),
                            (int)jnum(params, "top_k", 0),
                            jnum(params, "top_p", 0),
                            jnum(params, "min_p", 0));
            }

            str_appendf(&s, "  caps:     tools=%s vision=%s reasoning=%s\n",
                        caps && jbool(caps, "supports_tools", 0) ? "yes" : "no",
                        mods && jbool(mods, "vision", 0) ? "yes" : "no",
                        params ? jstr(params, "reasoning_format", "none") : "?");
            str_append_cstr(&s, "\n");
            cJSON_Delete(props);
        }
    }

    const char *ts;
    if (cfg->thinking.mode == THINKING_ON)
        ts = "yes";
    else if (cfg->thinking.mode == THINKING_EDRM)
        ts = bis_api ? "off (edrm n/a)" : "edrm";
    else
        ts = "no";
    str_appendf(&s, "client: temp=%.1f max_tokens=%d thinking=%s stream=%s\n",
                cfg->temperature, cfg->max_tokens,
                ts,
                cfg->stream ? "on" : "off");
    str_appendf(&s, "data:   %s\n", nash_dir);
    char cwd_buf[NASH_PATH_MAX];
    if (getcwd(cwd_buf, sizeof(cwd_buf)))
        str_appendf(&s, "cwd:    %s\n", cwd_buf);
    if (session_dir)
        str_appendf(&s, "\n[session: %s]\n", session_dir);

    return str_steal(&s);
}


/* --- Threading for non-blocking inference --- */
typedef struct {
    react_ctx_t *react;
    char        *query;
    ui_state_t  *ui;
    char        *result;
    atomic_int done;
} infer_args_t;

/* --- Threading for non-blocking /dream --- */
typedef struct {
    /* Inputs (set by main thread before pthread_create) */
    char            *nash_dir;
    store_t         *store;
    memory_t        *memory;
    config_t        *cfg;
    llm_config_t    *llm;
    provider_t      *provider;
    char            *server_model;
    ui_state_t      *ui;
    /* Output */
    int              dream_ok;
    atomic_int       done;
} dream_args_t;

/* Generic event callback for background threads — routes react events to TUI.
 * The userdata must point to a struct whose first member is unused (or any struct)
 * but which has a .ui field accessible. We use infer_args_t for regular queries
 * and dream_event_ctx_t for dream passes. */
typedef struct {
    ui_state_t *ui;
} dream_event_ctx_t;

static void threaded_event_cb(const react_event_t *ev, void *userdata) {
    infer_args_t *a = (infer_args_t *)userdata;
    pthread_mutex_lock(&a->ui->mtx);
    ui_state_on_event(ev, (void *)a->ui);
    pthread_mutex_unlock(&a->ui->mtx);
}

static void dream_event_cb(const react_event_t *ev, void *userdata) {
    dream_event_ctx_t *ctx = (dream_event_ctx_t *)userdata;
    pthread_mutex_lock(&ctx->ui->mtx);
    ui_state_on_event(ev, (void *)ctx->ui);
    pthread_mutex_unlock(&ctx->ui->mtx);
}

static void *infer_worker(void *arg) {
    infer_args_t *a = (infer_args_t *)arg;
    a->result = react_run(a->react, a->query, threaded_event_cb, a);
    a->done = 1;
    return NULL;
}

static void *dream_worker(void *arg) {
    dream_args_t *da = (dream_args_t *)arg;
    dream_event_ctx_t ev_ctx = { .ui = da->ui };

    scratchpad_t shared_scratch;
    scratchpad_init(&shared_scratch);

    const int n_dream_passes = 4;
    int dream_ok = 1;

    for (int pass = 0; pass < n_dream_passes; pass++) {
        const char *pass_labels[] = {
            "Dream 1/4: Inventory & scan",
            "Dream 2/4: Merge duplicates",
            "Dream 3/4: Resolve contradictions",
            "Dream 4/4: Synthesize & cross-link",
        };
        pthread_mutex_lock(&da->ui->mtx);
        ui_state_set_status(da->ui, STATUS_RUNNING, pass_labels[pass]);
        pthread_mutex_unlock(&da->ui->mtx);

        /* Fresh session per pass */
        char *pass_dir = create_session_dir(da->nash_dir);
        journal_t *pass_journal = journal_new(pass_dir);

        llm_config_t llm_cfg_copy = *da->llm;
        tool_ctx_t pass_tools = {
            .store = da->store,
            .journal = pass_journal,
            .memory = da->memory,
            .session_dir = pass_dir,
            .scratchpad = NULL,
            .cfg = da->cfg, .llm = &llm_cfg_copy, .provider = da->provider,
            .react_loop = 0,
            .aliases = alias_map_new(),
        };
        scratchpad_move(&pass_tools.scratch, &shared_scratch);

        react_flags_t dream_flags = REACT_FLAGS_BARE;
        react_ctx_t pass_react = {
            .provider = da->provider,
            .llm = &llm_cfg_copy,
            .tools = &pass_tools,
            .max_steps = da->cfg->max_react_steps,
            .verbose = 1,
            .flags = dream_flags,
        };

        /* Build per-pass prompt */
        char dream_prompt[8192];
        const char *mdir = da->memory->dir;
        const char *model = da->server_model ? da->server_model : "unknown-model";

        switch (pass) {
        case 0:
            snprintf(dream_prompt, sizeof(dream_prompt),
                "You are performing MEMORY INVENTORY for a persistent knowledge store.\n\n"
                "GOAL: Read and catalog ALL memories at: %s\n"
                "Each memory is a JSON file with fields: key, value, pinned, "
                "created_at, last_accessed, access_count, recall_hits, recall_misses, journal_ref.\n"
                "Validation score = (recall_hits+1)/(recall_hits+recall_misses+2) -- Beta posterior mean.\n\n"
                "TASK: List all memory files, read each one, and produce a structured report:\n"
                "1. Total memory count\n"
                "2. Groups of near-duplicate entries (same concept, different wording) -- list their keys\n"
                "3. Pairs of contradictory entries -- list their keys and the contradiction\n"
                "4. Task-specific entries that could be generalized -- list their keys\n"
                "5. Low-evidence entries (access_count=0, score=0.50) -- list their keys\n\n"
                "DO NOT make any changes. Only read and report.\n"
                "Store your full report in the scratchpad (notes tool) so the next pass can use it.\n"
                "Call done with a summary of what you found.",
                mdir);
            break;

        case 1:
            snprintf(dream_prompt, sizeof(dream_prompt),
                "You are performing DEDUPLICATION for a persistent knowledge store at: %s\n\n"
                "Read the scratchpad -- it contains an inventory from the previous pass listing "
                "near-duplicate memory groups.\n\n"
                "CAPABILITIES:\n"
                "- Read any memory: file_read on the JSON file path\n"
                "- Create/update memories: memory_store (preserves validation scores on update)\n"
                "- Delete memories: memory_delete with the key\n\n"
                "TASK: For each group of near-duplicates identified in the scratchpad:\n"
                "1. Read all entries in the group\n"
                "2. Create ONE merged entry combining the best content from all\n"
                "3. Preserve recall_hits/recall_misses from the highest-scored source\n"
                "4. Delete the redundant entries\n\n"
                "CONSTRAINTS:\n"
                "- Never touch pinned memories\n"
                "- Keep high-scoring entries (score > 0.7) -- merge INTO them, don't delete them\n"
                "- Be conservative -- only merge entries that truly cover the same concept\n\n"
                "Update the scratchpad with what you merged.\n"
                "Call done with a list of merges performed (old keys -> new key).",
                mdir);
            break;

        case 2:
            snprintf(dream_prompt, sizeof(dream_prompt),
                "You are performing CONTRADICTION RESOLUTION for a persistent knowledge store at: %s\n\n"
                "Read the scratchpad -- it contains an inventory identifying contradictory memory pairs, "
                "plus a log of merges already performed in the previous pass.\n\n"
                "CAPABILITIES:\n"
                "- Read any memory: file_read on the JSON file path\n"
                "- Read original context: file_read on journal_ref path\n"
                "- Read git history: shell_exec \"git -C %s log --oneline\"\n"
                "- Create/update memories: memory_store\n"
                "- Delete memories: memory_delete with the key\n\n"
                "TASK: For each contradictory pair:\n"
                "1. Read both entries fully\n"
                "2. If journal_ref exists, read the original context to understand WHY each was created\n"
                "3. Keep the one with higher validation score, or reconcile into a single entry\n"
                "4. Delete the superseded entry\n\n"
                "CONSTRAINTS:\n"
                "- Never touch pinned memories\n"
                "- If both have high scores, reconcile rather than delete\n"
                "- Check if previous pass already merged/deleted any of these entries\n\n"
                "Update the scratchpad with resolutions.\n"
                "Call done with a list of contradictions resolved.",
                mdir, mdir);
            break;

        case 3:
            snprintf(dream_prompt, sizeof(dream_prompt),
                "You are performing KNOWLEDGE SYNTHESIS for a persistent knowledge store at: %s\n\n"
                "Read the scratchpad -- it contains the full history of previous passes "
                "(inventory, merges, contradiction resolutions).\n\n"
                "CAPABILITIES:\n"
                "- List memories: shell_exec \"ls %s/\"\n"
                "- Read any memory: file_read on the JSON file path\n"
                "- Create/update memories: memory_store\n\n"
                "TASK (three parts):\n\n"
                "A. GENERALIZE: Find clusters of task-specific lessons that share a common pattern. "
                "Create a new 'strategy:' entry that captures the general principle. "
                "Do NOT delete the source lessons.\n\n"
                "B. CROSS-LINK via refs: Establish inter-memory relationships using the 'refs' "
                "parameter of memory_store. For each memory that relates to others, call "
                "memory_store with refs=[\"key1\", \"key2\", ...] listing the related memory keys. "
                "This creates 'see also' links between memories.\n"
                "Examples of valid refs relationships:\n"
                "  - A strategy refs the lessons it was generalized from\n"
                "  - A lesson refs other lessons about the same topic\n"
                "  - An anti-pattern refs the lesson that discovered it\n"
                "  - A skill refs strategies that inform its approach\n"
                "When creating new strategy: entries in part A, ALWAYS include refs to the "
                "source lesson keys.\n\n"
                "CONSTRAINTS:\n"
                "- Only create strategies when 3+ lessons share a clear pattern\n"
                "- Don't create strategies that already exist\n"
                "- Be conservative with refs -- only add genuinely useful connections\n"
                "- refs should be EXISTING memory keys (verify they exist before adding)\n\n"
                "WHEN DONE:\n"
                "- Compose a FULL summary of ALL changes across ALL 4 passes "
                "(read the scratchpad for passes 1-3)\n"
                "- Run: shell_exec \"cd %s && git add -A && git commit -m "
                "'Dream consolidation (4-pass)\\n\\n<your full summary here>"
                "\\n\\nConsolidated-by: %s'\"\n"
                "- Call done with the SAME summary text",
                mdir, mdir, mdir, model);
            break;
        }

        /* Run this pass */
        char *pass_result = react_run(&pass_react, dream_prompt,
                                      dream_event_cb, &ev_ctx);

        /* Harvest the scratchpad back from this pass for the next one */
        scratchpad_move(&shared_scratch, &pass_tools.scratch);

        int pass_failed = (pass_result == NULL);

        /* Cleanup per-pass resources */
        free(pass_result);
        if (pass_tools.scratchpad) free(pass_tools.scratchpad);
        alias_map_free(pass_tools.aliases);
        journal_free(pass_journal);
        free(pass_dir);

        if (pass_failed) {
            dream_ok = 0;
            break;
        }
    }

    scratchpad_free(&shared_scratch);
    da->dream_ok = dream_ok;

    /* Post-dream: prune with fresh validation scores */
    memory_prune(da->memory, da->cfg->prune_min_score, da->cfg->prune_min_evidence);

    da->done = 1;
    return NULL;
}

int main(int argc, char **argv) {
    /* Load config from ~/.nash/config.toml (or default) */
    char config_path[512];
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(config_path, sizeof(config_path), "%s/.nash/config.toml", home);

    config_t *cfg = config_load(config_path);

    /* CLI flags override config */
    const char *query = NULL;
    const char *session_dir_arg = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--api") == 0 && i + 1 < argc) {
            free(cfg->api_base);
            cfg->api_base = strdup(argv[++i]);
            /* --api forces local provider mode — override any [provider]
             * section in config.toml. The user is pointing to a specific
             * llama.cpp/OpenAI-compatible server, not a cloud API. */
            free(cfg->provider.type);
            cfg->provider.type = strdup("local");
        } else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--query") == 0) && i + 1 < argc) {
            query = argv[++i];
        } else if (strcmp(argv[i], "--data-dir") == 0 && i + 1 < argc) {
            free(cfg->data_dir);
            cfg->data_dir = strdup(argv[++i]);
        } else if (strcmp(argv[i], "--session") == 0 && i + 1 < argc) {
            session_dir_arg = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: nash [--api URL] [-p QUERY] [--data-dir PATH] [--session DIR]\n");
            printf("  --session DIR   Open existing session directory\n");
            printf("  --api URL       LLM server URL (default: %s)\n", cfg->api_base);
            printf("  -p QUERY        Run single query and exit (headless mode)\n");
            printf("  --data-dir PATH Data directory (default: ~/.nash/)\n");
            printf("  --session DIR   Open existing session directory\n");
            printf("\nConfig: %s\n", config_path);
            config_free(cfg);
            return 0;
        }
    }

    /* Initialize data directory */
    char *nash_dir = get_nash_dir(cfg);

    /* Write default config if it doesn't exist */
    config_write_default(config_path);

    /* ── Create provider from config ── */
    provider_config_t pcfg = {
        .type           = provider_type_from_str(cfg->provider.type),
        .model_id       = cfg->provider.model_id,
        .api_base       = cfg->api_base,
        .api_key_env    = cfg->provider.api_key_env,
        .project_id     = cfg->provider.project_id,
        .region         = cfg->provider.region,
        .context_size   = cfg->provider.context_size,
        .chars_per_token = cfg->provider.chars_per_token,
        .caching        = cfg->provider.caching,
        .max_tokens     = cfg->max_tokens,
        .temperature    = cfg->temperature,
        .enable_thinking = 0,
        .thinking_budget = -1,
    };
    provider_t *provider = provider_create(&pcfg);

    /* Fetch model info (local server: /props + /v1/models) */
    int context_size = 0;
    char *server_model = NULL;
    char *props_json = NULL;

    if (provider && provider->fetch_model_info) {
        provider->fetch_model_info(provider, &context_size, &server_model, &props_json);
    } else if (pcfg.type == PROVIDER_LOCAL) {
        /* Fallback for local without provider vtable */
        context_size = llm_fetch_context_size(cfg->api_base);
        server_model = llm_fetch_model_name(cfg->api_base);
        props_json = llm_fetch_props_json(cfg->api_base);
    } else {
        /* API providers: use config values, with sensible defaults */
        context_size = cfg->provider.context_size;
        server_model = cfg->provider.model_id ? strdup(cfg->provider.model_id) : NULL;

        /* BUG FIX: Cloud providers (Vertex, Anthropic, OpenAI) have no /props
         * endpoint to auto-detect context_size. Without a default, context_size
         * stays 0 and context eviction never triggers — causing unbounded
         * context growth until the API rejects with HTTP 400. */
        if (context_size == 0) {
            if (pcfg.type == PROVIDER_VERTEX || pcfg.type == PROVIDER_ANTHROPIC)
                context_size = 200000;  /* Claude models: 200K tokens */
            else if (pcfg.type == PROVIDER_OPENAI)
                context_size = 128000;  /* GPT-4o/4.1: 128K tokens */
        }
    }

    /* Update provider and config with fetched context size */
    if (context_size > 0) {
        if (provider) provider->cfg.context_size = context_size;
        if (cfg->provider.context_size == 0)
            cfg->provider.context_size = context_size;
    }
    if (provider && server_model) {
        free((char *)provider->cfg.model_id);  /* free the copy made by provider_create */
        provider->cfg.model_id = strdup(server_model);  /* replace with server-reported model */
    }

    /* Build LLM config (kept for backward compat: EDRM probe, etc.) */
    llm_config_t llm_cfg = {
        .api_base     = cfg->api_base,
        .model        = server_model,
        .max_tokens   = cfg->max_tokens,
        .temperature  = cfg->temperature,
        .context_size = context_size,
    };

    /* Print banner */
    print_banner(cfg, props_json, nash_dir);

    /* Shared store + memory */
    store_t *shared_store = store_new(nash_dir);
    memory_t *memory = memory_new(nash_dir);
    if (server_model)
        memory->model = strdup(server_model);
    /* P0: Set recall score threshold from config for abstention gate */
    memory->recall_min_score = cfg->recall_min_score;

    /* Prune stale memories at startup (90 days, access_count < 2) */
    int pruned = memory_prune(memory,
                              cfg->prune_min_score, cfg->prune_min_evidence);
    if (pruned > 0)
        fprintf(stderr, "[info] pruned %d stale memories\n", pruned);

    /* Initialize semantic embeddings for memory matching (if configured) */
    if (cfg->embedding.type && strcmp(cfg->embedding.type, "none") != 0) {
        memory_init_embeddings(memory, cfg->embedding.type,
                               cfg->embedding.model, cfg->embedding.api_base,
                               cfg->embedding.model_path,
                               cfg->embedding.dimension,
                               cfg->embedding.max_input_chars);
    }

    /* One-shot headless mode */
    if (query) {
        /* Detect existing session: --session arg, or CWD with journal.jsonl */
        char *session_dir = NULL;
        int lazy_session = 0;
        if (session_dir_arg) {
            char jpath[4112];
            snprintf(jpath, sizeof(jpath), "%s/journal.jsonl", session_dir_arg);
            if (access(jpath, F_OK) == 0) {
                session_dir = strdup(session_dir_arg);
            } else {
                fprintf(stderr, "[warn] %s has no journal.jsonl, creating new session\n", session_dir_arg);
            }
        }
        /* NOTE: Do NOT auto-detect session from CWD in headless mode.
         * When a parent nash spawns a child via shell_exec("nash -p ..."),
         * the child inherits the parent's CWD (which IS a session dir with
         * journal.jsonl), causing the child to hijack the parent's session:
         * loading its checkpoint, writing to its journal, and corrupting
         * the parent's state.  Only explicit --session should be honored. */
        journal_t *journal;
        if (!session_dir) {
            /* Lazy session: directory created on first journal_append */
            journal = journal_new_lazy(nash_dir);
            lazy_session = 1;
        } else {
            journal = journal_new(session_dir);
        }
        int start_loop = journal_max_react_loop(journal) + 1;
        tool_ctx_t tools = {
            .store = shared_store, .journal = journal,
            .memory = memory,
            .session_dir = NULL, .scratchpad = NULL,
            .cfg = cfg, .llm = &llm_cfg, .provider = provider,
            .react_loop = start_loop,
            .aliases = alias_map_new(),
        };
        scratchpad_init(&tools.scratch);
        /* Load scratchpad from previous session if it exists (session_dir may be NULL for lazy sessions) */
        if (session_dir) {
            if (scratchpad_load(&tools.scratch, session_dir) == 0 && tools.scratch.count > 0) {
                /* Section-based scratchpad loaded — generate legacy string */
                tools.scratchpad = scratchpad_serialize(&tools.scratch);
            } else {
                /* Try legacy format */
                tools.scratchpad = load_legacy_scratchpad(session_dir);
            }
        }
        react_flags_t default_flags = REACT_FLAGS_DEFAULT;
        react_ctx_t react = {
            .provider = provider, .llm = &llm_cfg, .tools = &tools,
            .max_steps = cfg->max_react_steps, .verbose = 1,
            .flags = default_flags,
        };
        char *result = react_run(&react, query, tui_on_event, NULL);
        /* Resolve session_dir from journal for lazy sessions.
         * journal_session_dir returns internal pointer — must strdup
         * because journal_free() will free the original. */
        if (lazy_session) {
            const char *jsd = journal_session_dir(journal);
            session_dir = jsd ? strdup(jsd) : NULL;
        }
        /* Tier 1 dreaming: deterministic Bayesian pruning after every react loop */
        memory_prune(memory, cfg->prune_min_score, cfg->prune_min_evidence);
        int have_result = (result != NULL);
        if (result) { printf("%s\n", result); free(result); }
        /* Save scratchpad if session was created */
        if (session_dir && tools.scratch.count > 0) {
            char sp_path[NASH_PATH_MAX];
            snprintf(sp_path, sizeof(sp_path), "%s/scratchpad.md", session_dir);
            scratchpad_save(&tools.scratch, sp_path);
        }
        tools.react_loop++;  /* increment for next query */
        if (tools.scratchpad) free(tools.scratchpad);
        scratchpad_free(&tools.scratch);
        alias_map_free(tools.aliases);
        journal_free(journal);
        /* Remove session directory if it's empty (no work was done) */
        if (session_dir && is_dir_empty(session_dir)) {
            rmdir(session_dir);
        }
        if (session_dir) free(session_dir);
        store_free(shared_store);
        memory_free(memory);
        provider_free(provider);
        free(nash_dir);
        free(props_json);
        free(server_model);
        config_free(cfg);
        return have_result ? 0 : 1;
    }

    /* Interactive TUI mode — ONE session for ALL queries */
    {
        /* Detect existing session: --session arg, or CWD with journal.jsonl */
        char *session_dir = NULL;
        if (session_dir_arg) {
            char jpath[4112];
            snprintf(jpath, sizeof(jpath), "%s/journal.jsonl", session_dir_arg);
            if (access(jpath, F_OK) == 0) {
                session_dir = strdup(session_dir_arg);
            } else {
                fprintf(stderr, "[warn] %s has no journal.jsonl, creating new session\n", session_dir_arg);
            }
        }
        if (!session_dir) {
            char cwd[NASH_PATH_MAX];
            if (getcwd(cwd, sizeof(cwd))) {
                char jpath[4112];
                snprintf(jpath, sizeof(jpath), "%s/journal.jsonl", cwd);
                if (access(jpath, F_OK) == 0) {
                    session_dir = strdup(cwd);
                }
            }
        }

        /* Interactive TUI needs session_dir immediately for journal display,
         * so always create it eagerly (lazy sessions break TUI rendering). */
        if (!session_dir) {
            session_dir = create_session_dir(nash_dir);
        }
        journal_t *journal = journal_new(session_dir);
        int start_loop = journal_max_react_loop(journal) + 1;
        tool_ctx_t tools = {
            .store = shared_store, .journal = journal,
            .memory = memory,
            .session_dir = session_dir, .scratchpad = NULL,
            .cfg = cfg, .llm = &llm_cfg, .provider = provider,
            .react_loop = start_loop,
            .aliases = alias_map_new(),
        };
        scratchpad_init(&tools.scratch);
        /* Load scratchpad from previous session if it exists */
        if (scratchpad_load(&tools.scratch, session_dir) == 0 && tools.scratch.count > 0) {
            tools.scratchpad = scratchpad_serialize(&tools.scratch);
        } else {
            tools.scratchpad = load_legacy_scratchpad(session_dir);
        }
        react_flags_t tui_default_flags = REACT_FLAGS_DEFAULT;
        react_ctx_t react = {
            .provider = provider, .llm = &llm_cfg, .tools = &tools,
            .max_steps = cfg->max_react_steps, .verbose = 1,
            .flags = tui_default_flags,
        };

        /* Initialize logging subsystem for TUI error routing */
        nash_log_init(journal, shared_store);

        /* Create UI state and initialize TUI */
        ui_state_t *ui = ui_state_new(session_dir, shared_store);
        /* Pass model name + context info for nashell-style status bar */
        if (server_model)
            ui->model_name = strdup(server_model);
        ui->context_size = context_size;
        ui->context_used = 0;
        ui->pause_flag = &react.pause_requested;  /* ESC → pause react loop */
        ui->bg_jobs = 0;
        ui_state_set_status(ui, STATUS_READY, "Ready");
        /* Set banner text for main pane */
        char *banner = build_banner_string(cfg, props_json, nash_dir, session_dir);
        ui_state_set_banner(ui, banner);
        free(banner);

        /* Initialize TUI BEFORE loading journal, so visible_rows is set
         * correctly for autoscroll calculations in rebuild_md(). */
        tui_init();
        nash_log_set_ui(ui);  /* enable TUI error routing */
        ui->visible_rows = LINES - 4;  /* terminal height minus chrome (top/bottom bars) */

        /* Load existing journal entries into UI state */
        ui_state_load_journal(ui, journal);

        tui_render(ui);

        /* Wrapper event callback: updates ViewModel + redraws TUI */
        /* We use a struct to pass both ui and tui context */

        /* Main TUI event loop */
        int running = 1;
        int inferring = 0;
        pthread_t infer_tid;
        static infer_args_t iargs;
        static dream_args_t dargs;
        static playbook_args_t pargs_tui;
        while (running) {
            /* Check if playbook thread completed */
            if (inferring == 3 && pargs_tui.done) {
                pthread_join(infer_tid, NULL);
                pthread_mutex_lock(&ui->mtx);
                if (pargs_tui.playbook_ok) {
                    char done_msg[256];
                    snprintf(done_msg, sizeof(done_msg), "Playbook '%s' complete (%d passes)",
                             pargs_tui.playbook->name, pargs_tui.playbook->n_passes);
                    ui_state_set_status(ui, STATUS_DONE, done_msg);
                } else {
                    ui_state_set_status(ui, STATUS_ERROR, "Playbook failed");
                }
                pthread_mutex_unlock(&ui->mtx);
                playbook_free(pargs_tui.playbook);
                pargs_tui.playbook = NULL;
                inferring = 0;
                tui_render(ui);
            }
            /* Check if dream thread completed */
            if (inferring == 2 && dargs.done) {
                pthread_join(infer_tid, NULL);
                pthread_mutex_lock(&ui->mtx);
                if (dargs.dream_ok) {
                    ui_state_set_status(ui, STATUS_DONE, "Dream complete (4 passes)");
                } else {
                    ui_state_set_status(ui, STATUS_ERROR, "Dream failed");
                }
                pthread_mutex_unlock(&ui->mtx);
                inferring = 0;
                tui_render(ui);
            }
            /* Check if inference thread completed */
            if (inferring == 1 && iargs.done) {
                pthread_join(infer_tid, NULL);
                inferring = 0;
                tools.react_loop++;  /* increment for next query */
                /* Tier 1 dreaming: deterministic Bayesian pruning after every react loop */
                memory_prune(memory, cfg->prune_min_score, cfg->prune_min_evidence);
                char *result = iargs.result;
                pthread_mutex_lock(&ui->mtx);
                if (result) {
                    ui_state_set_status(ui, STATUS_DONE, "Done");
                    /* Refresh journal view to show completed query */
                    ui_state_load_journal(ui, journal);
                } else if (react.pause_requested) {
                    /* User pressed Space — paused with checkpoint saved (toggle) */
                    react.pause_requested = 0;  /* reset for next run */
                    react.paused = 1;
                    ui_state_set_status(ui, STATUS_READY,
                        "Paused (Space to resume, type query to redirect)");
                } else {
                    ui_state_set_status(ui, STATUS_ERROR, "No result");
                }
                pthread_mutex_unlock(&ui->mtx);
                tui_render(ui);
                if (react.last_query) free(react.last_query);
                if (react.last_result) free(react.last_result);
                react.last_query = strdup(iargs.query);
                react.last_result = result ? strdup(result) : NULL;
                free(result);
                free(iargs.query);
                iargs.query = NULL;
                pthread_mutex_lock(&ui->mtx);
                ui_state_load_journal(ui, journal);
                ui_state_set_status(ui, STATUS_READY, "Ready");
                pthread_mutex_unlock(&ui->mtx);
                tui_render(ui);
            }
            char *submitted_query = NULL;
            int rc = tui_input(ui, &submitted_query);

            if (rc == -1) {
                /* Quit requested */
                running = 0;
                break;
            }

            if (submitted_query) {
                /* Check if inference thread is waiting for user_ask answer */
                if (inferring && react.user_ask_pending) {
                    /* Pass the user's answer to the waiting react loop */
                    free(react.user_ask_answer);
                    react.user_ask_answer = submitted_query;
                    submitted_query = NULL;  /* ownership transferred */
                    react.user_ask_pending = 0;  /* unblock the react loop */
                    pthread_mutex_lock(&ui->mtx);
                    ui_state_set_status(ui, STATUS_RUNNING, "Running...");
                    pthread_mutex_unlock(&ui->mtx);
                    tui_render(ui);
                    continue;
                }

                /* Handle exit/quit commands */
                if (strcmp(submitted_query, "quit") == 0 ||
                    strcmp(submitted_query, "exit") == 0 ||
                    strcmp(submitted_query, "/quit") == 0 ||
                    strcmp(submitted_query, "/exit") == 0) {
                    free(submitted_query);
                    running = 0;
                    break;
                }

                /* Handle /fork command */
                if (strncmp(submitted_query, "/fork ", 6) == 0) {
                    int fork_step = atoi(submitted_query + 6);
                    if (fork_step > 0) {
                        char *new_dir = create_session_dir(nash_dir);
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
                        int max_alias = tools.aliases ? tools.aliases->next_seq : fork_step + 5;
                        for (int loop = 0; loop <= tools.react_loop; loop++) {
                            for (int i = 0; i <= max_alias; i++) {
                                char ref[32], sl[NASH_PATH_MAX], tgt[NASH_PATH_MAX], dl[NASH_PATH_MAX];
                                snprintf(ref, sizeof(ref), "R%dS%d",
                                         loop, i);
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
                        cJSON_AddNumberToObject(cp, "react_loop", tools.react_loop);
                        if (react.last_query)
                            cJSON_AddStringToObject(cp, "user_query", react.last_query);
                        if (tools.scratchpad)
                            cJSON_AddStringToObject(cp, "scratchpad", tools.scratchpad);
                        char *cpj = cJSON_Print(cp);
                        char cp_path[NASH_PATH_MAX];
                        snprintf(cp_path, sizeof(cp_path), "%s/checkpoint.json", new_dir);
                        FILE *cpf = fopen(cp_path, "w");
                        if (cpf) { fputs(cpj, cpf); fclose(cpf); }
                        free(cpj);
                        cJSON_Delete(cp);
                        /* Switch to forked session */
                        journal_free(journal);
                        free(session_dir);
                        session_dir = new_dir;
                        journal = journal_new(session_dir);
                        tools.journal = journal;
                        tools.session_dir = session_dir;
                        alias_map_clear(tools.aliases);
                        ui_state_set_status(ui, STATUS_READY,
                            "Forked — ready for new query");
                        tui_render(ui);
                    }
                    free(submitted_query);
                    continue;
                }

                /* Handle /name command — create a named symlink to the current session */
                if (strncmp(submitted_query, "/name ", 6) == 0) {
                    const char *name = submitted_query + 6;
                    /* Validate: non-empty, no slashes, reasonable length */
                    if (strlen(name) == 0 || strlen(name) > 255 ||
                        strchr(name, '/') != NULL || strchr(name, '\n') != NULL) {
                        pthread_mutex_lock(&ui->mtx);
                        ui_state_set_status(ui, STATUS_ERROR,
                            "/name: invalid name (no slashes, max 255 chars)");
                        pthread_mutex_unlock(&ui->mtx);
                        tui_render(ui);
                        free(submitted_query);
                        continue;
                    }
                    char sessions_base[1024], link_path[1088];
                    snprintf(sessions_base, sizeof(sessions_base), "%s/sessions", nash_dir);
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
                    free(submitted_query);
                    continue;
                }

                /* Handle /cwd command — change working directory, create if needed */
                if (strncmp(submitted_query, "/cwd ", 5) == 0) {
                    const char *dir = submitted_query + 5;
                    /* Skip leading whitespace */
                    while (*dir == ' ') dir++;
                    if (*dir == '\0') {
                        pthread_mutex_lock(&ui->mtx);
                        ui_state_set_status(ui, STATUS_ERROR,
                            "/cwd: missing directory argument");
                        pthread_mutex_unlock(&ui->mtx);
                        tui_render(ui);
                        free(submitted_query);
                        continue;
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
                            free(submitted_query);
                            continue;
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
                        free(submitted_query);
                        continue;
                    }
                    /* Show success with resolved path */
                    char resolved[4096];
                    if (!getcwd(resolved, sizeof(resolved)))
                        snprintf(resolved, sizeof(resolved), "%s", dir);
                    char status_msg[4112];
                    snprintf(status_msg, sizeof(status_msg),
                        "CWD: %s", resolved);
                    pthread_mutex_lock(&ui->mtx);
                    ui_state_set_status(ui, STATUS_READY, status_msg);
                    pthread_mutex_unlock(&ui->mtx);
                    tui_render(ui);
                    free(submitted_query);
                    continue;
                }

                /* Handle /dream command — alias for /play dream */
                if (strcmp(submitted_query, "/dream") == 0) {
                    free(submitted_query);
                    submitted_query = NULL;

                    if (inferring) {
                        pthread_mutex_lock(&ui->mtx);
                        ui_state_set_status(ui, STATUS_ERROR,
                                            "Wait for inference to finish");
                        pthread_mutex_unlock(&ui->mtx);
                        tui_render(ui);
                        continue;
                    }

                    /* Load dream playbook from ~/.nash/playbooks/dream.yaml.
                     * If not found, write the default and load it. */
                    char pb_path[NASH_PATH_MAX];
                    snprintf(pb_path, sizeof(pb_path), "%s/playbooks/dream.yaml", nash_dir);
                    playbook_t *dream_pb = playbook_load(pb_path);
                    if (!dream_pb) {
                        playbook_write_default_dream(pb_path);
                        dream_pb = playbook_load(pb_path);
                    }
                    if (!dream_pb) {
                        /* Fallback: use legacy dream_worker */
                        dargs = (dream_args_t){
                            .nash_dir = nash_dir,
                            .store = shared_store,
                            .memory = memory,
                            .cfg = cfg,
                            .llm = &llm_cfg,
                            .provider = provider,
                            .server_model = server_model,
                            .ui = ui,
                            .dream_ok = 0,
                            .done = 0,
                        };
                        pthread_create(&infer_tid, NULL, dream_worker, &dargs);
                        inferring = 2;
                    } else {
                        /* Use playbook system */
                        pargs_tui = (playbook_args_t){
                            .playbook = dream_pb,
                            .nash_dir = nash_dir,
                            .store = shared_store,
                            .memory = memory,
                            .cfg = cfg,
                            .llm = &llm_cfg,
                            .provider = provider,
                            .server_model = server_model,
                            .ui = ui,
                            .playbook_ok = 0,
                            .done = 0,
                        };
                        pthread_create(&infer_tid, NULL, playbook_worker, &pargs_tui);
                        inferring = 3;  /* 3 = playbook */
                    }
                    tui_render(ui);
                    continue;
                }

                /* Handle /play command — run a playbook */
                if (strncmp(submitted_query, "/play ", 6) == 0) {
                    const char *arg = submitted_query + 6;
                    while (*arg == ' ') arg++;

                    if (inferring) {
                        pthread_mutex_lock(&ui->mtx);
                        ui_state_set_status(ui, STATUS_ERROR,
                                            "Wait for inference to finish");
                        pthread_mutex_unlock(&ui->mtx);
                        tui_render(ui);
                        free(submitted_query);
                        continue;
                    }

                    if (strcmp(arg, "list") == 0) {
                        /* List available playbooks */
                        int pb_count = 0;
                        playbook_t **pbs = playbook_list(nash_dir, &pb_count);
                        str_t display = str_new(1024);
                        str_appendf(&display, "# Available Playbooks\n\n");
                        if (pb_count == 0) {
                            str_appendf(&display, "No playbooks found in %s/playbooks/\n", nash_dir);
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
                        free(submitted_query);
                        continue;
                    }

                    /* Load playbook by name or path */
                    char pb_path[NASH_PATH_MAX];
                    if (strchr(arg, '/') || strchr(arg, '.')) {
                        snprintf(pb_path, sizeof(pb_path), "%s", arg);
                    } else {
                        snprintf(pb_path, sizeof(pb_path), "%s/playbooks/%s.yaml",
                                 nash_dir, arg);
                    }
                    playbook_t *pb = playbook_load(pb_path);
                    if (!pb) {
                        pthread_mutex_lock(&ui->mtx);
                        char errmsg[512];
                        snprintf(errmsg, sizeof(errmsg),
                                 "/play: cannot load playbook '%s'", pb_path);
                        ui_state_set_status(ui, STATUS_ERROR, errmsg);
                        pthread_mutex_unlock(&ui->mtx);
                        tui_render(ui);
                        free(submitted_query);
                        continue;
                    }

                    pargs_tui = (playbook_args_t){
                        .playbook = pb,
                        .nash_dir = nash_dir,
                        .store = shared_store,
                        .memory = memory,
                        .cfg = cfg,
                        .llm = &llm_cfg,
                        .provider = provider,
                        .server_model = server_model,
                        .ui = ui,
                        .playbook_ok = 0,
                        .done = 0,
                    };
                    pthread_create(&infer_tid, NULL, playbook_worker, &pargs_tui);
                    inferring = 3;
                    tui_render(ui);
                    free(submitted_query);
                    continue;
                }

                /* Handle /memory_recall <query> — semantic memory search.
                 * Usage: /memory_recall "what I know about X"
                 * Searches the memory store using the same hybrid scoring
                 * (embeddings + substring) that powers agent recall.
                 * Displays results directly in the TUI main pane. */
                if (strncmp(submitted_query, "/memory_recall ", 15) == 0) {
                    const char *query = submitted_query + 15;
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
                        free(submitted_query);
                        continue;
                    }
                    char qbuf[NASH_PATH_MAX];
                    memcpy(qbuf, q_start, q_len);
                    qbuf[q_len] = '\0';

                    memory_results_t results = memory_recall(memory, qbuf, 10);
                    if (results.count == 0) {
                        pthread_mutex_lock(&ui->mtx);
                        ui_state_set_status(ui, STATUS_READY,
                            "No memories matched query");
                        pthread_mutex_unlock(&ui->mtx);
                        tui_render(ui);
                        free(submitted_query);
                        continue;
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
                        /* Tags */
                        (void)0; /* tags removed */
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
                    free(submitted_query);
                    continue;
                }

                /* If paused, any query (typed or Space-resume) clears the paused flag.
                 * checkpoint_restore will inject the query into restored context. */
                if (react.paused) {
                    react.paused = 0;
                }

                /* Handle /continue: resume from checkpoint with original query.
                 * If user types "continue" or "/continue", read the original
                 * user_query from checkpoint.json and use that instead.
                 * If user types anything else, it passes through as-is —
                 * checkpoint_restore will inject it into the restored context,
                 * effectively saying "resume but with this new instruction." */
                if (strcmp(submitted_query, "continue") == 0 ||
                    strcmp(submitted_query, "/continue") == 0) {
                    char *orig = checkpoint_read_query(session_dir);
                    if (orig) {
                        free(submitted_query);
                        submitted_query = orig;
                    }
                    /* If no checkpoint exists, "continue" falls through as a
                     * regular query — the LLM will see "continue" and can
                     * interpret it in context (e.g., continue a conversation). */
                }

                /* Regular query — spawn inference in background thread */
                if (inferring) {
                    /* Previous inference still running — reject new query.
                     * react_ctx_t and tools are shared state that can't
                     * support concurrent react loops. */
                    pthread_mutex_lock(&ui->mtx);
                    ui_state_set_status(ui, STATUS_RUNNING,
                        "Still running — wait for completion");
                    pthread_mutex_unlock(&ui->mtx);
                    tui_render(ui);
                    free(submitted_query);
                    continue;
                }
                pthread_mutex_lock(&ui->mtx);
                ui_state_set_status(ui, STATUS_RUNNING, "Running...");
                ui_state_add_query(ui, submitted_query);
                ui->current_react_loop = tools.react_loop;
                pthread_mutex_unlock(&ui->mtx);
                tui_render(ui);
                iargs = (infer_args_t){
                    .react = &react, .query = strdup(submitted_query),
                    .ui = ui, .result = NULL, .done = 0,
                };
                free(submitted_query);  /* strdup'd into iargs.query; ui_state_add_query also strdup'd */
                pthread_create(&infer_tid, NULL, infer_worker, &iargs);
                inferring = 1;
                tui_render(ui);
            }

            /* Always render if dirty */
            if (ui->dirty) tui_render(ui);

            /* Small sleep to avoid busy-waiting when no input */
            /* Auto-refresh MD every 100ms during inference */
            if (inferring) {
                static struct timespec last_refresh = {0, 0};
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                long elapsed_ms = (now.tv_sec - last_refresh.tv_sec) * 1000
                                + (now.tv_nsec - last_refresh.tv_nsec) / 1000000;
                if (elapsed_ms >= 100) {
                    last_refresh = now;
                    pthread_mutex_lock(&ui->mtx);
                    ui_state_reload_file(ui);
                    pthread_mutex_unlock(&ui->mtx);
                    tui_render(ui);
                }
            }
            { struct timespec ts = {0, 10000000}; nanosleep(&ts, NULL); }  /* 10ms */
        }

        tui_shutdown();
        nash_log_set_ui(NULL);  /* disable TUI error routing */
        ui_state_free(ui);

        /* Save scratchpad */
        if (tools.scratch.count > 0) {
            char sp_path[NASH_PATH_MAX];
            snprintf(sp_path, sizeof(sp_path), "%s/scratchpad.md", session_dir);
            scratchpad_save(&tools.scratch, sp_path);
        }
        if (tools.scratchpad) free(tools.scratchpad);
        scratchpad_free(&tools.scratch);
        alias_map_free(tools.aliases);
        journal_free(journal);
        /* Remove session directory if it's empty (no work was done) */
        if (session_dir && is_dir_empty(session_dir)) {
            rmdir(session_dir);
        }
        if (session_dir) free(session_dir);
    }
    printf("Bye.\n");
    web_search_cleanup();  /* tear down auto-started SearXNG container */
    store_free(shared_store);
    memory_free(memory);
    provider_free(provider);
    free(nash_dir);
    free(props_json);
    free(server_model);
    free(llm_cfg.last_error);
    free(llm_cfg.last_error_response);
    free(llm_cfg.last_error_request);
    config_free(cfg);
    return 0;
}
