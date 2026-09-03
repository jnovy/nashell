/* tool_subtask.c — Sub-task spawning: isolated child react_run() calls.
 *
 * Implements the "LLM-as-Code" DAG pattern (arXiv:2606.15874):
 * the parent's context grows by exactly 2 messages per sub-task call
 * (assistant action + tool result), regardless of how many steps the
 * child took internally.  The child's entire chat is freed inside
 * react_run() before control returns.
 *
 * Pattern follows playbook.c:562-652 (proven child context setup). */

#include "tools_internal.h"
#include "tool_plugin.h"
#include "react.h"
#include "scratchpad.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdatomic.h>

/* Global counter for unique subtask directory names */
static atomic_int subtask_counter = 0;

/* Maximum nesting depth - now configurable via subtask_max_depth config knob.
 * Compile-time absolute cap to prevent misconfiguration. */
#define SUBTASK_MAX_DEPTH_CAP 5

/* ── Subtask event forwarding (mirrors playbook.c pb_event_cb pattern) ── */
typedef struct {
  react_event_fn parent_cb; /* parent's event callback */
  void *parent_data;        /* parent's event userdata */
  const char *child_dir;    /* subtask session directory */
  const char *parent_dir;   /* parent's session directory */
  int parent_loop;          /* parent's react_loop number */
} subtask_event_ctx_t;

static void subtask_event_cb(const react_event_t *ev, void *userdata) {
  subtask_event_ctx_t *sctx = (subtask_event_ctx_t *)userdata;
  react_event_t enriched = *ev;           /* shallow copy */
  enriched.session_dir = sctx->child_dir; /* redirect TUI to subtask dir */
  enriched.pass_label = "subtask";        /* status bar label */
  enriched.pass_index = -1;               /* not a playbook pass */
  sctx->parent_cb(&enriched, sctx->parent_data);
}

tool_result_t tool_subtask(tool_ctx_t *ctx, cJSON *params) {
  TOOL_REQ_STR(params, "query", query);
  TOOL_OPT_STR(params, "context", context_level);

  /* Optional temperature override for the child's provider */
  double temp_override = -1.0;
  {
    cJSON *temp_node = cJSON_GetObjectItem(params, "temperature");
    if (temp_node && cJSON_IsNumber(temp_node))
      temp_override = temp_node->valuedouble;
  }

  /* Parse context inheritance level:
   *   "minimal"  - query only (no scratchpad, no memory)
   *   "standard" - scratchpad + INFORM (default, current behavior)
   *   "rich"     - scratchpad + INFORM + memory injection
   *   "critic"   - like rich, plus a critic system prompt that instructs
   *                the child to find flaws rather than solve the problem */
  enum { CTX_MINIMAL, CTX_STANDARD, CTX_RICH, CTX_CRITIC } ctx_mode = CTX_STANDARD;
  if (context_level) {
    if (strcmp(context_level, "minimal") == 0)
      ctx_mode = CTX_MINIMAL;
    else if (strcmp(context_level, "rich") == 0)
      ctx_mode = CTX_RICH;
    else if (strcmp(context_level, "critic") == 0)
      ctx_mode = CTX_CRITIC;
    else if (strcmp(context_level, "standard") != 0) {
      char err[128];
      snprintf(err, sizeof(err),
               "Invalid context level \"%s\" - must be minimal, standard, rich, or critic",
               context_level);
      return tools_make_error(err);
    }
  }

  /* Subtask runs with unlimited steps (same as parent) */
  int max_steps = -1;

  /* ── Depth guard ──────────────────────────────────────── */
  /* ctx->react_loop encodes the current loop ID.  To track depth,
     * we count how many subtask_N directories are nested in session_dir.
     * A simpler approach: pass depth through a naming convention. */
  int depth = 0;
  {
    /* Only count /subtask_ within the session hierarchy, not in the
         * workspace prefix.  E.g. /projects/subtask_tests/.sessions/123/subtask_0
         * should yield depth=1, not depth=2. */
    const char *base = strstr(ctx->session_dir, "/.sessions/");
    const char *p = base ? base : ctx->session_dir;
    while ((p = strstr(p, "/subtask_")) != NULL) {
      depth++;
      p += 9;
    }
  }
  int max_depth = ctx->cfg->subtask_max_depth;
  if (max_depth > SUBTASK_MAX_DEPTH_CAP) max_depth = SUBTASK_MAX_DEPTH_CAP;
  if (max_depth < 1) max_depth = 1;
  if (depth >= max_depth) {
    char err[128];
    snprintf(err, sizeof(err),
             "Sub-task nesting limit reached (depth %d >= %d). "
             "Solve this directly instead of spawning another sub-task.",
             depth, max_depth);
    return tools_make_error(err);
  }

  /* ── Create child session directory under parent's session ──── */
  int seq = atomic_fetch_add(&subtask_counter, 1);
  char child_dir[4096];
  snprintf(child_dir, sizeof(child_dir), "%s/subtask_%d", ctx->session_dir, seq);
  if (mkdir(child_dir, 0755) != 0 && errno != EEXIST) {
    char err[256];
    snprintf(err, sizeof(err), "Failed to create subtask directory: %s", strerror(errno));
    return tools_make_error(err);
  }

  /* ── Create child journal ─────────────────────────────── */
  journal_t *child_journal = journal_new(child_dir);
  if (!child_journal)
    return tools_make_error("Failed to create subtask journal");

  /* ── Create child tool context (follows playbook.c:572 pattern) ── */
  tool_ctx_t child_tools = {
    .store = ctx->store,             /* shared: global content store */
    .journal = child_journal,        /* own: isolated journal */
    .memory = ctx->memory,           /* shared: global memory */
    .ws = ctx->ws,                   /* shared: workspace */
    .cfg = ctx->cfg,                 /* shared: configuration */
    .session_dir = child_dir,        /* own: subtask directory */
    .session_lock_fd = -1,           /* no lock needed (under parent dir) */
    .provider = ctx->provider,       /* shared: LLM provider */
    .react_loop = 0,                 /* fresh loop numbering */
    .aliases = alias_map_new(),      /* own: fresh alias map */
    .last_notes_step = -1,           /* init: no notes yet */
    .session_idx = ctx->session_idx, /* inherit: session index for memory_search */
  };

  /* Scratchpad: snapshot parent's scratchpad into child (read-only copy).
     * Child modifications do NOT propagate back to parent.
     * Skipped for "minimal" context - child gets a clean slate.
     * No scratchpad_copy() exists, so we serialize + parse. */
  scratchpad_init(&child_tools.scratch);
  if (ctx_mode != CTX_MINIMAL) {
    char *parent_scratch = scratchpad_serialize(&ctx->scratch);
    if (parent_scratch) {
      scratchpad_parse(&child_tools.scratch, parent_scratch, "inherited", 5);
      free(parent_scratch);
    }
  }

  /* Block user_ask in child — no TUI user to answer.
     * We do this by not initializing the user_ask mutex/cond in the
     * child react_ctx_t (leaving them zeroed), which means react_run
     * will work but user_ask cannot be used. The tool_filter blocks it. */
  const char *blocked_tools[] = {"user_ask", "subtask"}; /* prevent recursion via filter too */
  tool_filter_t child_filter = {
    .blocked = blocked_tools,
    .n_blocked = (depth + 1 >= max_depth) ? 2 : 1, /* block subtask at max depth-1 */
  };
  /* If parent already has a filter, we only add our blocks.
     * For simplicity, just use our filter (subtask inherits all tools
     * except user_ask, and subtask at depth limit). */
  child_tools.tool_filter = child_filter;

  /* ── Temperature override: clone provider if needed ──── */
  provider_t *child_provider = ctx->provider;
  int child_provider_owned = 0;
  if (temp_override >= 0) {
    child_provider = provider_clone_with_temperature(
      ctx->provider, (float)temp_override, &child_provider_owned);
  }

  /* ── Create child react context ───────────────────────── */
  react_ctx_t child_react = {
    .provider = child_provider,     /* owned or shared */
    .tools = &child_tools,          /* own */
    .max_steps = max_steps,         /* capped */
    .verbose = 0,                   /* quiet — parent handles UI */
    .flags = REACT_FLAGS_BARE,      /* baseline: no reflection/scoring/memory */
    .parent_loop = ctx->react_loop, /* DAG edge to parent */
  };

  /* "rich" and "critic" context: enable memory injection + compaction.
   * This lets the child access workspace memory (lessons, skills, strategies)
   * which is useful for implementation subtasks that need prior decisions.
   * "critic" additionally sets a system prompt that instructs the child
   * to find flaws rather than solve the problem (Meta^n depth-4+ pattern). */
  if (ctx_mode == CTX_RICH || ctx_mode == CTX_CRITIC) {
    child_react.flags.inject_memory = 1;
    child_react.flags.enable_compaction = 1;
  }
  if (ctx_mode == CTX_CRITIC) {
    static const char critic_prompt[] =
      "You are a CRITIC. Your job is to find flaws, risks, and blind spots "
      "in the approach described in your query.\n"
      "\n"
      "Rules:\n"
      "- Do NOT solve the problem yourself.\n"
      "- Identify what is wrong, incomplete, or risky in the current solution.\n"
      "- Point out unstated assumptions that could break.\n"
      "- Suggest specific alternative strategies if the current one is flawed.\n"
      "- Rank issues by severity (critical > important > minor).\n"
      "- Be concrete: cite file:line, variable names, exact conditions.\n";
    child_react.custom_system_prompt = critic_prompt;
    child_react.system_prompt_replace = 0; /* append to base */
  }

  /* Initialize user_ask mutex/cond (react_run may reference them) */
  pthread_mutex_init(&child_react.user_ask_mutex, NULL);
  pthread_cond_init(&child_react.user_ask_cond, NULL);
  /* Pause infrastructure: share the parent's pause mutex/cond/query.
   * When the user types during a subtask, main.c writes to the root
   * react_ctx_t's pause fields.  The child reads from the parent via
   * pause_owner and injects the redirect into its own chat, so user
   * input reaches the currently-visible react loop. */
  child_react.pause_owner = ctx->react_ctx;
  child_tools.react_ctx = &child_react; /* back-link for nested subtasks */

  /* ── Journal the subtask start ────────────────────────── */
  tools_inject_thought(ctx, params);

  /* ── Enrich query with subtask context ────────────────── */
  /* The child doesn't know it's a subtask — it gets the same system prompt
     * as a top-level session.  Prepend instructions so it knows to report
     * comprehensively: the parent only sees the done() result text, so any
     * findings, dead ends, or unexpected discoveries not included there are
     * permanently lost.  This is the highest-ROI fix for subtask information
     * loss (prompt change, zero architectural risk). */
  char *enriched_query = NULL;
  {
    static const char preamble[] =
      "[SUBTASK CONTEXT]\n"
      "You are running as an isolated sub-task. The parent agent "
      "that spawned you sees ONLY your done() result text -- your "
      "intermediate steps, tool outputs, and reasoning are discarded.\n"
      "\n"
      "To maximize the value of your result:\n"
      "- Include all key findings, not just the final answer\n"
      "- Report dead ends explored and why they were ruled out\n"
      "- Flag anything unexpected or surprising, even if tangential\n"
      "- When multiple observations are independently valuable, list them all\n"
      "- Include exact file:line references, command outputs, and concrete data\n"
      "- Do NOT editorialize about what the parent \"probably\" wants -- "
      "report everything you found and let the parent decide what matters\n"
      "\n"
      "[TASK]\n";
    size_t plen = sizeof(preamble) - 1;
    size_t qlen = strlen(query);
    enriched_query = xmalloc(plen + qlen + 1);
    if (enriched_query) {
      memcpy(enriched_query, preamble, plen);
      memcpy(enriched_query + plen, query, qlen + 1);
    }
  }
  const char *effective_query = enriched_query ? enriched_query : query;

  /* ── Run the child react loop ─────────────────────────── */
  /* Forward parent's event callback so subtask steps are visible in TUI.
     * Pattern mirrors playbook.c pb_event_cb: enrich events with child
     * session_dir so TUI reads the correct journal for reactRX.md. */
  subtask_event_ctx_t ev_ctx = {
    .parent_cb = ctx->on_event,
    .parent_data = ctx->on_event_data,
    .child_dir = child_dir,
    .parent_dir = ctx->session_dir,
    .parent_loop = ctx->react_loop,
  };
  react_event_fn cb = ctx->on_event ? subtask_event_cb : NULL;
  void *cb_data = ctx->on_event ? &ev_ctx : NULL;
  char *result = react_run(&child_react, effective_query, cb, cb_data);

  /* ── Restore parent TUI context ──────────────────────── */
  /* Emit a synthetic STEP_START so ui_event.c switches playbook_session_dir
     * back to the parent's session dir for subsequent parent events. */
  if (ctx->on_event) {
    react_event_t restore = {0};
    restore.type = REACT_EVENT_STEP_START;
    restore.session_dir = ctx->session_dir;
    restore.react_loop = ctx->react_loop;
    restore.step = ctx->step;
    restore.max_steps = 0; /* won't trigger auto-nav (step > 1) */
    restore.pass_index = -1; /* not a playbook pass */
    ctx->on_event(&restore, ctx->on_event_data);
  }

  /* ── Cleanup child resources ──────────────────────────── */
  if (child_provider_owned) provider_free(child_provider);
  pthread_mutex_destroy(&child_react.user_ask_mutex);
  pthread_cond_destroy(&child_react.user_ask_cond);
  /* pause_mutex/cond/query belong to parent (via pause_owner) - not ours */
  free(child_react.user_ask_question);
  free(child_react.user_ask_answer);

  scratchpad_free(&child_tools.scratch);
  alias_map_free(child_tools.aliases);
  tool_free_deferred_consolidations(&child_tools);
  for (int i = 0; i < child_tools.n_recalled_keys; i++)
    free(child_tools.recalled_keys[i]);
  free(child_tools.recalled_keys);
  tool_fire_ledger_free(&child_tools);
  for (int i = 0; i < child_tools.n_modified_files; i++)
    free(child_tools.modified_files[i].path);
  child_tools.n_modified_files = 0;
  free(child_tools.last_spec_hash);
  journal_free(child_journal);
  free(enriched_query); /* subtask preamble (NULL-safe) */
  /* child_dir is stack-allocated, no free needed */
  /* session_lock_fd is -1, no release needed */

  /* ── Build result for parent context ──────────────────── */
  if (!result) {
    return tools_make_error("Sub-task failed to produce a result");
  }

  /* Store result in content-addressed store */
  char *hash = store_save(ctx->store, result);
  char *alias = hash ? tool_register_alias(ctx, hash) : NULL;

  cJSON *meta = cJSON_CreateObject();
  cJSON_AddStringToObject(meta, "status", "subtask completed");
  cJSON_AddStringToObject(meta, "result", result);
  cJSON_AddNumberToObject(meta, "depth", depth + 1);
  if (alias) { char _ref[64]; cJSON_AddStringToObject(meta, "ref", tool_ref_path(alias, _ref, sizeof(_ref))); }

  /* Store child_dir basename in params so TUI can build reactR0.md link */
  char child_basename[64];
  snprintf(child_basename, sizeof(child_basename), "subtask_%d", seq);
  cJSON_AddStringToObject(params, "child_dir", child_basename);

  tool_journal(ctx, "subtask", params, alias,
               strlen(result), 0, NULL, NULL);

  char *ref_copy = alias ? xstrdup(alias) : NULL;
  free(alias);
  free(hash);
  free(result);
  return tools_make_result(1, meta, ref_copy);
}

/* ── plugin registration ──────────────────────────────── */

static const char *context_enum[] = {"minimal", "standard", "rich", "critic", NULL};

static const tool_param_t subtask_params[] = {
  TOOL_PARAM("query", "string", "Task description for the sub-task to solve", 1),
  TOOL_PARAM_ENUM("context", "string",
    "Context inheritance level: "
    "\"minimal\" = query only (no scratchpad, no memory - best for search/analysis), "
    "\"standard\" = inherits scratchpad (default), "
    "\"rich\" = inherits scratchpad + memory injection (for implementation tasks needing prior decisions), "
    "\"critic\" = like rich, plus a critic system prompt (finds flaws instead of solving)",
    0, context_enum),
  TOOL_PARAM("temperature", "number",
    "Override temperature for this subtask (0.0-1.0). "
    "Higher = more creative, lower = more precise. "
    "Ignored for reasoning models. Default: inherit from parent.", 0),
  TOOL_PARAM_END};

static const tool_plugin_t subtask_plugin =
  TOOL_DEF("subtask",
           "Spawn an isolated sub-task with its own context. The child runs a full react loop in isolation and returns only the final result -- the parent's context grows by exactly 2 messages regardless of how many steps the child took. Use for self-contained sub-problems (searching, analyzing, building) that would otherwise bloat the parent's context with intermediate steps. Use context=\"minimal\" for search/analysis (no parent noise), context=\"rich\" for implementation needing memory, context=\"critic\" for reviewing/critiquing an approach.",
           subtask_params, tool_subtask);
TOOL_PLUGIN_REGISTER(subtask_plugin)
