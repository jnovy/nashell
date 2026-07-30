/* Suppress -Wformat-truncation for PATH_MAX path construction.
 * snprintf with sizeof(buf) handles truncation safely. */
#pragma GCC diagnostic ignored "-Wformat-truncation"

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
#include <signal.h>
#include <sys/file.h>
#include <fcntl.h>
#include <readline/readline.h>
#include <readline/history.h>

#include "config.h"
#include "llm.h"
#include "provider.h"
#include "tools.h"
#include "tool_plugin.h"
#include "searxng.h"
#include "react.h"
#include "store.h"
#include "journal.h"
#include "frontend_tui.h"
#include "nash_limits.h"
#include "memory.h"
#include "workspace.h"
#include "session_index.h"
#include "ui_state.h"
#include "tui.h"
#include "nash_log.h"
#include "str.h"
#include "playbook.h"
#include "regression.h"
#include "postmortem.h"
#include "prompt_optimize.h"
#include "mailbox.h"
#include "telegram.h"
#include "matrix.h"
#include "banner.h"
#include "commands.h"
#include "commands_internal.h"
#include "agents.h"
#include "setup.h"

/* (load_legacy_scratchpad removed — legacy format handled by scratchpad_parse) */

/* FIX #6: Daemon mode graceful shutdown via signal handler.
 * SIGTERM/SIGINT set this flag; the daemon loop checks it each iteration. */
static volatile sig_atomic_t shutdown_requested = 0;
static void shutdown_handler(int sig) {
    (void)sig;
    shutdown_requested = 1;
}

/* ── Daemon lock file ────────────────────────────────────────────────────────
 * Prevent multiple daemon/matrix/telegram instances from running
 * simultaneously.  Each would poll the same Matrix/Telegram room,
 * causing duplicate "Processing..." messages.
 * Uses flock() — automatically released when the process exits/crashes. */
static int daemon_lock_fd = -1;

static int daemon_lock_acquire(const char *nash_dir) {
    char path[NASH_PATH_MAX];
    snprintf(path, sizeof(path), "%s/daemon.lock", nash_dir);
    daemon_lock_fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (daemon_lock_fd < 0) {
        fprintf(stderr, "[daemon] warning: cannot create lock file %s: %s\n",
                path, strerror(errno));
        return 0;  /* non-fatal — proceed without lock */
    }
    if (flock(daemon_lock_fd, LOCK_EX | LOCK_NB) != 0) {
        /* Read PID from existing lock file for diagnostics */
        char buf[32] = {0};
        pread(daemon_lock_fd, buf, sizeof(buf) - 1, 0);
        close(daemon_lock_fd);
        daemon_lock_fd = -1;
        fprintf(stderr,
            "[daemon] ✗ another nash daemon is already running (PID %s)\n"
            "[daemon]   kill it first, or use a different --data-dir\n",
            buf[0] ? buf : "?");
        return -1;
    }
    /* Write our PID */
    if (ftruncate(daemon_lock_fd, 0) == 0) {
        char pidbuf[24];
        int n = snprintf(pidbuf, sizeof(pidbuf), "%d\n", (int)getpid());
        if (write(daemon_lock_fd, pidbuf, (size_t)n) < 0) { /* ignore */ }
    }
    return 0;
}

static void daemon_lock_release(void) {
    if (daemon_lock_fd >= 0) {
        flock(daemon_lock_fd, LOCK_UN);
        close(daemon_lock_fd);
        daemon_lock_fd = -1;
    }
}

/* Check if a daemon (--matrix/--telegram) is currently running.
 * Returns 1 if daemon is running, 0 otherwise.
 * Uses non-blocking flock() probe on daemon.lock. */
static int daemon_is_running(const char *nash_dir) {
    char path[NASH_PATH_MAX];
    snprintf(path, sizeof(path), "%s/daemon.lock", nash_dir);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;  /* no lock file — no daemon */
    int locked = (flock(fd, LOCK_EX | LOCK_NB) != 0);
    if (!locked) flock(fd, LOCK_UN);  /* we grabbed it — release immediately */
    close(fd);
    return locked;
}

/* Route a result to the mailbox outbox so bridge threads (Matrix/Telegram)
 * can relay it.  Only writes if a daemon is currently running.
 * ws_name and agent_name are optional (NULL = omitted from prefix). */
static void route_to_outbox(const char *nash_dir, const char *result,
                            const char *ws_name, const char *agent_name) {
    if (!result || !nash_dir) return;
    if (!daemon_is_running(nash_dir)) return;

    char mbox_dir[NASH_PATH_MAX];
    if (mailbox_init(nash_dir, mbox_dir, sizeof(mbox_dir)) != 0) return;

    /* Build prefixed result: "[workspace: X] [agent: Y]\n\nresult" */
    str_t msg = str_new(strlen(result) + 128);
    if (ws_name && ws_name[0]) {
        str_appendf(&msg, "[workspace: %s]", ws_name);
    }
    if (agent_name && agent_name[0]) {
        if (msg.len > 0) str_append_cstr(&msg, " ");
        str_appendf(&msg, "[agent: %s]", agent_name);
    }
    if (msg.len > 0) str_append_cstr(&msg, "\n\n");
    str_append_cstr(&msg, result);

    const char *task_id = mailbox_gen_id();
    mailbox_write_result(mbox_dir, task_id, msg.data);
    nash_log("[mailbox] result routed to outbox for bridge relay");
    str_free(&msg);
}

/* Route a query notification to the mailbox outbox so bridge threads
 * can show it as the thread root.  Only writes if a daemon is running.
 * is_new_session: 1 = start new thread (root), 0 = reply in existing thread. */
static void route_query_to_outbox(const char *nash_dir, const char *query,
                                   const char *ws_name, const char *agent_name,
                                   int is_new_session) {
    if (!query || !nash_dir) return;
    if (!daemon_is_running(nash_dir)) return;

    char mbox_dir[NASH_PATH_MAX];
    if (mailbox_init(nash_dir, mbox_dir, sizeof(mbox_dir)) != 0) return;

    const char *query_id = mailbox_gen_id();
    const char *source = agent_name ? "agent" : "tui";

    /* For agents, show compact notification instead of full query */
    char agent_text[256];
    const char *text = query;
    if (agent_name && agent_name[0]) {
        snprintf(agent_text, sizeof(agent_text), "[agent %s executed]", agent_name);
        text = agent_text;
    }

    mailbox_write_query(mbox_dir, query_id, text, ws_name,
                        NULL /* route_token */,
                        is_new_session ? "root" : "reply",
                        source, agent_name);
    nash_log("[mailbox] query routed to outbox for bridge relay");
}

/* Get the nash data directory: ~/.nash/ or config override */
static char *get_nash_dir(const config_t *cfg) {
    if (cfg->data_dir && cfg->data_dir[0]) {
        mkdir(cfg->data_dir, 0755);
        return strdup(cfg->data_dir);
    }
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    char path[NASH_PATH_MAX];
    snprintf(path, sizeof(path), "%s/.nash", home);
    mkdir(path, 0755);
    return strdup(path);
}

/* Recursive mkdir: create all path components (like mkdir -p).
 * Returns 0 on success, -1 on failure (errno set). */


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

/* Set to 1 when a PATH positional arg is given on the CLI or /cwd command.
 * Controls whether repo map is injected (default off without PATH). */
int g_path_given = 0;

/* Apply model profile react_flags overrides to a react_flags_t.
 * Profile values of -1 mean "inherit" (no override). */
static void apply_profile_flags(react_flags_t *flags, const config_t *cfg) {
    if (cfg->profile_inject_memory >= 0)
        flags->inject_memory = cfg->profile_inject_memory;
    if (cfg->profile_inject_prev_result >= 0)
        flags->inject_prev_result = cfg->profile_inject_prev_result;
    if (cfg->profile_enable_reflection >= 0)
        flags->enable_reflection = cfg->profile_enable_reflection;
    if (cfg->profile_enable_pruning >= 0)
        flags->enable_pruning = cfg->profile_enable_pruning;
    if (cfg->profile_enable_compaction >= 0)
        flags->enable_compaction = cfg->profile_enable_compaction;
    if (cfg->profile_enable_scoring >= 0)
        flags->enable_scoring = cfg->profile_enable_scoring;
    /* Repo map: enabled only when a PATH arg was given on the CLI,
     * unless explicitly disabled in config (repo_map = false). */
    if (cfg->repo_map >= 0 && !cfg->repo_map)
        flags->inject_repomap = 0;   /* config explicitly disabled */
    else if (g_path_given)
        flags->inject_repomap = 1;   /* PATH given on CLI */
}

/* Build a tool_filter_t from profile-level tool filter on config.
 * Returns a filter with pointers into cfg (no allocation needed). */
static tool_filter_t build_profile_tool_filter(const config_t *cfg) {
    tool_filter_t tf = {0};
    if (cfg->n_profile_tools_allow > 0) {
        tf.allowed = (const char **)cfg->profile_tools_allow;
        tf.n_allowed = cfg->n_profile_tools_allow;
    }
    if (cfg->n_profile_tools_block > 0) {
        tf.blocked = (const char **)cfg->profile_tools_block;
        tf.n_blocked = cfg->n_profile_tools_block;
    }
    if (cfg->n_profile_tool_descs > 0) {
        tf.desc_names = cfg->profile_tool_desc_names;
        tf.desc_values = cfg->profile_tool_desc_values;
        tf.n_descs = cfg->n_profile_tool_descs;
    }
    return tf;
}

/* --- Threading for non-blocking inference --- */
typedef struct {
    react_ctx_t *react;
    char        *query;
    ui_state_t  *ui;
    char        *result;
    atomic_int done;
} infer_args_t;

/* --- Threading for non-blocking inference --- */

static void threaded_event_cb(const react_event_t *ev, void *userdata) {
    infer_args_t *a = (infer_args_t *)userdata;
    pthread_mutex_lock(&a->ui->mtx);
    ui_state_on_event(ev, (void *)a->ui);
    pthread_mutex_unlock(&a->ui->mtx);
}

static void *infer_worker(void *arg) {
    infer_args_t *a = (infer_args_t *)arg;
    a->result = react_run(a->react, a->query, threaded_event_cb, a);
    a->done = 1;
    return NULL;
}

/* ── Session init/cleanup helpers ──────────────────────────────────
 * Unifies the 4 duplicated initialization paths (daemon, headless,
 * mailbox, TUI) into a single function. */

/* session_idx is set after session_init_tools by the caller or globally */
static session_index_t *g_session_idx = NULL;

/* Role-based provider routing — file-scope statics so cleanup_globals can free them.
 * NULL = not configured (use default provider for that role). */
static provider_t *g_planner_provider = NULL;
static provider_t *g_reflection_provider = NULL;
static provider_t *g_consolidation_provider = NULL;

/* Create a provider_t from a named [routing] role.
 * Looks up the role_name in cfg->named_providers, builds a provider_config_t
 * from its TOML config + global sampling settings, and returns provider_create().
 * Returns NULL if role_name is NULL or creation fails. */
static provider_t *create_role_provider(const config_t *cfg,
                                        const char *role_name,
                                        const char *role_label) {
    if (!role_name) return NULL;

    const named_provider_t *np = config_find_provider(cfg, role_name);
    if (!np) {
        fprintf(stderr, "[routing] %s provider '%s' not found in [providers.*]\n",
                role_label, role_name);
        return NULL;
    }

    provider_config_t pcfg = {
        .type           = provider_type_from_str(np->config.type),
        .model_id       = np->config.model_id,
        .api_base       = np->config.api_base,
        .api_key_env    = np->config.api_key_env,
        .project_id     = np->config.project_id,
        .region         = np->config.region,
        .context_size   = np->config.context_size,
        .chars_per_token = np->config.chars_per_token,
        .caching        = np->config.caching,
        .max_tokens     = cfg->max_tokens,
        .temperature    = cfg->temperature,
        .top_p          = cfg->top_p,
        .top_k          = cfg->top_k,
        .enable_thinking = 0,
        .thinking_budget = -1,
        .llm_timeout     = cfg->llm_timeout,
        .max_retries     = cfg->provider_max_retries,
        .retry_base_sec  = cfg->provider_retry_base,
    };
    provider_t *p = provider_create(&pcfg);
    if (!p) {
        fprintf(stderr, "[routing] failed to create %s provider '%s'\n",
                role_label, role_name);
        return NULL;
    }
    nash_log("[routing] %s = %s (%s, %s)",
             role_label, role_name, np->config.type,
             np->config.model_id ? np->config.model_id : "auto");
    return p;
}

static void session_init_tools(tool_ctx_t *tools, store_t *store,
                               journal_t *journal, memory_t *memory,
                               workspace_t *ws,
                               char *session_dir, config_t *cfg,
                               provider_t *provider) {
    memset(tools, 0, sizeof(*tools));
    tools->store = store;
    tools->journal = journal;
    tools->memory = memory;
    tools->ws = ws;
    tools->session_dir = session_dir;
    tools->session_lock_fd = session_lock_acquire(session_dir);
    tools->cfg = cfg;
    tools->provider = provider;
    tools->react_loop = journal_max_react_loop(journal) + 1;
    tools->aliases = alias_map_new();
    tools->session_idx = g_session_idx;  /* v4 unified memory L3 */
    scratchpad_init(&tools->scratch);
    if (session_dir) {
        scratchpad_load(&tools->scratch, session_dir);
    }
    tools->tool_filter = build_profile_tool_filter(cfg);
    tool_apply_default_blocks(&tools->tool_filter);
    tools->last_notes_step = -1;  /* -1 = never used */
}

static void session_init_react(react_ctx_t *react, provider_t *provider,
                               provider_t *planner_provider,
                               provider_t *reflection_provider,
                               tool_ctx_t *tools, config_t *cfg) {
    memset(react, 0, sizeof(*react));
    react->provider = provider;
    react->planner_provider = planner_provider;
    react->reflection_provider = reflection_provider;
    react->tools = tools;
    react->max_steps = cfg->max_react_steps;
    react->verbose = 1;
    react->flags = (react_flags_t)REACT_FLAGS_DEFAULT;
    apply_profile_flags(&react->flags, cfg);
    react->parent_loop = -1;
    pthread_mutex_init(&react->user_ask_mutex, NULL);
    pthread_cond_init(&react->user_ask_cond, NULL);
    pthread_mutex_init(&react->pause_mutex, NULL);
    pthread_cond_init(&react->pause_cond, NULL);
}

static void session_cleanup(tool_ctx_t *tools, react_ctx_t *react,
                            journal_t *journal) {
    pthread_mutex_destroy(&react->user_ask_mutex);
    pthread_cond_destroy(&react->user_ask_cond);
    pthread_mutex_destroy(&react->pause_mutex);
    pthread_cond_destroy(&react->pause_cond);
    free(react->pause_query);
    react->pause_query = NULL;
    free(react->tui_viewing_file);
    react->tui_viewing_file = NULL;
    tool_free_deferred_consolidations(tools);
    scratchpad_free(&tools->scratch);
    alias_map_free(tools->aliases);
    free(tools->last_spec_hash);
    tools->last_spec_hash = NULL;
    session_lock_release(tools->session_lock_fd);
    tools->session_lock_fd = -1;
    journal_free(journal);
}

/* Unified cleanup for global resources.
 * Replaces 8+ duplicated cleanup sequences across early-return paths.
 * All _free functions handle NULL safely. */
static void cleanup_globals(store_t *shared_store, workspace_t *ws,
                            provider_t *provider, char *nash_dir,
                            char *props_json, char *server_model,
                            config_t *cfg) {
    session_index_free(g_session_idx);
    g_session_idx = NULL;
    store_free(shared_store);
    workspace_free(ws);
    /* Free role providers (if different from main provider) */
    if (g_planner_provider && g_planner_provider != provider)
        provider_free(g_planner_provider);
    if (g_reflection_provider && g_reflection_provider != provider)
        provider_free(g_reflection_provider);
    if (g_consolidation_provider && g_consolidation_provider != provider)
        provider_free(g_consolidation_provider);
    g_planner_provider = NULL;
    g_reflection_provider = NULL;
    g_consolidation_provider = NULL;
    provider_free(provider);
    free(nash_dir);
    free(props_json);
    free(server_model);
    config_free(cfg);
}

int main(int argc, char **argv) {
    /* FIX: Ignore SIGPIPE globally. Without this, broken pipe from curl
     * (e.g., LLM server drops connection mid-stream) or from popen/write
     * to a defunct subprocess kills the entire process silently.
     * curl sets CURLOPT_NOSIGNAL by default in multi-threaded code, but
     * our popen calls (gcloud auth) and direct pipe I/O are unprotected. */
    signal(SIGPIPE, SIG_IGN);

    /* Sort plugin-registered tools by name for deterministic ordering.
     * Constructors run before main() in undefined order across TUs. */
    tool_plugin_sort();

    /* Load config from ~/.nash/config.toml (or default) */
    char config_path[NASH_PATH_MAX];
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(config_path, sizeof(config_path), "%s/.nash/config.toml", home);

    config_t *cfg = config_load(config_path);

    /* Load external plugins from configured directory */
    if (cfg->plugin_dir && cfg->plugin_dir[0]) {
        int n = tool_plugin_load_dir(cfg->plugin_dir);
        if (n > 0)
            tool_plugin_sort();  /* re-sort after adding external plugins */
    }

    /* CLI flags override config */
    const char *query = NULL;
    const char *session_dir_arg = NULL;
    const char *play_arg = NULL;
    const char *validate_playbook_arg = NULL;  /* --validate-playbook NAME: validate and exit */
    int regression_mode = 0;
    const char *validate_harness = NULL;  /* "baseline" or "compare" */
    int regression_split = -1;           /* -1 = all, 0 = held-in, 1 = held-out */
    int postmortem_mode = 0;
    int postmortem_sessions = 50;        /* default: scan last 50 sessions */
    int spec_mode = 0;                   /* --spec: dump resolved spec and exit */
    const char *load_spec_path = NULL;    /* --load-spec FILE: overlay spec on config */
    const char *optimize_budget = NULL;   /* --optimize BUDGET: GEPA prompt optimization */
    const char *reflect_model_arg = NULL; /* --reflect-model MODEL: reflection LM for optimization */
    int optimize_epochs = 1;              /* --epochs N: SkillOpt multi-epoch training (default 1) */
    int optimize_edit_budget = 4;         /* --edit-budget N: initial edit budget L_0 (default 4) */
    int mailbox_mode = 0;                 /* --mailbox: enable file-based mailbox for user_ask */
    int daemon_mode = 0;                  /* --daemon: watch mailbox inbox for tasks */
    int telegram_mode = 0;                /* --telegram: Telegram Bot bridge (implies --daemon) */
    int matrix_mode = 0;                  /* --matrix: Matrix bridge (implies --daemon) */
    int mailbox_timeout = 0;              /* --mailbox-timeout SECS: user_ask timeout */
    int agents_mode = 0;                  /* --agent: agent management mode */
    int agents_list = 0;                  /* --agent (default): show agent table */
    int agents_dry_run = 0;               /* --agent --dry-run: show what would run */
    int agents_due = 0;                   /* --agent --due: run all due agents */
    const char *agent_target_id = NULL;   /* --agent ID: run specific agent */
    char *agent_arguments = NULL;         /* --agent ID arg1 arg2...: arguments for agent */
    const char *provider_name_arg = NULL;  /* --provider NAME: use named provider from [providers.*] */
    int setup_mode = 0;                    /* --setup: interactive setup wizard */
    const char *path_arg = NULL;            /* positional PATH: working directory for repo map */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--api") == 0 && i + 1 < argc) {
            /* --api URL: create an ad-hoc local provider and select it */
            const char *url = argv[++i];
            int n = cfg->n_named_providers;
            cfg->named_providers = realloc(cfg->named_providers,
                                           (n + 1) * sizeof(named_provider_t));
            memset(&cfg->named_providers[n], 0, sizeof(named_provider_t));
            cfg->named_providers[n].name = strdup("__cli_api");
            cfg->named_providers[n].config.type = strdup("local");
            cfg->named_providers[n].config.api_base = strdup(url);
            cfg->n_named_providers = n + 1;
            free(cfg->routing.default_provider);
            cfg->routing.default_provider = strdup("__cli_api");
        } else if (strcmp(argv[i], "--provider") == 0 && i + 1 < argc) {
            provider_name_arg = argv[++i];
        } else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--query") == 0) && i + 1 < argc) {
            query = argv[++i];
        } else if (strcmp(argv[i], "--play") == 0 && i + 1 < argc) {
            play_arg = argv[++i];
        } else if (strcmp(argv[i], "--validate-playbook") == 0 && i + 1 < argc) {
            validate_playbook_arg = argv[++i];
        } else if (strcmp(argv[i], "--data-dir") == 0 && i + 1 < argc) {
            free(cfg->data_dir);
            cfg->data_dir = strdup(argv[++i]);
        } else if (strcmp(argv[i], "--session") == 0 && i + 1 < argc) {
            session_dir_arg = argv[++i];
        } else if (strcmp(argv[i], "--regression") == 0) {
            regression_mode = 1;
        } else if (strcmp(argv[i], "--validate-harness") == 0 && i + 1 < argc) {
            validate_harness = argv[++i];
            regression_mode = 1;
        } else if (strcmp(argv[i], "--split") == 0 && i + 1 < argc) {
            const char *sp = argv[++i];
            if (strcmp(sp, "held-in") == 0) regression_split = 0;
            else if (strcmp(sp, "held-out") == 0) regression_split = 1;
        } else if (strcmp(argv[i], "--postmortem") == 0) {
            postmortem_mode = 1;
        } else if (strcmp(argv[i], "--postmortem-sessions") == 0 && i + 1 < argc) {
            postmortem_sessions = atoi(argv[++i]);
            postmortem_mode = 1;
        } else if (strcmp(argv[i], "--setup") == 0) {
            setup_mode = 1;
        } else if (strcmp(argv[i], "--spec") == 0) {
            spec_mode = 1;
        } else if (strcmp(argv[i], "--load-spec") == 0 && i + 1 < argc) {
            load_spec_path = argv[++i];
        } else if (strcmp(argv[i], "--optimize") == 0 && i + 1 < argc) {
            optimize_budget = argv[++i];
        } else if (strcmp(argv[i], "--reflect-model") == 0 && i + 1 < argc) {
            reflect_model_arg = argv[++i];
        } else if (strcmp(argv[i], "--epochs") == 0 && i + 1 < argc) {
            optimize_epochs = atoi(argv[++i]);
            if (optimize_epochs < 1) optimize_epochs = 1;
        } else if (strcmp(argv[i], "--edit-budget") == 0 && i + 1 < argc) {
            optimize_edit_budget = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--mailbox") == 0) {
            mailbox_mode = 1;
        } else if (strcmp(argv[i], "--daemon") == 0) {
            daemon_mode = 1;
            mailbox_mode = 1;  /* daemon implies mailbox */
        } else if (strcmp(argv[i], "--telegram") == 0) {
            telegram_mode = 1;
            daemon_mode = 1;   /* telegram implies daemon */
            mailbox_mode = 1;  /* daemon implies mailbox */
        } else if (strcmp(argv[i], "--matrix") == 0) {
            matrix_mode = 1;
            daemon_mode = 1;   /* matrix implies daemon */
            mailbox_mode = 1;  /* daemon implies mailbox */
        } else if (strcmp(argv[i], "--mailbox-timeout") == 0 && i + 1 < argc) {
            mailbox_timeout = atoi(argv[++i]);
            mailbox_mode = 1;
        } else if (strcmp(argv[i], "--workspace-list") == 0 || strcmp(argv[i], "-wl") == 0) {
            char *nd = get_nash_dir(cfg);
            workspace_list_all(nd);
            free(nd);
            config_free(cfg);
            return 0;
        } else if ((strcmp(argv[i], "--workspace") == 0 || strcmp(argv[i], "-w") == 0) && i + 1 < argc) {
            free(cfg->workspace);
            cfg->workspace = strdup(argv[++i]);
        } else if (strcmp(argv[i], "--isolated") == 0) {
            cfg->workspace_isolated = 1;
        } else if (strcmp(argv[i], "--agent") == 0) {
            agents_mode = 1;
            /* Collect positional args: --agent ID [arg1 arg2 ...] */
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                agent_target_id = argv[++i];
                /* Remaining non-flag args become agent arguments */
                if (i + 1 < argc && argv[i + 1][0] != '-') {
                    /* Concatenate remaining positional args */
                    size_t total = 0;
                    for (int j = i + 1; j < argc && argv[j][0] != '-'; j++)
                        total += strlen(argv[j]) + 1;
                    agent_arguments = malloc(total + 1);
                    agent_arguments[0] = '\0';
                    for (int j = i + 1; j < argc && argv[j][0] != '-'; j++) {
                        if (agent_arguments[0]) strcat(agent_arguments, " ");
                        strcat(agent_arguments, argv[j]);
                        i = j;
                    }
                }
            }
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            agents_dry_run = 1;
        } else if (strcmp(argv[i], "--due") == 0) {
            agents_due = 1;
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: nash [OPTIONS] [PATH]\n");
            printf("\nPATH: working directory for codebase context (enables repo map).\n");
            printf("      Without PATH, no repo map is generated (saves context tokens).\n");
            printf("\nOptions:\n");
            printf("  --session DIR   Open existing session directory\n");
            printf("  --api URL       LLM server URL (creates ad-hoc local provider)\n");
            printf("  --provider NAME Use named provider from [providers.*] in config\n");
            printf("  -p QUERY        Run single query and exit (headless mode)\n");
            printf("  --play NAME     Run a playbook and exit (e.g. --play dream)\n");
            printf("  --data-dir PATH Data directory (default: ~/.nash/)\n");
            printf("  --session DIR   Open existing session directory\n");
            printf("\nSelf-Harness:\n");
            printf("  --regression          Run regression test suite\n");
            printf("  --split SPLIT         Filter: held-in or held-out\n");
            printf("  --validate-harness MODE  baseline or compare\n");
            printf("  --postmortem          Analyze session failures\n");
            printf("  --postmortem-sessions N  Sessions to scan (default 50)\n");
            printf("  --optimize BUDGET     GEPA prompt optimization (light/medium/heavy/N)\n");
            printf("  --reflect-model MODEL Reflection LM for optimization\n");
            printf("  --epochs N            SkillOpt multi-epoch training (default 1)\n");
            printf("  --edit-budget N       Initial edit budget L_0 (default 4, cosine decay)\n");
            printf("\nSpec:\n");
            printf("  --spec                Dump fully-resolved config spec and exit\n");
            printf("  --load-spec FILE      Load a spec TOML as config overlay\n");
            printf("\nSetup:\n");
            printf("  --setup               Interactive first-time setup wizard\n");
            printf("\nWorkspace (memory segregation):\n");
            printf("  -w, --workspace NAME  Activate a named workspace\n");
            printf("  -wl, --workspace-list List all available workspaces\n");
            printf("  --isolated            Fully isolate workspace (no global recall)\n");
            printf("\nMailbox (headless communication):\n");
            printf("  --mailbox             Enable file-based mailbox for user_ask in -p mode\n");
            printf("  --daemon              Watch mailbox inbox for task files (implies --mailbox)\n");
            printf("  --telegram            Telegram Bot bridge (implies --daemon)\n");
            printf("  --matrix              Matrix bridge (implies --daemon)\n");
            printf("  --mailbox-timeout N   Timeout in seconds for user_ask answers (0=forever)\n");
            printf("\nAgents (autonomous scheduled workflows):\n");
            printf("  --agent              List all discovered agents and status\n");
            printf("  --agent --due        Scan workspaces, run due agents, exit\n");
            printf("  --agent --dry-run    Show what would run without executing\n");
            printf("  --agent ID [ARGS]    Run specific agent (like /agent run in TUI)\n");
            printf("\nConfig: %s\n", config_path);
            config_free(cfg);
            return 0;
        } else if (argv[i][0] != '-') {
            /* Positional arg: treat as PATH for repo map context */
            path_arg = argv[i];
        }
    }

    /* Handle positional PATH arg: chdir and enable repo map */
    if (path_arg) {
        struct stat st;
        if (stat(path_arg, &st) != 0 || !S_ISDIR(st.st_mode)) {
            fprintf(stderr, "[error] PATH is not a directory: %s\n", path_arg);
            config_free(cfg);
            return 1;
        }
        if (chdir(path_arg) != 0) {
            fprintf(stderr, "[error] cannot chdir to: %s\n", path_arg);
            config_free(cfg);
            return 1;
        }
        g_path_given = 1;
    }

    /* Initialize data directory */
    char *nash_dir = get_nash_dir(cfg);

    /* Create models/ directory for per-model profiles */
    {
        char models_dir[1024];
        snprintf(models_dir, sizeof(models_dir), "%s/models", nash_dir);
        mkdir(models_dir, 0755);
        config_load_model_profiles(cfg, models_dir);
    }

    /* Write default config if it doesn't exist */
    config_write_default(config_path);

    /* Load credentials.toml (API keys) and merge into named providers */
    config_load_credentials(cfg, nash_dir);

    /* --setup: interactive setup wizard (exit early, before provider resolution) */
    if (setup_mode) {
        int rc = setup_run(nash_dir, 0);
        free(nash_dir);
        config_free(cfg);
        return rc;
    }

    /* Apply spec overlay if --load-spec was given.
     * This overrides config.toml settings before provider creation. */
    if (load_spec_path) {
        if (config_load_spec_overlay(cfg, load_spec_path) != 0) {
            fprintf(stderr, "[error] failed to load spec from %s\n", load_spec_path);
            free(nash_dir);
            config_free(cfg);
            return 1;
        }
    }

    /* CWD-based workspace auto-detection: if no -w was given, check if CWD
     * is inside ~/.nash/workspaces/<name>/ and auto-activate that workspace. */
    if (!cfg->workspace || !cfg->workspace[0]) {
        char cwd_auto[NASH_PATH_MAX];
        if (getcwd(cwd_auto, sizeof(cwd_auto))) {
            char ws_prefix[NASH_PATH_MAX];
            snprintf(ws_prefix, sizeof(ws_prefix), "%s/workspaces/", nash_dir);
            size_t pfx_len = strlen(ws_prefix);
            if (strncmp(cwd_auto, ws_prefix, pfx_len) == 0 && cwd_auto[pfx_len]) {
                /* CWD is under workspaces/ — extract workspace name.
                 * e.g. ~/.nash/workspaces/rh/container-tools → "rh/container-tools" */
                free(cfg->workspace);
                cfg->workspace = strdup(cwd_auto + pfx_len);
                /* Strip trailing slash if any */
                size_t wlen = strlen(cfg->workspace);
                if (wlen > 0 && cfg->workspace[wlen - 1] == '/')
                    cfg->workspace[wlen - 1] = '\0';
                fprintf(stderr, "[info] auto-detected workspace from CWD: %s\n",
                        cfg->workspace);
            }
        }
    }

    /* Per-workspace config overlay: ~/.nash/workspaces/<name>/config.toml
     * Allows workspace-specific provider, limits, and other settings.
     * Applied after global config + spec overlay, before provider creation. */
    if (cfg->workspace && cfg->workspace[0]) {
        char ws_config[NASH_PATH_MAX];
        snprintf(ws_config, sizeof(ws_config), "%s/workspaces/%s/config.toml",
                 nash_dir, cfg->workspace);
        struct stat ws_st;
        if (stat(ws_config, &ws_st) == 0) {
            fprintf(stderr, "[info] loading workspace config: %s\n", ws_config);
            config_load_spec_overlay(cfg, ws_config);
        }
    }

    /* ── Postmortem mode: no LLM needed ── */
    if (postmortem_mode) {
        postmortem_report_t *pm = postmortem_analyze(nash_dir, postmortem_sessions);
        postmortem_print(pm);

        /* Save evidence bundle */
        char bundle_path[NASH_PATH_MAX];
        snprintf(bundle_path, sizeof(bundle_path), "%s/postmortem.md", nash_dir);
        if (pm->total_failures > 0) {
            postmortem_save(pm, bundle_path);
            fprintf(stderr, "  Evidence bundle saved: %s\n\n", bundle_path);
        }

        postmortem_free(pm);
        free(nash_dir);
        config_free(cfg);
        return 0;
    }

    /* ── Create provider from config ── */
    /* Resolve which provider to use:
     * 1. --provider NAME (CLI override) -> named provider
     * 2. [routing].default -> named provider */
    provider_config_toml_t resolved_prov = {0};
    if (config_resolve_provider(cfg, provider_name_arg, &resolved_prov) != 0) {
        /* First-run auto-detection: offer interactive setup if TTY available */
        if (isatty(STDIN_FILENO) && isatty(STDERR_FILENO)) {
            fprintf(stderr, "\nnash: no provider configured.\n"
                            "Run interactive setup? [Y/n]: ");
            fflush(stderr);
            char ans[16] = {0};
            if (fgets(ans, sizeof(ans), stdin) &&
                (ans[0] == '\n' || ans[0] == 'y' || ans[0] == 'Y')) {
                int rc = setup_run(nash_dir, 0);
                free(nash_dir);
                config_free(cfg);
                return rc;
            }
        }
        fprintf(stderr, "nash: failed to resolve provider configuration.\n"
                        "Run 'nash --setup' to configure, or use --api URL\n");
        free(nash_dir);
        config_free(cfg);
        return 1;
    }

    /* Deep-copy resolved provider into cfg->provider (owned strings).
     * This populates the "resolved provider snapshot" that banner.c,
     * react.c, config_dump_spec, etc. all read from. */
    cfg->provider.type           = resolved_prov.type ? strdup(resolved_prov.type) : NULL;
    cfg->provider.model_id       = resolved_prov.model_id ? strdup(resolved_prov.model_id) : NULL;
    cfg->provider.api_base       = resolved_prov.api_base ? strdup(resolved_prov.api_base) : NULL;
    cfg->provider.api_key_env    = resolved_prov.api_key_env ? strdup(resolved_prov.api_key_env) : NULL;
    cfg->provider.project_id     = resolved_prov.project_id ? strdup(resolved_prov.project_id) : NULL;
    cfg->provider.region         = resolved_prov.region ? strdup(resolved_prov.region) : NULL;
    cfg->provider.context_size   = resolved_prov.context_size;
    cfg->provider.chars_per_token = resolved_prov.chars_per_token;
    cfg->provider.caching        = resolved_prov.caching;

    /* Apply provider-type-specific env var defaults (Vertex, OpenAI, etc.) */
    config_apply_provider_env_defaults(cfg);

    /* Log which provider we resolved */
    if (provider_name_arg) {
        nash_log("[config] --provider %s", provider_name_arg);
    } else if (cfg->routing.default_provider) {
        nash_log("[config] [routing].default = %s", cfg->routing.default_provider);
    }

    provider_config_t pcfg = {
        .type           = provider_type_from_str(cfg->provider.type),
        .model_id       = cfg->provider.model_id,
        .api_base       = cfg->provider.api_base,
        .api_key_env    = cfg->provider.api_key_env,
        .project_id     = cfg->provider.project_id,
        .region         = cfg->provider.region,
        .context_size   = cfg->provider.context_size,
        .chars_per_token = cfg->provider.chars_per_token,
        .caching        = cfg->provider.caching,
        .max_tokens     = cfg->max_tokens,
        .temperature    = cfg->temperature,
        .top_p          = cfg->top_p,
        .top_k          = cfg->top_k,
        .enable_thinking = 0,
        .thinking_budget = -1,
        .llm_timeout     = cfg->llm_timeout,
        .max_retries     = cfg->provider_max_retries,
        .retry_base_sec  = cfg->provider_retry_base,
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
        context_size = llm_fetch_context_size(cfg->provider.api_base);
        server_model = llm_fetch_model_name(cfg->provider.api_base);
        props_json = llm_fetch_props_json(cfg->provider.api_base);
    } else {
        /* API providers: use config values, with sensible defaults */
        context_size = cfg->provider.context_size;
        server_model = cfg->provider.model_id ? strdup(cfg->provider.model_id) : NULL;

        /* Cloud providers have no /props endpoint to auto-detect context_size.
         * Without a default, context eviction never triggers. */
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

    /* ── Apply model profile (Unified Spec) ── */
    const char *matched_profile_file = NULL;
    if (server_model) {
        const model_profile_t *profile = config_match_model(cfg, server_model);
        if (profile) {
            matched_profile_file = profile->source_file;
            cfg->matched_profile_file = matched_profile_file;
            fprintf(stderr, "[model-profile] matched '%s' from %s\n",
                    profile->match, profile->source_file);

            /* Apply all profile overrides via unified config_apply_profile() */
            config_apply_profile(cfg, profile);

            /* Sync chars_per_token to provider vtable (provider has its own copy) */
            if (provider && cfg->provider.chars_per_token > 0)
                provider->cfg.chars_per_token = cfg->provider.chars_per_token;

            /* native_context warning */
            if (profile->native_context > 0 && context_size > 0 &&
                context_size < profile->native_context / 2) {
                fprintf(stderr, "[model-profile] ⚠ server n_ctx=%d but %s supports %d\n",
                        context_size, profile->match, profile->native_context);
            }
        }
    }

    /* llm_config_t removed — provider_t is the single source of truth. */

    /* ── Role-based provider routing ──
     * Create separate providers for planner/reflection/consolidation roles
     * if configured in [routing]. Skip if the role points to the same
     * named provider as [routing].default (avoid duplicate provider_t). */
    {
        const char *def = cfg->routing.default_provider;
        if (cfg->routing.planner && (!def || strcmp(cfg->routing.planner, def) != 0))
            g_planner_provider = create_role_provider(cfg, cfg->routing.planner, "planner");
        if (cfg->routing.worker && def && strcmp(cfg->routing.worker, def) != 0)
            fprintf(stderr, "[routing] worker = %s (overrides default for execution steps)\n",
                    cfg->routing.worker);
        if (cfg->routing.reflection && (!def || strcmp(cfg->routing.reflection, def) != 0))
            g_reflection_provider = create_role_provider(cfg, cfg->routing.reflection, "reflection");
        if (cfg->routing.consolidation && (!def || strcmp(cfg->routing.consolidation, def) != 0))
            g_consolidation_provider = create_role_provider(cfg, cfg->routing.consolidation, "consolidation");
    }

    /* If [routing].worker differs from default, it IS the main provider.
     * Swap: create worker provider, move original to planner if no explicit planner. */
    if (cfg->routing.worker && cfg->routing.default_provider &&
        strcmp(cfg->routing.worker, cfg->routing.default_provider) != 0) {
        provider_t *worker_provider = create_role_provider(cfg, cfg->routing.worker, "worker");
        if (worker_provider) {
            if (!g_planner_provider)
                g_planner_provider = provider;  /* old default becomes planner */
            else
                provider_free(provider);      /* explicit planner exists, free old default */
            provider = worker_provider;       /* worker becomes the main provider */

            /* Update server_model to reflect the actual running model.
             * Without this, status bar / memory / workspace all show the
             * original default model instead of the worker model. */
            free(server_model);
            server_model = worker_provider->cfg.model_id
                         ? strdup(worker_provider->cfg.model_id) : NULL;
        }
    }

    /* ── Spec mode: dump resolved config and exit ── */
    if (spec_mode) {
        config_dump_spec(cfg, stdout, matched_profile_file);
        if (g_planner_provider && g_planner_provider != provider)
            provider_free(g_planner_provider);
        if (g_reflection_provider && g_reflection_provider != provider)
            provider_free(g_reflection_provider);
        if (g_consolidation_provider && g_consolidation_provider != provider)
            provider_free(g_consolidation_provider);
        provider_free(provider);
        free(nash_dir);
        free(props_json);
        free(server_model);
        config_free(cfg);
        return 0;
    }

    /* Print banner (skip in headless playbook/agent mode) */
    if (!play_arg && !agents_mode)
        print_banner(cfg, props_json, nash_dir, matched_profile_file);

    /* Shared store + memory (workspace-aware) */
    store_t *shared_store = store_new(nash_dir);

    /* Create workspace: two-layer memory (global + optional workspace).
     * If no workspace is configured, workspace_t wraps just the global
     * memory — backward compatible with single-pool behavior. */
    int ws_isolated = cfg->workspace_isolated ||
                      (cfg->workspace_global_recall == 0);
    workspace_t *ws = workspace_new(nash_dir, cfg->workspace,
                                    ws_isolated, cfg->workspace_global_weight);
    /* For backward compatibility, 'memory' points to global layer.
     * Code that hasn't been migrated to workspace_* yet (e.g., consolidation)
     * continues to use ctx->memory which points here. */
    memory_t *memory = ws ? ws->global : NULL;
    if (server_model && memory)
        memory->model = strdup(server_model);
    /* Also set model on workspace memory if it exists */
    if (server_model && ws && ws->workspace)
        ws->workspace->model = strdup(server_model);

    if (cfg->workspace && cfg->workspace[0])
        fprintf(stderr, "[info] workspace: %s%s\n", cfg->workspace,
                ws_isolated ? " (isolated)" : "");

    /* Set recall tuning parameters from config (centralized sync) */
    workspace_set_recall_config(ws, cfg->recall_min_score,
                                cfg->recall_blend_semantic,
                                cfg->recall_blend_substring,
                                cfg->vscore_exponent);

    /* Prune stale memories at startup */
    int pruned = workspace_prune(ws,
                                 cfg->prune_min_score, cfg->prune_min_evidence);
    if (pruned > 0)
        fprintf(stderr, "[info] pruned %d stale memories\n", pruned);

    /* Initialize semantic embeddings for memory matching (if configured) */
    if (cfg->embedding.type && strcmp(cfg->embedding.type, "none") != 0) {
        workspace_init_embeddings(ws, cfg->embedding.type,
                                  cfg->embedding.model, cfg->embedding.api_base,
                                  cfg->embedding.model_path,
                                  cfg->embedding.dimension,
                                  cfg->embedding.max_input_chars);
    }

    /* v4 unified memory: load session index for L3 search via memory_query.
     * Scans sessions/<ts>/ for chunks.emb (v4.1) or summary.emb (legacy). */
    session_index_t *session_idx = NULL;
    {
        char sessions_dir[NASH_PATH_MAX];
        snprintf(sessions_dir, sizeof(sessions_dir), "%s/sessions", nash_dir);
        session_idx = session_index_load(sessions_dir);

        /* Also load workspace sessions into the same index */
        if (cfg->workspace && cfg->workspace[0] && session_idx) {
            char *ws_sessions = sessions_base_dir(nash_dir, cfg->workspace);
            session_index_load_dir(session_idx, ws_sessions);
            free(ws_sessions);
        }

        /* v4.1: Backfill chunk embeddings for sessions without chunks.emb.
         * Runs at startup, skips sessions already processed.
         * ~5ms per chunk via ONNX, ~5 chunks/session avg. */
        if (memory && memory_has_embeddings(memory)) {
            embed_ctx_t *backfill_embed = memory_embed_ctx(memory);
            if (backfill_embed) {
                session_index_chunk_backfill(sessions_dir, backfill_embed,
                                            session_idx);
                /* Also backfill workspace sessions */
                if (cfg->workspace && cfg->workspace[0]) {
                    char *ws_sessions = sessions_base_dir(nash_dir, cfg->workspace);
                    session_index_chunk_backfill(ws_sessions, backfill_embed,
                                                session_idx);
                    free(ws_sessions);
                }
            }
        }

        g_session_idx = session_idx;  /* make available to session_init_tools */
    }

    /* Dream reminder — usage-based memory consolidation reminder.
     *
     * Counts entries created since last dream using existing per-entry
     * created_at timestamps (P1) vs .last_dream file mtime.
     * If count >= dream_reminder_threshold, shows a warning in the status bar.
     * Does NOT auto-trigger — user decides when to run /dream.
     *
     * Research basis: Generative Agents [Park et al., 2023] — importance
     * threshold accumulation triggers reflection. Here the "importance"
     * being accumulated is the count of unconsolidated writes. */
    int dream_new_count = 0;
    if (cfg->dream_reminder_threshold > 0 && memory) {
        /* Check .last_dream in the active memory dir.
         * When workspace is active, check workspace memory dir;
         * otherwise check global memory dir. */
        char dream_ts_path[NASH_PATH_MAX];
        if (ws && ws->workspace) {
            snprintf(dream_ts_path, sizeof(dream_ts_path),
                     "%s/workspaces/%s/.memory/.last_dream",
                     nash_dir, cfg->workspace);
        } else {
            snprintf(dream_ts_path, sizeof(dream_ts_path),
                     "%s/memory/.last_dream", nash_dir);
        }
        struct stat dream_st;

        if (stat(dream_ts_path, &dream_st) != 0) {
            /* No .last_dream file — never dreamed, count all entries */
            dream_new_count = memory_count(memory);
            if (ws && ws->workspace)
                dream_new_count += memory_count(ws->workspace);
        } else {
            double last_dream_epoch = (double)dream_st.st_mtime;
            for (int i = 0; i < memory_count(memory); i++) {
                if (memory->idx.entries[i].created_at > last_dream_epoch)
                    dream_new_count++;
            }
            /* Also count new workspace entries */
            if (ws && ws->workspace) {
                for (int i = 0; i < memory_count(ws->workspace); i++) {
                    if (ws->workspace->idx.entries[i].created_at > last_dream_epoch)
                        dream_new_count++;
                }
            }
        }
    }

    /* ── Regression test mode: --regression ── */
    if (regression_mode) {
        /* Create regression directory and seed query bank */
        char regression_dir[NASH_PATH_MAX];
        snprintf(regression_dir, sizeof(regression_dir), "%s/regression", nash_dir);
        regression_write_seed(regression_dir);

        /* Load query banks */
        int n_banks = 0;
        query_bank_t *banks = regression_load_banks(regression_dir, &n_banks);
        if (!banks || n_banks == 0) {
            fprintf(stderr, "[regression] no query banks found in %s\n", regression_dir);
            cleanup_globals(shared_store, ws, provider, nash_dir, props_json, server_model, cfg);
            return 1;
        }

        fprintf(stderr, "[regression] loaded %d query banks\n", n_banks);

        /* Run tests */
        regression_report_t *report = regression_run(
            banks, n_banks, regression_split,
            provider, cfg, memory, shared_store, nash_dir);

        regression_print_report(report);

        int exit_code = 0;

        /* Handle --validate-harness */
        if (validate_harness) {
            char baseline_path[NASH_PATH_MAX];
            snprintf(baseline_path, sizeof(baseline_path),
                     "%s/regression/baseline.json", nash_dir);

            if (strcmp(validate_harness, "baseline") == 0) {
                regression_save_report(report, baseline_path);
                fprintf(stderr, "[regression] baseline saved: %s\n", baseline_path);
            } else if (strcmp(validate_harness, "compare") == 0) {
                regression_report_t *baseline = regression_load_report(baseline_path);
                if (!baseline) {
                    fprintf(stderr, "[regression] no baseline found at %s\n", baseline_path);
                    fprintf(stderr, "[regression] run with --validate-harness baseline first\n");
                    exit_code = 2;
                } else {
                    exit_code = (int)regression_compare(baseline, report);
                    regression_free_report(baseline);
                }
            }
        }

        regression_free_report(report);
        regression_free_banks(banks, n_banks);
        cleanup_globals(shared_store, ws, provider, nash_dir, props_json, server_model, cfg);
        return exit_code;
    }

    /* ── GEPA Prompt Optimization mode: --optimize BUDGET ── */
    if (optimize_budget) {
        int rounds = optimize_parse_budget(optimize_budget);
        if (rounds < 0) {
            fprintf(stderr, "[optimize] invalid budget '%s' — use light, medium, heavy, or a number\n",
                    optimize_budget);
            cleanup_globals(shared_store, ws, provider, nash_dir, props_json, server_model, cfg);
            return 1;
        }

        /* Create regression directory and seed query banks */
        char regression_dir[NASH_PATH_MAX];
        snprintf(regression_dir, sizeof(regression_dir), "%s/regression", nash_dir);
        regression_write_seed(regression_dir);

        /* Load query banks */
        int n_banks = 0;
        query_bank_t *banks = regression_load_banks(regression_dir, &n_banks);
        if (!banks || n_banks == 0) {
            fprintf(stderr, "[optimize] no query banks found in %s\n", regression_dir);
            cleanup_globals(shared_store, ws, provider, nash_dir, props_json, server_model, cfg);
            return 1;
        }

        /* Create reflection provider (same as student by default) */
        provider_t *reflection_provider = provider;
        if (reflect_model_arg) {
            /* Parse reflect-model as "provider/model" e.g. "anthropic/claude-sonnet-4-20250514" */
            char *slash = strchr(reflect_model_arg, '/');
            if (slash) {
                char ptype[64];
                snprintf(ptype, sizeof(ptype), "%.*s", (int)(slash - reflect_model_arg), reflect_model_arg);
                const char *rmodel = slash + 1;
                provider_config_t rpcfg = {
                    .type           = provider_type_from_str(ptype),
                    .model_id       = rmodel,
                    .api_base       = cfg->provider.api_base,
                    .api_key_env    = cfg->provider.api_key_env,
                    .project_id     = cfg->provider.project_id,
                    .region         = cfg->provider.region,
                    .context_size   = cfg->provider.context_size,
                    .chars_per_token = cfg->provider.chars_per_token,
                    .max_tokens     = cfg->max_tokens,
                    .temperature    = 0.7f,  /* slightly creative for reflection */
                    .top_p          = 1.0f,   /* disabled for reflection */
                    .top_k          = 0,      /* disabled for reflection */
                    .enable_thinking = 0,
                    .thinking_budget = -1,
                    .llm_timeout     = cfg->llm_timeout,
                };
                reflection_provider = provider_create(&rpcfg);
                if (!reflection_provider) {
                    fprintf(stderr, "[optimize] failed to create reflection provider '%s'\n",
                            reflect_model_arg);
                    reflection_provider = provider;  /* fallback to student */
                }
            } else {
                fprintf(stderr, "[optimize] --reflect-model format: provider/model (e.g. anthropic/claude-sonnet-4-20250514)\n");
                fprintf(stderr, "[optimize] using student model as reflection model\n");
            }
        }

        /* Find model profile path for writing results */
        const char *profile_path = NULL;
        char profile_path_buf[NASH_PATH_MAX * 2];
        if (server_model) {
            const model_profile_t *profile = config_match_model(cfg, server_model);
            if (profile && profile->source_file) {
                snprintf(profile_path_buf, sizeof(profile_path_buf),
                         "%s/models/%s", nash_dir, profile->source_file);
                profile_path = profile_path_buf;
            }
        }

        /* Configure and run optimization */
        optimize_config_t opt = {
            .max_rounds       = rounds,
            .proposal_width   = 2,  /* Self-Harness K=2: two parallel proposals per round */
            .student          = provider,
            .reflection       = reflection_provider,
            .profile_path     = profile_path,
            .split_filter     = regression_split,
            .verbose          = 1,
            /* SkillOpt extensions [arXiv:2605.23904v2] */
            .n_epochs         = optimize_epochs,
            .edit_budget_init = optimize_edit_budget,
            .edit_budget_floor = optimize_edit_budget > 2 ? 2 : 1,
            .minibatch_size   = 0,  /* 0 = all-at-once (default) */
        };

        prompt_candidate_t best = optimize_run(
            &opt, banks, n_banks, cfg, memory, shared_store, nash_dir);

        /* Cleanup */
        optimize_free_candidate(&best);
        regression_free_banks(banks, n_banks);
        if (reflection_provider != provider)
            provider_free(reflection_provider);
        cleanup_globals(shared_store, ws, provider, nash_dir, props_json, server_model, cfg);
        return 0;
    }

    /* Validate playbook mode: --validate-playbook NAME */
    if (validate_playbook_arg) {
        char pb_path[NASH_PATH_MAX];
        if (strchr(validate_playbook_arg, '/') || strchr(validate_playbook_arg, '.')) {
            snprintf(pb_path, sizeof(pb_path), "%s", validate_playbook_arg);
        } else {
            snprintf(pb_path, sizeof(pb_path), "%s/playbooks/%s.yaml",
                     nash_dir, validate_playbook_arg);
        }
        playbook_t *pb = playbook_load(pb_path);
        if (!pb) {
            fprintf(stderr, "Error: cannot load playbook '%s'\n", pb_path);
            cleanup_globals(shared_store, ws, provider, nash_dir, props_json, server_model, cfg);
            return 1;
        }
        fprintf(stderr, "Validating playbook '%s' (%d passes) from %s\n",
                pb->name, pb->n_passes, pb_path);
        char errbuf[4096];
        int rc = playbook_validate(pb, errbuf, sizeof(errbuf));
        fprintf(stderr, "%s", errbuf);
        playbook_free(pb);
        cleanup_globals(shared_store, ws, provider, nash_dir, props_json, server_model, cfg);
        return rc == 0 ? 0 : 1;
    }

    /* Headless playbook mode: --play NAME */
    if (play_arg) {
        /* Resolve playbook path */
        char pb_path[NASH_PATH_MAX];
        if (strchr(play_arg, '/') || strchr(play_arg, '.')) {
            snprintf(pb_path, sizeof(pb_path), "%s", play_arg);
        } else {
            playbook_resolve(play_arg, nash_dir, pb_path, sizeof(pb_path));
        }

        playbook_t *pb = playbook_load(pb_path);
        if (!pb) {
            fprintf(stderr, "Error: cannot load playbook '%s'\n", pb_path);
            cleanup_globals(shared_store, ws, provider, nash_dir, props_json, server_model, cfg);
            return 1;
        }

        /* Validate before running */
        {
            char vbuf[4096];
            if (playbook_validate(pb, vbuf, sizeof(vbuf)) != 0) {
                fprintf(stderr, "[play] Playbook validation failed:\n%s", vbuf);
                playbook_free(pb);
                cleanup_globals(shared_store, ws, provider, nash_dir, props_json, server_model, cfg);
                return 1;
            }
        }

        fprintf(stderr, "[play] Running playbook '%s' (%d passes)\n",
                pb->name, pb->n_passes);

        playbook_args_t pargs = {
            .playbook = pb,
            .nash_dir = nash_dir,
            .store = shared_store,
            .memory = memory,
            .cfg = cfg,
            .provider = provider,
            .consolidation_provider = g_consolidation_provider,
            .server_model = server_model,
            .ui = NULL,  /* headless — no TUI */
            .playbook_ok = 0,
            .done = 0,
        };

        /* Run synchronously (no thread needed in headless mode) */
        playbook_worker(&pargs);

        int ok = pargs.playbook_ok;
        fprintf(stderr, "[play] Playbook '%s' %s\n",
                pb->name, ok ? "completed successfully" : "FAILED");

        /* Route result to outbox if a daemon (--matrix/--telegram) is running */
        route_to_outbox(nash_dir, pargs.result_text,
                        ws && ws->name ? ws->name : NULL, pb->name);

        free(pargs.result_text);
        free(pargs.last_session_dir);
        playbook_free(pb);
        cleanup_globals(shared_store, ws, provider, nash_dir, props_json, server_model, cfg);
        return ok ? 0 : 1;
    }

    /* Agent mode: scan workspaces, build calendar, run agents.
     * When --agent ID is used interactively (TTY), route through the TUI
     * for streaming output + tool call visualization instead of headless. */
    int agent_tui_mode = (agents_mode && agent_target_id &&
                          !agents_due && !agents_dry_run && isatty(STDOUT_FILENO));
    if (agents_mode && !agent_tui_mode) {
        /* Default to listing agents when no sub-command given */
        if (!agents_due && !agents_list && !agents_dry_run && !agent_target_id) {
            agents_list = 1;
        }

        /* Acquire lock (separate from daemon lock -- uses agent.lock) */
        {
            char lock_path[NASH_PATH_MAX];
            snprintf(lock_path, sizeof(lock_path), "%s/agent", nash_dir);
            mkdir(lock_path, 0755);
        }

        /* Install signal handlers for graceful shutdown */
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = shutdown_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGTERM, &sa, NULL);
        sigaction(SIGINT, &sa, NULL);

        /* Scan + schedule */
        agent_queue_t *q = agent_scan(nash_dir);
        if (!q) {
            fprintf(stderr, "[agent] error: scan failed\n");
            free(agent_arguments);
            cleanup_globals(shared_store, ws, provider, nash_dir, props_json, server_model, cfg);
            return 1;
        }
        agent_queue_load(q, nash_dir);
        agent_queue_schedule(q, time(NULL));

        if (agents_list) {
            agent_queue_print(q, stdout);
            agent_queue_free(q);
            free(agent_arguments);
            cleanup_globals(shared_store, ws, provider, nash_dir, props_json, server_model, cfg);
            return 0;
        }

        if (agents_dry_run) {
            fprintf(stderr, "[agent] DRY RUN -- would execute:\n");
            int n = 0;
            for (int i = 0; i < q->n_agents; i++) {
                if (!q->agents[i].is_due) continue;
                n++;
                char tstr[32] = "never run";
                if (q->agents[i].last_run > 0) {
                    int ago = (int)(time(NULL) - q->agents[i].last_run);
                    if (ago < 3600) snprintf(tstr, sizeof(tstr), "%dm ago", ago / 60);
                    else snprintf(tstr, sizeof(tstr), "%dh ago", ago / 3600);
                }
                fprintf(stderr, "  %d. %-40s (due: %s, timeout: %ds)\n",
                        n, q->agents[i].id, tstr, q->agents[i].timeout);
            }
            if (n == 0) fprintf(stderr, "  (no agent definitions due)\n");
            agent_queue_free(q);
            free(agent_arguments);
            cleanup_globals(shared_store, ws, provider, nash_dir, props_json, server_model, cfg);
            return 0;
        }

        /* Execute agents (re-scans internally to own the queue) */
        agent_queue_free(q);
        /* If a daemon (--matrix/--telegram) is running, route agent results
         * through its mailbox outbox so the bridge can deliver them. */
        char mbox_dir[NASH_PATH_MAX];
        const char *mbox = NULL;
        if (mailbox_init(nash_dir, mbox_dir, sizeof(mbox_dir)) == 0)
            mbox = mbox_dir;
        int n_fail = agent_run_due(nash_dir, shared_store, cfg, provider,
                                   server_model, agent_target_id,
                                   agent_arguments,
                                   &shutdown_requested, mbox);
        free(agent_arguments);
        cleanup_globals(shared_store, ws, provider, nash_dir, props_json, server_model, cfg);
        return n_fail > 0 ? 1 : 0;
    }

    /* Daemon mode: watch mailbox inbox for tasks, process them sequentially */
    if (daemon_mode) {
        /* Prevent multiple daemons sharing the same mailbox/room */
        if (daemon_lock_acquire(nash_dir) != 0) {
            cleanup_globals(shared_store, ws, provider, nash_dir, props_json, server_model, cfg);
            return 1;
        }
        char mbox_dir[NASH_PATH_MAX];
        if (mailbox_init(nash_dir, mbox_dir, sizeof(mbox_dir)) != 0) {
            fprintf(stderr, "[error] failed to initialize mailbox\n");
            cleanup_globals(shared_store, ws, provider, nash_dir, props_json, server_model, cfg);
            return 1;
        }
        fprintf(stderr, "[daemon] nash mailbox daemon started\n");
        fprintf(stderr, "[daemon] inbox: %s/inbox/  (drop task_* files here)\n", mbox_dir);
        fprintf(stderr, "[daemon] outbox: %s/outbox/ (results appear here)\n", mbox_dir);

        /* FIX #6: Install signal handlers for graceful daemon shutdown.
         * SIGTERM/SIGINT set shutdown_requested; the loop checks it
         * each iteration so in-progress tasks complete before exit.
         * Use sigaction WITHOUT SA_RESTART so that blocking calls
         * (fgets, poll, read) return with EINTR on Ctrl-C. */
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = shutdown_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;  /* no SA_RESTART — let syscalls fail with EINTR */
        sigaction(SIGTERM, &sa, NULL);
        sigaction(SIGINT, &sa, NULL);

        /* Telegram bridge: start thread that bridges mailbox ↔ Telegram API */
        pthread_t tg_thread = 0;
        telegram_ctx_t tg_ctx;
        if (telegram_mode) {
            telegram_init(&tg_ctx, config_path, nash_dir, mbox_dir, &shutdown_requested);
            if (!tg_ctx.bot_token || !tg_ctx.chat_id) {
                /* No config — run interactive setup */
                if (telegram_setup(&tg_ctx) != 0) {
                    fprintf(stderr, "[telegram] setup failed, exiting\n");
                    telegram_free(&tg_ctx);
                    cleanup_globals(shared_store, ws, provider, nash_dir, props_json, server_model, cfg);
                    return 1;
                }
            }
            pthread_create(&tg_thread, NULL, telegram_run, &tg_ctx);
            fprintf(stderr, "[telegram] bot bridge active — send messages to your bot\n");
        }

        /* Matrix bridge: start thread that bridges mailbox ↔ Matrix API */
        pthread_t mx_thread = 0;
        matrix_ctx_t mx_ctx;
        if (matrix_mode) {
            matrix_init(&mx_ctx, config_path, nash_dir, mbox_dir, &shutdown_requested);
            if (!mx_ctx.access_token || !mx_ctx.room_id) {
                /* No config — run interactive setup */
                if (matrix_setup(&mx_ctx) != 0) {
                    fprintf(stderr, "[matrix] setup failed, exiting\n");
                    matrix_free(&mx_ctx);
                    cleanup_globals(shared_store, ws, provider, nash_dir, props_json, server_model, cfg);
                    return 1;
                }
            }
            pthread_create(&mx_thread, NULL, matrix_run, &mx_ctx);
            fprintf(stderr, "[matrix] bot bridge active — send messages to the Matrix room\n");
        }

        /* ── Workspace context pool for daemon mode ────────────────────
         * Maintains a pool of workspace sessions keyed by workspace name.
         * Each workspace has its own session_dir, journal, tool_ctx, react_ctx.
         * Tasks arriving with X-Workspace headers are routed to the
         * appropriate workspace slot.  Slots are lazily created on first
         * message to a workspace.
         *
         * When no workspace is specified (legacy or global), the default
         * slot (index 0, ws_name from cfg->workspace) is used.
         */
        #define DAEMON_MAX_WS_SLOTS 16
        typedef struct {
            char        *name;          /* workspace name (NULL = global) */
            workspace_t *ws;
            memory_t    *mem;           /* ws->global shortcut */
            char        *session_dir;
            journal_t   *journal;
            tool_ctx_t   tools;
            react_ctx_t  react;
            int          active;        /* 1 = initialized */
        } daemon_ws_slot_t;

        daemon_ws_slot_t ws_pool[DAEMON_MAX_WS_SLOTS];
        memset(ws_pool, 0, sizeof(ws_pool));
        int ws_pool_count = 0;

        /* Initialize default slot (from cfg->workspace or global) */
        {
            daemon_ws_slot_t *s = &ws_pool[0];
            s->name = cfg->workspace ? strdup(cfg->workspace) : NULL;
            s->ws = ws;  /* reuse the ws already created at L655 */
            s->mem = ws ? ws->global : NULL;
            s->session_dir = create_session_dir(nash_dir, cfg->workspace);
            s->journal = journal_new(s->session_dir);
            session_init_tools(&s->tools, shared_store, s->journal, s->mem,
                               s->ws, s->session_dir, cfg, provider);
            session_init_react(&s->react, provider, g_planner_provider, g_reflection_provider, &s->tools, cfg);
            s->active = 1;
            ws_pool_count = 1;
            fprintf(stderr, "[daemon] default session: %s (workspace: %s)\n",
                    s->session_dir, s->name ? s->name : "global");
        }

        while (!shutdown_requested) {
            /* ── Check for session reset command (cmd_new in inbox) ──── */
            {
                char cmd_path[NASH_PATH_MAX];
                snprintf(cmd_path, sizeof(cmd_path), "%s/inbox/cmd_new", mbox_dir);
                if (access(cmd_path, F_OK) == 0) {
                    unlink(cmd_path);
                    fprintf(stderr, "\n[daemon] === session reset (all slots) ===\n");

                    /* Reset all active slots */
                    for (int si = 0; si < ws_pool_count; si++) {
                        daemon_ws_slot_t *s = &ws_pool[si];
                        if (!s->active) continue;
                        if (s->tools.scratch.count > 0)
                            scratchpad_save(&s->tools.scratch, s->session_dir);
                        free(s->react.last_query);  s->react.last_query = NULL;
                        free(s->react.last_result); s->react.last_result = NULL;
                        session_cleanup(&s->tools, &s->react, s->journal);
                        if (is_dir_empty(s->session_dir))
                            rmdir(s->session_dir);
                        free(s->session_dir);
                        s->session_dir = create_session_dir(nash_dir, s->name);
                        s->journal = journal_new(s->session_dir);
                        session_init_tools(&s->tools, shared_store, s->journal,
                                           s->mem, s->ws, s->session_dir,
                                           cfg, provider);
                        session_init_react(&s->react, provider, g_planner_provider, g_reflection_provider, &s->tools, cfg);
                        fprintf(stderr, "[daemon] reset slot '%s': %s\n",
                                s->name ? s->name : "global", s->session_dir);
                    }
                    continue;
                }
            }

            /* Wait for next task (with workspace metadata).
             * Timeout allows periodic agent scheduling checks. */
            mailbox_task_t *task = mailbox_wait_task_ex(mbox_dir, 60);
            if (!task) {
                if (shutdown_requested) break;
                char cmd_chk[NASH_PATH_MAX];
                snprintf(cmd_chk, sizeof(cmd_chk), "%s/inbox/cmd_new", mbox_dir);
                if (access(cmd_chk, F_OK) != 0) {
                    /* No command pending -- check for due agents */
                    agent_run_due(nash_dir, shared_store, cfg,
                                  provider, server_model,
                                  NULL, NULL,
                                  &shutdown_requested, mbox_dir);
                }
                continue;
            }

            /* Check again for session reset (may have arrived while waiting) */
            {
                char cmd_path[NASH_PATH_MAX];
                snprintf(cmd_path, sizeof(cmd_path), "%s/inbox/cmd_new", mbox_dir);
                if (access(cmd_path, F_OK) == 0) {
                    unlink(cmd_path);
                    fprintf(stderr, "\n[daemon] === session reset (during wait) ===\n");
                    for (int si = 0; si < ws_pool_count; si++) {
                        daemon_ws_slot_t *s = &ws_pool[si];
                        if (!s->active) continue;
                        if (s->tools.scratch.count > 0)
                            scratchpad_save(&s->tools.scratch, s->session_dir);
                        free(s->react.last_query);  s->react.last_query = NULL;
                        free(s->react.last_result); s->react.last_result = NULL;
                        session_cleanup(&s->tools, &s->react, s->journal);
                        if (is_dir_empty(s->session_dir))
                            rmdir(s->session_dir);
                        free(s->session_dir);
                        s->session_dir = create_session_dir(nash_dir, s->name);
                        s->journal = journal_new(s->session_dir);
                        session_init_tools(&s->tools, shared_store, s->journal,
                                           s->mem, s->ws, s->session_dir,
                                           cfg, provider);
                        session_init_react(&s->react, provider, g_planner_provider, g_reflection_provider, &s->tools, cfg);
                    }
                }
            }

            /* Find or create workspace slot for this task */
            const char *task_ws = task->workspace;  /* NULL = default */
            daemon_ws_slot_t *slot = NULL;

            /* Look for existing slot */
            for (int si = 0; si < ws_pool_count; si++) {
                daemon_ws_slot_t *s = &ws_pool[si];
                if (!s->active) continue;
                /* Match: both NULL (global), or same name */
                if (!task_ws && !s->name) { slot = s; break; }
                if (task_ws && s->name && strcmp(task_ws, s->name) == 0) {
                    slot = s; break;
                }
            }

            /* Create new slot if needed */
            if (!slot && ws_pool_count < DAEMON_MAX_WS_SLOTS) {
                daemon_ws_slot_t *s = &ws_pool[ws_pool_count];
                s->name = task_ws ? strdup(task_ws) : NULL;
                int ws_iso = cfg->workspace_isolated ||
                             (cfg->workspace_global_recall == 0);
                double gw = cfg->workspace_global_weight;
                s->ws = workspace_new(nash_dir, s->name, ws_iso, gw);
                s->mem = s->ws ? s->ws->global : NULL;
                if (server_model && s->mem)
                    s->mem->model = strdup(server_model);
                if (server_model && s->ws && s->ws->workspace)
                    s->ws->workspace->model = strdup(server_model);
                workspace_set_recall_config(s->ws, cfg->recall_min_score,
                                            cfg->recall_blend_semantic,
                                            cfg->recall_blend_substring,
                                            cfg->vscore_exponent);
                s->session_dir = create_session_dir(nash_dir, s->name);
                s->journal = journal_new(s->session_dir);
                session_init_tools(&s->tools, shared_store, s->journal,
                                   s->mem, s->ws, s->session_dir,
                                   cfg, provider);
                session_init_react(&s->react, provider, g_planner_provider, g_reflection_provider, &s->tools, cfg);
                s->active = 1;
                ws_pool_count++;
                slot = s;
                fprintf(stderr, "[daemon] created workspace slot '%s': %s\n",
                        s->name ? s->name : "global", s->session_dir);
            }

            if (!slot) {
                /* Pool full — use default slot */
                slot = &ws_pool[0];
                fprintf(stderr, "[daemon] workspace pool full, using default\n");
            }

            fprintf(stderr, "\n[daemon] === task: %s (R%d, ws=%s) ===\n",
                    task->task_id ? task->task_id : "unknown",
                    slot->tools.react_loop,
                    slot->name ? slot->name : "global");
            fprintf(stderr, "[daemon] query: %.200s%s\n", task->query,
                    strlen(task->query) > 200 ? "..." : "");

            mailbox_ctx_t mbox = {
                .react_ctx = &slot->react,
                .mailbox_dir = mbox_dir,
                .session_dir = slot->session_dir,
                .timeout_sec = mailbox_timeout,
            };

            char *result = react_run(&slot->react, task->query,
                                     mailbox_on_event, &mbox);

            /* Write result to outbox (with route token for bridge routing) */
            if (task->task_id) {
                mailbox_write_result_routed(mbox_dir, task->task_id,
                                           result, task->route_token,
                                           task->workspace, task->query);
            }

            if (result) {
                fprintf(stderr, "[daemon] task %s completed (R%d)\n",
                        task->task_id ? task->task_id : "unknown",
                        slot->tools.react_loop);
                printf("%s\n", result);
            } else {
                fprintf(stderr, "[daemon] task %s failed (no result)\n",
                        task->task_id ? task->task_id : "unknown");
            }

            /* Propagate context to next react loop (like TUI does) */
            free(slot->react.last_query);
            free(slot->react.last_result);
            slot->react.last_query = strdup(task->query);
            slot->react.last_result = result ? strdup(result) : NULL;
            slot->tools.react_loop++;

            /* Tier 1 dreaming */
            memory_prune(slot->mem, cfg->prune_min_score,
                         cfg->prune_min_evidence);

            free(result);
            mailbox_task_free(task);
        }

        /* Graceful shutdown — cleanup all workspace slots */
        fprintf(stderr, "[daemon] shutting down...\n");
        for (int si = 0; si < ws_pool_count; si++) {
            daemon_ws_slot_t *s = &ws_pool[si];
            if (!s->active) continue;
            if (s->tools.scratch.count > 0)
                scratchpad_save(&s->tools.scratch, s->session_dir);
            free(s->react.last_query);  s->react.last_query = NULL;
            free(s->react.last_result); s->react.last_result = NULL;
            session_cleanup(&s->tools, &s->react, s->journal);
            if (is_dir_empty(s->session_dir))
                rmdir(s->session_dir);
            free(s->session_dir);
            /* Don't free ws_pool[0].ws — it's the shared 'ws' freed later */
            if (si > 0 && s->ws) {
                workspace_free(s->ws);
            }
            free(s->name);
        }
        if (telegram_mode && tg_thread) {
            pthread_join(tg_thread, NULL);
            telegram_free(&tg_ctx);
        }
        if (matrix_mode && mx_thread) {
            pthread_join(mx_thread, NULL);
            matrix_free(&mx_ctx);
        }
        daemon_lock_release();
        web_search_cleanup();
        cleanup_globals(shared_store, ws, provider, nash_dir, props_json, server_model, cfg);
        return 0;
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
            journal = journal_new_lazy(nash_dir, cfg->workspace);
            lazy_session = 1;
        } else {
            journal = journal_new(session_dir);
        }
        tool_ctx_t tools;
        session_init_tools(&tools, shared_store, journal, memory,
                           ws, session_dir, cfg, provider);
        react_ctx_t react;
        session_init_react(&react, provider, g_planner_provider, g_reflection_provider, &tools, cfg);

        /* Mailbox mode: use mailbox_on_event to handle user_ask via files */
        char *result;
        if (mailbox_mode) {
            char mbox_dir[NASH_PATH_MAX];
            if (mailbox_init(nash_dir, mbox_dir, sizeof(mbox_dir)) != 0) {
                fprintf(stderr, "[error] failed to initialize mailbox\n");
                session_cleanup(&tools, &react, journal);
                if (session_dir) free(session_dir);
                cleanup_globals(shared_store, ws, provider, nash_dir, props_json, server_model, cfg);
                return 1;
            }
            mailbox_ctx_t mbox = {
                .react_ctx = &react,
                .mailbox_dir = mbox_dir,
                .session_dir = session_dir,
                .timeout_sec = mailbox_timeout,
            };
            fprintf(stderr, "[mailbox] enabled — questions in %s/outbox/, answers in %s/inbox/\n",
                    mbox_dir, mbox_dir);
            /* Forward query to bridge before processing */
            route_query_to_outbox(nash_dir, query,
                                  ws && ws->name ? ws->name : NULL, NULL, 1);
            result = react_run(&react, query, mailbox_on_event, &mbox);
        } else {
            /* Forward query to bridge before processing */
            route_query_to_outbox(nash_dir, query,
                                  ws && ws->name ? ws->name : NULL, NULL, 1);
            result = react_run(&react, query, tui_on_event, NULL);
        }
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
        /* Route result to outbox if a daemon (--matrix/--telegram) is running */
        route_to_outbox(nash_dir, result,
                        ws && ws->name ? ws->name : NULL, NULL);
        if (result) { printf("%s\n", result); free(result); }
        /* Save scratchpad if session was created */
        if (session_dir && tools.scratch.count > 0) {
            scratchpad_save(&tools.scratch, session_dir);
        }
        tools.react_loop++;  /* increment for next query */
        session_cleanup(&tools, &react, journal);
        /* Remove session directory if it's empty (no work was done) */
        if (session_dir && is_dir_empty(session_dir)) {
            rmdir(session_dir);
        }
        if (session_dir) free(session_dir);
        cleanup_globals(shared_store, ws, provider, nash_dir, props_json, server_model, cfg);
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
            session_dir = create_session_dir(nash_dir, cfg->workspace);
        }
        journal_t *journal = journal_new(session_dir);
        tool_ctx_t tools;
        session_init_tools(&tools, shared_store, journal, memory,
                           ws, session_dir, cfg, provider);
        react_ctx_t react;
        session_init_react(&react, provider, g_planner_provider, g_reflection_provider, &tools, cfg);

        /* Initialize logging subsystem for TUI error routing */
        nash_log_init(journal, shared_store);
        nash_log_set_tools(&tools);

        /* Create UI state and initialize TUI */
        ui_state_t *ui = ui_state_new(session_dir, shared_store);
        ui->nash_dir = strdup(nash_dir);  /* for /? cross-session search */
        if (ws && ws->name)
            ui->workspace_name = strdup(ws->name);  /* for status bar breadcrumb */
        /* Pass model name + context info for nashell-style status bar */
        if (server_model)
            ui->model_name = strdup(server_model);
        ui->context_size = context_size;
        ui->context_used = 0;
        ui->pause_flag = &react.pause_requested;  /* Space → pause react loop */
        ui->abort_flag = &provider->abort_retry;    /* abort in-progress HTTP call */
        ui->bg_jobs = 0;
        /* Show dream reminder in status bar if threshold exceeded */
        if (cfg->dream_reminder_threshold > 0 && dream_new_count >= cfg->dream_reminder_threshold) {
            char dream_msg[256];
            snprintf(dream_msg, sizeof(dream_msg),
                     "⚠ %d new memories — /dream to consolidate", dream_new_count);
            ui_state_set_status(ui, STATUS_READY, dream_msg);
        } else {
            ui_state_set_status(ui, STATUS_READY, "Ready");
        }
        /* Set banner text for main pane */
        char *banner = build_banner_string(cfg, props_json, nash_dir, session_dir, matched_profile_file);
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
        /* State machine: 0=idle (main thread only), 1=inference running,
         * 3=playbook running. Values 1 and 3 mean infer_tid is joinable.
         * FIX #13: Made atomic for defense in depth — currently only the
         * main thread reads/writes, but atomic_int prevents data races
         * if future code accesses it from another thread. */
        atomic_int inferring = INFER_IDLE;
        pthread_t infer_tid;
        /* Thread-shared args: static lifetime so they survive across loop
         * iterations.  Thread ownership contract:
         *   Written by main thread BEFORE pthread_create (happens-before).
         *   Read by inference thread during react_run.
         *   .done is atomic_int — polled by main thread, set by infer thread.
         *   Main thread only touches these again AFTER pthread_join. */
        static infer_args_t iargs;
        static playbook_args_t pargs_tui;
        char *pending_redirect = NULL;  /* stashed query when user types during inference */

        /* Auto-dispatch agent when launched via --agent ID (interactive TUI mode) */
        if (agent_tui_mode) {
            char agent_cmd[1024];
            snprintf(agent_cmd, sizeof(agent_cmd), "/agent run %s%s%s",
                     agent_target_id,
                     agent_arguments ? " " : "",
                     agent_arguments ? agent_arguments : "");
            char *auto_cmd = strdup(agent_cmd);
            command_ctx_t cmd_ctx = {
                .session_dir  = &session_dir,
                .nash_dir     = nash_dir,
                .tools        = &tools,
                .react        = &react,
                .ui           = ui,
                .journal      = &journal,
                .provider     = provider,
                .consolidation_provider = g_consolidation_provider,
                .cfg          = cfg,
                .store        = shared_store,
                .memory       = memory,
                .ws           = ws,
                .server_model = server_model,
                .inferring    = &inferring,
                .infer_tid    = &infer_tid,
                .pargs        = &pargs_tui,
            };
            command_dispatch(&cmd_ctx, &auto_cmd);
            free(auto_cmd);  /* in case dispatch didn't consume it */
            free(agent_arguments);
            agent_arguments = NULL;
        }

        while (running) {
            /* Check if playbook thread completed */
            if (inferring == INFER_PLAYBOOK && pargs_tui.done) {
                pthread_join(infer_tid, NULL);

                /* Log to agent history if this was an /agent run */
                if (pargs_tui.agent_id) {
                    int dur = (int)(time(NULL) - pargs_tui.agent_start_time);
                    const char *status = pargs_tui.playbook_ok ? "ok" : "fail";
                    agent_entry_t tmp_agent = { .id = pargs_tui.agent_id };
                    agent_history_append(pargs_tui.nash_dir, &tmp_agent,
                                         dur, status, NULL);
                    agent_queue_update_run(pargs_tui.nash_dir,
                                           pargs_tui.agent_id,
                                           pargs_tui.agent_start_time,
                                           dur, status);
                    if (pargs_tui.last_session_dir)
                        agent_save_result(pargs_tui.nash_dir,
                                          pargs_tui.agent_id,
                                          pargs_tui.last_session_dir);
                    free(pargs_tui.agent_id);
                    pargs_tui.agent_id = NULL;
                    free(pargs_tui.workspace_override);
                    pargs_tui.workspace_override = NULL;
                    workspace_free(pargs_tui.agent_ws);
                    pargs_tui.agent_ws = NULL;
                }

                /* Route result to outbox if a daemon (--matrix/--telegram) is running */
                route_to_outbox(nash_dir, pargs_tui.result_text,
                                ws && ws->name ? ws->name : NULL,
                                pargs_tui.playbook ? pargs_tui.playbook->name : NULL);

                pthread_mutex_lock(&ui->mtx);
                if (pargs_tui.playbook_ok) {
                    char done_msg[256];
                    snprintf(done_msg, sizeof(done_msg), "Playbook '%s' complete (%d passes)",
                             pargs_tui.playbook->name, pargs_tui.playbook->n_passes);
                    ui_state_set_status(ui, STATUS_DONE, done_msg);
                } else {
                    ui_state_set_status(ui, STATUS_ERROR, "Playbook failed");
                }
                /* Agent finished — clear gates so session.md regen resumes.
                 * The user stays on the agent's reactRX.md until they press
                 * Escape, but session.md must be regenerable for when they do. */
                ui->agent_view = 0;
                ui->agent_running = 0;
                ui_state_generate_session_md(ui);
                pthread_mutex_unlock(&ui->mtx);
                free(pargs_tui.result_text);
                pargs_tui.result_text = NULL;
                free(pargs_tui.last_session_dir);
                pargs_tui.last_session_dir = NULL;
                playbook_free(pargs_tui.playbook);
                pargs_tui.playbook = NULL;
                inferring = INFER_IDLE;
                tui_render(ui);
            }

            /* Check if inference thread is paused and waiting for redirect.
             * Update status bar so user knows they can type a new query. */
            if (inferring == INFER_REACT && react.pause_waiting &&
                ui->status != STATUS_READY) {
                pthread_mutex_lock(&ui->mtx);
                ui_state_set_status(ui, STATUS_READY,
                    "Paused (type query to redirect, Space to resume)");
                pthread_mutex_unlock(&ui->mtx);
                tui_render(ui);
            }

            /* Check if inference thread completed */
            if (inferring == INFER_REACT && iargs.done) {
                pthread_join(infer_tid, NULL);
                inferring = INFER_IDLE;
                tools.react_loop++;  /* increment for next query */
                /* Tier 1 dreaming: deterministic Bayesian pruning after every react loop */
                memory_prune(memory, cfg->prune_min_score, cfg->prune_min_evidence);
                char *result = iargs.result;
                /* Route result to outbox if a daemon (--matrix/--telegram) is running */
                route_to_outbox(nash_dir, result,
                                ws && ws->name ? ws->name : NULL, NULL);
                pthread_mutex_lock(&ui->mtx);
                if (result) {
                    ui_state_set_status(ui, STATUS_DONE, "Done");
                    /* Refresh journal view to show completed query */
                    ui_state_load_journal(ui, journal);
                } else if (react.pause_requested && !pending_redirect) {
                    /* FIX #3: Only enter pause path if no redirect is pending.
                     * Race condition: if user types while inference is finishing,
                     * the main thread may set pause_requested=1 after the inference
                     * thread has already completed (TOCTOU on iargs.done).
                     * When pending_redirect is set, the user intended to start a
                     * new query, not pause — so skip the pause path and let the
                     * redirect be dispatched on the next iteration (line 1201). */
                    react.pause_requested = 0;  /* reset for next run */
                    react.paused = 1;
                    ui_state_set_status(ui, STATUS_READY,
                        "Paused (Space to resume, type query to redirect)");
                } else {
                    /* Clear stale pause_requested if redirect will take over */
                    react.pause_requested = 0;
                    if (pending_redirect) {
                        ui_state_set_status(ui, STATUS_READY, "Redirecting...");
                    } else if (ui->status != STATUS_ERROR) {
                        /* Only set generic "No result" if a more descriptive
                         * error wasn't already surfaced by REACT_EVENT_ERROR */
                        ui_state_set_status(ui, STATUS_ERROR, "No result");
                    }
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
                /* Only reset to Ready on success.  On error (result==NULL)
                 * preserve the STATUS_ERROR so the user actually sees it. */
                if (react.last_result)
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

            /* Auto-dispatch stashed redirect: when inference was paused
             * by user input (pending_redirect != NULL) and either:
             * (a) the thread has joined (!inferring), or
             * (b) the thread is paused and waiting on the condvar.
             * In both cases, inject the stashed query immediately. */
            if (!submitted_query && pending_redirect &&
                (!inferring || react.pause_waiting)) {
                submitted_query = pending_redirect;
                pending_redirect = NULL;
            }

            if (submitted_query) {
                /* Check if inference thread is waiting for user_ask answer */
                if (inferring && react.user_ask_pending) {
                    /* Pass the user's answer to the waiting react loop.
                     * All writes to shared state (user_ask_answer, user_ask_pending)
                     * are done inside the mutex to ensure proper happens-before
                     * ordering with the inference thread's condvar wait. */
                    pthread_mutex_lock(&react.user_ask_mutex);
                    free(react.user_ask_answer);
                    react.user_ask_answer = submitted_query;
                    submitted_query = NULL;  /* ownership transferred */
                    react.user_ask_pending = 0;  /* unblock the react loop */
                    pthread_cond_signal(&react.user_ask_cond);
                    pthread_mutex_unlock(&react.user_ask_mutex);
                    pthread_mutex_lock(&ui->mtx);
                    ui_state_set_status(ui, STATUS_RUNNING, "Running...");
                    pthread_mutex_unlock(&ui->mtx);
                    tui_render(ui);
                    continue;
                }

                /* Check if inference thread is paused and waiting for redirect */
                if (inferring && react.pause_waiting) {
                    /* Pass the user's redirect query to the waiting react loop.
                     * This preserves the full chat context (no new react_run). */
                    pthread_mutex_lock(&react.pause_mutex);
                    free(react.pause_query);
                    react.pause_query = submitted_query;
                    submitted_query = NULL;  /* ownership transferred */
                    pthread_cond_signal(&react.pause_cond);
                    pthread_mutex_unlock(&react.pause_mutex);
                    pthread_mutex_lock(&ui->mtx);
                    ui_state_set_status(ui, STATUS_RUNNING, "Resuming...");
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

                /* FIX #4: Guard slash commands that access shared inference state.
                 * During inference, only user_ask, exit/quit, and regular queries
                 * (which get stashed as pending_redirect) are safe. Slash commands
                 * like /fork read aliases->next_seq which is written by the inference
                 * thread without synchronization. Defer them until inference completes. */
                if (inferring && submitted_query[0] == '/'
                    && strncmp(submitted_query, "/quit", 5) != 0
                    && strncmp(submitted_query, "/exit", 5) != 0) {
                    /* Stash as pending_redirect — will execute after join */
                    free(pending_redirect);
                    pending_redirect = submitted_query;
                    submitted_query = NULL;
                    react.pause_requested = 1;
                    provider->abort_retry = 1;  /* wake provider_sleep early */
                    pthread_mutex_lock(&ui->mtx);
                    ui_state_set_status(ui, STATUS_RUNNING,
                        "Pausing to handle command…");
                    pthread_mutex_unlock(&ui->mtx);
                    tui_render(ui);
                    continue;
                }

                /* Dispatch slash commands via command_dispatch */
                {
                    command_ctx_t cmd_ctx = {
                        .session_dir  = &session_dir,
                        .nash_dir     = nash_dir,
                        .tools        = &tools,
                        .react        = &react,
                        .ui           = ui,
                        .journal      = &journal,
                        .provider     = provider,
                        .consolidation_provider = g_consolidation_provider,
                        .cfg          = cfg,
                        .store        = shared_store,
                        .memory       = memory,
                        .ws           = ws,
                        .server_model = server_model,
                        .inferring    = &inferring,
                        .infer_tid    = &infer_tid,
                        .pargs        = &pargs_tui,
                    };
                    if (command_dispatch(&cmd_ctx, &submitted_query) == CMD_CONTINUE) {
                        continue;
                    }
                }
                /* submitted_query may have been modified by /continue */

                /* If paused, any query (typed or Space-resume) clears the paused flag.
                 * checkpoint_restore will inject the query into restored context. */
                if (react.paused) {
                    react.paused = 0;
                }

                /* Regular query — spawn inference in background thread */
                if (inferring) {
                    /* User typed while inference is running — auto-pause and
                     * stash the query.  When the react loop breaks at the next
                     * step boundary the stashed query is dispatched immediately,
                     * eliminating the two-step "Space then type" dance. */
                    free(pending_redirect);  /* replace any earlier stash */
                    pending_redirect = submitted_query;
                    submitted_query = NULL;  /* ownership transferred */
                    react.pause_requested = 1;
                    provider->abort_retry = 1;  /* wake provider_sleep early */
                    pthread_mutex_lock(&ui->mtx);
                    ui_state_set_status(ui, STATUS_RUNNING,
                        "Pausing after current step…");
                    pthread_mutex_unlock(&ui->mtx);
                    tui_render(ui);
                    continue;
                }
                /* ── Tree branching: determine parent_loop ── */
                int parent_loop = -1;  /* default: root */
                if (tools.react_loop > 0) {
                    int viewed_loop = -1;
                    pthread_mutex_lock(&ui->mtx);
                    const char *viewing = ui->current_filepath;
                    if (viewing) {
                        /* Find last '/' or use full string */
                        const char *base = strrchr(viewing, '/');
                        base = base ? base + 1 : viewing;
                        sscanf(base, "reactR%d.md", &viewed_loop);
                    }
                    pthread_mutex_unlock(&ui->mtx);
                    if (viewed_loop >= 0) {
                        parent_loop = viewed_loop;
                    } else {
                        /* Viewing session.md or something else → linear follow-up */
                        parent_loop = tools.react_loop - 1;
                    }
                }
                react.parent_loop = parent_loop;

                /* If branching (parent != latest completed loop), override
                 * result.md so [PREVIOUS RESULT] matches the branch point.
                 * Scratchpad filtering is done non-destructively in react.c
                 * at injection time (using parent_loop + journal ancestor chain). */
                int is_branch = (parent_loop >= 0 &&
                                 parent_loop != tools.react_loop - 1);
                if (is_branch) {
                    /* Override result.md with parent's result from scratchpad */
                    char sec_name[32];
                    snprintf(sec_name, sizeof(sec_name), "R%d_result", parent_loop);
                    int idx = scratchpad_find(&tools.scratch, sec_name);
                    if (idx >= 0) {
                        char rpath[NASH_PATH_MAX];
                        snprintf(rpath, sizeof(rpath), "%s/result.md", session_dir);
                        write_file(rpath, tools.scratch.sections[idx].content,
                                   strlen(tools.scratch.sections[idx].content));
                    }
                }

                /* Build the final query string — add branch context hint if branching */
                char *final_query;
                if (is_branch) {
                    /* Find parent's query text from journal for context */
                    char parent_query_text[256] = "";
                    char jpath2[NASH_PATH_MAX];
                    snprintf(jpath2, sizeof(jpath2), "%s/journal.jsonl", session_dir);
                    FILE *jf2 = fopen(jpath2, "r");
                    if (jf2) {
                        char jline2[NASH_LINE_MAX];
                        while (fgets(jline2, sizeof(jline2), jf2)) {
                            cJSON *entry = cJSON_Parse(jline2);
                            if (!entry) continue;
                            const char *jtool = cJSON_GetStringValue(
                                cJSON_GetObjectItem(entry, "tool"));
                            int rl = (int)cJSON_GetNumberValue(
                                cJSON_GetObjectItem(entry, "react_loop"));
                            if (jtool && strcmp(jtool, "query") == 0 && rl == parent_loop) {
                                cJSON *params = cJSON_GetObjectItem(entry, "params");
                                cJSON *text = params ? cJSON_GetObjectItem(params, "text") : NULL;
                                if (text && text->valuestring) {
                                    snprintf(parent_query_text, sizeof(parent_query_text),
                                             "%.250s", text->valuestring);
                                }
                                cJSON_Delete(entry);
                                break;
                            }
                            cJSON_Delete(entry);
                        }
                        fclose(jf2);
                    }
                    size_t fqlen = strlen(submitted_query) + 512;
                    final_query = malloc(fqlen);
                    if (final_query) {
                        snprintf(final_query, fqlen,
                            "[Branched from R%d: \"%s\"]\n%s",
                            parent_loop, parent_query_text, submitted_query);
                    } else {
                        final_query = strdup(submitted_query);
                    }
                } else {
                    final_query = strdup(submitted_query);
                }

                pthread_mutex_lock(&ui->mtx);
                ui_state_set_status(ui, STATUS_RUNNING, "Running...");
                ui_state_add_query(ui, submitted_query);
                ui->current_react_loop = tools.react_loop;
                pthread_mutex_unlock(&ui->mtx);
                tui_render(ui);
                iargs = (infer_args_t){
                    .react = &react, .query = final_query,
                    .ui = ui, .result = NULL, .done = 0,
                };
                provider->abort_retry = 0;  /* reset before new inference */
                /* Snapshot the file the user is viewing so the LLM gets
                 * context about what the user is looking at. */
                free(react.tui_viewing_file);
                react.tui_viewing_file = ui->current_filepath
                    ? strdup(ui->current_filepath) : NULL;
                free(submitted_query);  /* strdup'd into final_query; ui_state_add_query also strdup'd */
                /* Forward query to bridge for session threading */
                route_query_to_outbox(nash_dir, final_query,
                                      ws && ws->name ? ws->name : NULL, NULL,
                                      tools.react_loop == 0 ? 1 : 0);
                pthread_create(&infer_tid, NULL, infer_worker, &iargs);
                inferring = INFER_REACT;
                tui_render(ui);
            }

            /* Always render if dirty */
            if (ui->dirty) tui_render(ui);

            /* ── Deferred regeneration ─────────────────────────────
             * The inference thread's event handler sets needs_*_regen
             * flags instead of doing expensive file I/O under the mutex.
             * We perform the generation here on the main thread, keeping
             * mutex hold time bounded to short in-memory operations.
             *
             * Throttled to 100ms intervals to avoid redundant work when
             * multiple events fire in rapid succession. */
            if (inferring) {
                static struct timespec last_refresh = {0, 0};
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                long elapsed_ms = (now.tv_sec - last_refresh.tv_sec) * 1000
                                + (now.tv_nsec - last_refresh.tv_nsec) / 1000000;
                if (elapsed_ms >= 100) {
                    last_refresh = now;
                    /* Snapshot and clear the deferred flags under mutex.
                     * The file I/O (generate + reload) is done here on the
                     * main thread.  The key fix is that the INFERENCE thread's
                     * event handler no longer does expensive I/O under the
                     * mutex — it just sets these flags.  So even though we
                     * hold the mutex during generation, the inference thread
                     * is only blocked for its fast in-memory flag/state updates,
                     * not the other way around (which caused the TUI freeze). */
                    pthread_mutex_lock(&ui->mtx);
                    int do_react   = ui->needs_react_regen;
                    int do_session = ui->needs_session_regen;
                    int do_reload  = ui->needs_file_reload;
                    int cur_loop   = ui->current_react_loop;
                    /* Force periodic regen while inference is active so
                     * the spinner, elapsed-time counter, and progress
                     * indicators stay live.  Previously this only fired
                     * during tool_executing, leaving the spinner frozen
                     * during prompt processing, between token arrivals,
                     * and between last token and TOOL_START. */
                    if (ui->status == STATUS_RUNNING)
                        do_react = 1;
                    ui->needs_react_regen  = 0;
                    ui->needs_session_regen = 0;
                    ui->needs_file_reload  = 0;

                    if (do_react)
                        ui_state_generate_react_md(ui, cur_loop);
                    if (do_session)
                        ui_state_generate_session_md(ui);
                    if (do_react || do_session || do_reload) {
                        ui_state_reload_file(ui);
                        /* Request deferred auto-scroll — only when viewing the
                         * active react loop's file, not when user navigated
                         * elsewhere (e.g. session.md, a different reactRX.md) */
                        if (ui->doc && !ui->user_scrolled) {
                            const char *fp = ui->current_filepath;
                            if (fp) {
                                char expect[64];
                                snprintf(expect, sizeof(expect), "reactR%d.md", cur_loop);
                                const char *base = strrchr(fp, '/');
                                base = base ? base + 1 : fp;
                                if (strcmp(base, expect) == 0) {
                                    /* For playbook per-pass mode, also verify
                                     * we're in the correct session directory */
                                    int path_ok = 1;
                                    if (ui->playbook_session_dir) {
                                        char full[4096];
                                        snprintf(full, sizeof(full), "%s/%s",
                                                 ui->playbook_session_dir, expect);
                                        path_ok = (strcmp(fp, full) == 0);
                                    }
                                    if (path_ok)
                                        ui->needs_auto_scroll = 1;
                                }
                            }
                        }
                        ui->dirty = 1;
                    }
                    pthread_mutex_unlock(&ui->mtx);
                    tui_render(ui);
                }
            }
            { struct timespec ts = {0, 10000000}; nanosleep(&ts, NULL); }  /* 10ms */
        }

        /* If inference thread is paused on condvar, wake it up so it can exit.
         * Send a "quit" redirect that will cause the loop to take one more step
         * and then exit naturally (or we just signal to unblock it). */
        if (inferring && react.pause_waiting) {
            pthread_mutex_lock(&react.pause_mutex);
            free(react.pause_query);
            react.pause_query = strdup("quit");
            react.pause_requested = 0;  /* clear so loop doesn't re-pause */
            pthread_cond_signal(&react.pause_cond);
            pthread_mutex_unlock(&react.pause_mutex);
        }
        /* If inference thread is paused on user_ask condvar, unblock it too */
        if (inferring && react.user_ask_pending) {
            pthread_mutex_lock(&react.user_ask_mutex);
            free(react.user_ask_answer);
            react.user_ask_answer = strdup("(quit)");
            react.user_ask_pending = 0;
            pthread_cond_signal(&react.user_ask_cond);
            pthread_mutex_unlock(&react.user_ask_mutex);
        }
        if (inferring) {
            pthread_join(infer_tid, NULL);
            inferring = INFER_IDLE;
            free(iargs.result);
            free(iargs.query);
        }
        free(pending_redirect);  /* clean up any un-dispatched redirect */
        tui_shutdown();
        nash_log_set_ui(NULL);  /* disable TUI error routing */
        ui_state_free(ui);

        /* Save scratchpad */
        if (tools.scratch.count > 0) {
            scratchpad_save(&tools.scratch, session_dir);
        }
        session_cleanup(&tools, &react, journal);
        /* Remove session directory if it's empty (no work was done) */
        if (session_dir && is_dir_empty(session_dir)) {
            rmdir(session_dir);
        }
        if (session_dir) free(session_dir);
    }
    printf("Bye.\n");
    web_search_cleanup();  /* tear down auto-started SearXNG container */
    tool_plugin_cleanup();  /* dlclose any loaded external plugins */
    cleanup_globals(shared_store, ws, provider, nash_dir, props_json, server_model, cfg);
    return 0;
}
