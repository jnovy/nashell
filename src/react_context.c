/* react_context.c — Initial context construction for the react loop.
 * Extracted from react_run() to reduce its ~900-line monolith (Fix #2).
 *
 * Builds the initial chat context: system prompt, memory injection,
 * scratchpad, previous result, user query, and journal logging.
 * Called once at the start of react_run() when not restoring from checkpoint.
 */

#include "react_internal.h"

/* Review B5: Converted from INJECT_TYPE macro to debuggable static function.
 * Injects relevant memories of a given type prefix into the chat context. */
static void inject_memory_type(llm_chat_t *chat, tool_ctx_t *tools,
                               memory_results_t *all, const char *label,
                               const char *prefix, int plen, int max_count,
                               llm_msg_type_t mtype) {
    if (max_count <= 0) return;
    int remaining = max_count;
    str_t msg = str_new(4096);
    str_appendf(&msg, "%s\n", label);
    for (int j = 0; j < all->count; j++) {
        if (all->entries[j].key &&
            strncmp(all->entries[j].key, prefix, (size_t)plen) == 0) {
            str_appendf(&msg, "\n--- %s ---\n%s\n",
                all->entries[j].key,
                all->entries[j].value ? all->entries[j].value : "");
            tool_track_recalled_key(tools, all->entries[j].key);
            remaining--;
        }
        if (remaining <= 0) break;
    }
    if (msg.len > strlen(label) + 5) {
        llm_chat_add_typed(chat, "user", str_cstr(&msg), mtype);
    }
    str_free(&msg);
}

void react_build_context(react_ctx_t *ctx, llm_chat_t *chat,
                         const char *user_query,
                         react_event_fn on_event, void *userdata) {
    (void)on_event; (void)userdata;

    /* Reset per-loop counters FIRST — before any journal logging that uses step.
     * Previously this was done after memory injection, causing memory_context
     * journal entries to inherit the step value from the previous react loop. */
    ctx->tools->step = 0;

    /* System message */
    react_add_system_prompt(chat, ctx->tools->cfg);

    /* v5: No manifest injection — scratchpad is the sole persistence mechanism.
     * Cross-loop state is carried via scratchpad (auto-saved done results +
     * LLM-pruned summaries). Within-loop recovery uses LLM summarization
     * instead of manifest re-injection. */

    /* Inject memory summary (counts only — no alphabetical listing) */
    if (ctx->flags.inject_memory && (ctx->tools->memory || ctx->tools->ws)) {
        char *mem_summary = ctx->tools->ws
            ? workspace_build_index(ctx->tools->ws)
            : memory_build_index(ctx->tools->memory);
        /* S4 FIX: Cache strlen result instead of calling twice. */
        size_t mem_summary_len = mem_summary ? strlen(mem_summary) : 0;
        if (mem_summary_len > 0) {
            size_t mem_msg_sz = mem_summary_len + 512;
            char *mem_msg = malloc(mem_msg_sz);
            if (mem_msg) {
                snprintf(mem_msg, mem_msg_sz, "[MEMORY INDEX]\n%s\n\n"
                        "Call memory_recall when the answer may depend on user preferences, "
                        "prior decisions, ongoing projects, or historical context not visible "
                        "in the current conversation.\n"
                        "Use memory_list to browse all keys (optionally filtered by type).", mem_summary);
                llm_chat_add_typed(chat, "user", mem_msg, LLM_MSG_MEMORY_INDEX);
                free(mem_msg);
            }
        }

        /* Inject pinned memories (always-active knowledge) */
        char *pinned = ctx->tools->ws
            ? workspace_load_pinned(ctx->tools->ws)
            : memory_load_pinned(ctx->tools->memory);
        if (pinned && strlen(pinned) > 0) {
            size_t pin_msg_sz = strlen(pinned) + 64;
            char *pin_msg = malloc(pin_msg_sz);
            if (pin_msg) {
                snprintf(pin_msg, pin_msg_sz, "[PINNED KNOWLEDGE]\n%s", pinned);
                llm_chat_add_typed(chat, "user", pin_msg, LLM_MSG_PINNED);
                free(pin_msg);
            }
        }

        /* Inject relevant memories by type — semantic recall filtered by prefix. */
        int max_skills = ctx->tools->cfg ? ctx->tools->cfg->max_skills_per_query : 3;
        int max_lessons = ctx->tools->cfg ? ctx->tools->cfg->max_lessons_per_query : 2;
        int max_strategies = ctx->tools->cfg ? ctx->tools->cfg->max_strategies_per_query : 2;
        int max_antipatterns = ctx->tools->cfg ? ctx->tools->cfg->max_antipatterns_per_query : 1;
        int max_candidates = (max_skills + max_lessons + max_strategies + max_antipatterns) * 3;

        /* Build enriched recall query: user_query + scratchpad content. */
        str_t recall_query = str_new(1024);
        str_append_cstr(&recall_query, user_query);
        if (ctx->tools->scratch.count > 0) {
            char *sp_text = scratchpad_serialize_budget(&ctx->tools->scratch, SIZE_MAX);
            if (sp_text && sp_text[0]) {
                str_append_cstr(&recall_query, "\n");
                str_append_cstr(&recall_query, sp_text);
            }
            free(sp_text);
        }
        memory_results_t all_memories = ctx->tools->ws
            ? workspace_recall(ctx->tools->ws, str_cstr(&recall_query), max_candidates)
            : memory_recall(ctx->tools->memory, str_cstr(&recall_query), max_candidates);
        str_free(&recall_query);

        /* Review B5: Replaced INJECT_TYPE macro with debuggable static function calls */
        inject_memory_type(chat, ctx->tools, &all_memories,
            "[RELEVANT SKILLS]", "skill:", 6, max_skills, LLM_MSG_SKILLS);
        inject_memory_type(chat, ctx->tools, &all_memories,
            "[RELEVANT LESSONS]", "lesson:", 7, max_lessons, LLM_MSG_LESSONS);
        inject_memory_type(chat, ctx->tools, &all_memories,
            "[RELEVANT STRATEGIES]", "strategy:", 9, max_strategies, LLM_MSG_STRATEGIES);
        inject_memory_type(chat, ctx->tools, &all_memories,
            "[RELEVANT ANTI-PATTERNS]", "anti-pattern:", 13, max_antipatterns, LLM_MSG_ANTIPATTERNS);

        /* Log memory context for debugging — before freeing mem_summary/pinned */
        react_log_memory_context(ctx->tools, ctx->tools->react_loop,
                           ctx->tools->step, mem_summary, pinned,
                           &all_memories, user_query);

        free(mem_summary);
        free(pinned);
        memory_results_free(&all_memories);
    }

    /* X4+S5 FIX: Scratchpad budget uses the shared dual-cap policy.
     * min(absolute_cap, remaining_cap) prevents initial injection from
     * being larger than after the first eviction cycle. */
    long context_budget = react_context_budget(ctx);
    long current_chars = react_calc_total_chars(chat);
    /* Review B7: Use REACT_SP_MIN constant instead of hardcoded 2048 */
    size_t max_scratchpad = react_scratchpad_budget(context_budget, current_chars,
                                                     REACT_SP_MIN);

    /* Inject scratchpad if exists (budget-aware, priority-ordered).
     * When branching (parent_loop != previous loop), filter R*_result
     * sections to only include ancestors in the branch path. */
    {
        char *serialized = NULL;
        int is_branch = (ctx->parent_loop >= 0 &&
                         ctx->tools->react_loop > 0 &&
                         ctx->parent_loop != ctx->tools->react_loop - 1);

        if (ctx->tools->scratch.count > 0) {
            if (is_branch) {
                /* Build ancestor set by walking parent chain in journal */
                int ancestors[256];
                int n_ancestors = 0;
                ancestors[n_ancestors++] = ctx->parent_loop;

                char jpath[NASH_PATH_MAX];
                FILE *jf = NULL;
                if (ctx->tools->session_dir) {
                    snprintf(jpath, sizeof(jpath), "%s/journal.jsonl",
                             ctx->tools->session_dir);
                    jf = fopen(jpath, "r");
                }
                if (jf) {
                    /* FIX #2: Use dynamically-sized parent map instead of fixed 1024.
                     * Previously, react_loop IDs >= 1024 were silently dropped,
                     * breaking ancestry tracking in long TUI sessions. */
                    int pmap_cap = 1024;
                    int *pmap = malloc(sizeof(int) * (size_t)pmap_cap);
                    if (pmap) {
                        memset(pmap, -1, sizeof(int) * (size_t)pmap_cap);
                    }
                    char jline[32768];
                    while (pmap && fgets(jline, sizeof(jline), jf)) {
                        cJSON *entry = cJSON_Parse(jline);
                        if (!entry) continue;
                        const char *jtool = cJSON_GetStringValue(
                            cJSON_GetObjectItem(entry, "tool"));
                        if (jtool && strcmp(jtool, "query") == 0) {
                            int rl = (int)cJSON_GetNumberValue(
                                cJSON_GetObjectItem(entry, "react_loop"));
                            cJSON *pp = cJSON_GetObjectItem(
                                cJSON_GetObjectItem(entry, "params"),
                                "parent_loop");
                            if (pp && cJSON_IsNumber(pp) && rl >= 0) {
                                /* Grow pmap if needed */
                                if (rl >= pmap_cap) {
                                    int new_cap = pmap_cap;
                                    while (new_cap <= rl) new_cap *= 2;
                                    int *tmp = realloc(pmap, sizeof(int) * (size_t)new_cap);
                                    if (tmp) {
                                        memset(tmp + pmap_cap, -1,
                                               sizeof(int) * (size_t)(new_cap - pmap_cap));
                                        pmap = tmp;
                                        pmap_cap = new_cap;
                                    } else {
                                        /* realloc failed — skip this entry */
                                        cJSON_Delete(entry);
                                        continue;
                                    }
                                }
                                pmap[rl] = (int)pp->valuedouble;
                            }
                        }
                        cJSON_Delete(entry);
                    }
                    fclose(jf);
                    if (pmap) {
                        int cur = ctx->parent_loop;
                        while (cur >= 0 && cur < pmap_cap && pmap[cur] >= 0
                               && n_ancestors < 256) {
                            cur = pmap[cur];
                            ancestors[n_ancestors++] = cur;
                        }
                        free(pmap);
                    }
                }

                /* Build a temporary filtered scratchpad copy */
                scratchpad_t filtered;
                scratchpad_init(&filtered);
                for (int si = 0; si < ctx->tools->scratch.count; si++) {
                    const char *sname = ctx->tools->scratch.sections[si].name;
                    int rloop = -1;
                    if (sname && sscanf(sname, "R%d_result", &rloop) == 1) {
                        int is_ancestor = 0;
                        for (int ai = 0; ai < n_ancestors; ai++) {
                            if (ancestors[ai] == rloop) {
                                is_ancestor = 1;
                                break;
                            }
                        }
                        if (!is_ancestor) continue;
                    }
                    scratchpad_write(&filtered, sname,
                                     ctx->tools->scratch.sections[si].content,
                                     ctx->tools->scratch.sections[si].priority);
                }
                serialized = scratchpad_serialize_budget(&filtered, max_scratchpad);
                scratchpad_free(&filtered);
            } else {
                /* Normal (linear) — serialize all sections */
                serialized = scratchpad_serialize_budget(&ctx->tools->scratch, max_scratchpad);
            }
        }
        /* D1 FIX: Use shared scratchpad injection helper */
        react_inject_scratchpad_msg(chat, chat->n_msgs, serialized);
        free(serialized);
    }

    /* Inject previous result — loaded from session_dir/result.txt. */
    if (ctx->flags.inject_prev_result && ctx->tools->session_dir) {
        char rpath[NASH_PATH_MAX];
        snprintf(rpath, sizeof(rpath), "%s/result.txt", ctx->tools->session_dir);
        char *prev_result = slurp_file(rpath, NULL);
        if (prev_result && strlen(prev_result) > 0) {
            size_t rlen = strlen(prev_result);
            char *prev_msg = malloc(rlen + 128);
            if (prev_msg) {
                snprintf(prev_msg, rlen + 128,
                    "[PREVIOUS RESULT]\n%s\n"
                    "The above is the result of the previous task. "
                    "You can reference it for follow-up queries.",
                    prev_result);
                llm_chat_add_typed(chat, "user", prev_msg, LLM_MSG_PREV_RESULT);
                free(prev_msg);
            }
        }
        free(prev_result);
    }

    /* User query */
    llm_chat_add_typed(chat, "user", user_query, LLM_MSG_USER_QUERY);

    /* Record system prompt and user query in journal (step 0).
     * Fix #12: reuse react_build_system_prompt(). */
    {
        char *sys_prompt = react_build_system_prompt(ctx->tools->cfg);
        char *sys_hash = store_save(ctx->tools->store, sys_prompt);
        char *sys_alias = tool_register_alias(ctx->tools, sys_hash);
        cJSON *sys_p = cJSON_CreateObject();
        cJSON_AddStringToObject(sys_p, "type", "system_prompt");
        if (ctx->provider && ctx->provider->cfg.model_id)
            cJSON_AddStringToObject(sys_p, "model", ctx->provider->cfg.model_id);
        if (ctx->tools->cfg && ctx->tools->cfg->provider.type)
            cJSON_AddStringToObject(sys_p, "provider", ctx->tools->cfg->provider.type);
        if (ctx->tools->cfg && ctx->tools->cfg->matched_profile_file)
            cJSON_AddStringToObject(sys_p, "profile", ctx->tools->cfg->matched_profile_file);
        journal_append(ctx->tools->journal, ctx->tools->react_loop, 0, "system", sys_p, sys_alias,
                       strlen(sys_prompt), count_lines(sys_prompt), NULL, NULL);
        cJSON_Delete(sys_p);
        free(sys_alias);
        free(sys_hash);
        free(sys_prompt);

        cJSON *q_p = cJSON_CreateObject();
        cJSON_AddStringToObject(q_p, "text", user_query);
        cJSON_AddNumberToObject(q_p, "parent_loop", ctx->parent_loop);
        char *q_hash = store_save(ctx->tools->store, user_query);
        char *q_alias = q_hash ? tool_register_alias(ctx->tools, q_hash) : NULL;
        journal_append(ctx->tools->journal, ctx->tools->react_loop, 0, "query", q_p, q_alias,
                       strlen(user_query), 0, NULL, NULL);
        cJSON_Delete(q_p);
        free(q_alias);
        free(q_hash);
    }

    /* Log the full initial LLM context (all messages) as a single
     * journal entry so the TUI can show exactly what the LLM received. */
    {
        char *ctx_text = llm_chat_serialize(chat);
        if (ctx_text && ctx_text[0]) {
            char *ctx_hash = store_save(ctx->tools->store, ctx_text);
            char *ctx_alias = ctx_hash ? tool_register_alias(ctx->tools, ctx_hash) : NULL;
            cJSON *ctx_p = cJSON_CreateObject();
            cJSON_AddNumberToObject(ctx_p, "n_messages", chat->n_msgs);
            journal_append(ctx->tools->journal, ctx->tools->react_loop, 0,
                           "context", ctx_p, ctx_alias,
                           strlen(ctx_text), count_lines(ctx_text), NULL, NULL);
            cJSON_Delete(ctx_p);
            free(ctx_alias);
            free(ctx_hash);
        }
        free(ctx_text);
    }
}
