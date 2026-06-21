/* react_context.c — Initial context construction for the react loop.
 * Extracted from react_run() to reduce its ~900-line monolith (Fix #2).
 *
 * Builds the initial chat context: system prompt, memory injection,
 * scratchpad, previous result, user query, and journal logging.
 * Called once at the start of react_run() when not restoring from checkpoint.
 */

#include "react_internal.h"
#include "session_index.h"

/* Format a unix timestamp as a relative recency string (e.g. "2d ago", "3w ago").
 * Writes to a caller-supplied buffer. */
static const char *format_recency(double created_at, char *buf, size_t bufsz) {
    if (created_at <= 0) { snprintf(buf, bufsz, "unknown"); return buf; }
    double now = (double)time(NULL);
    double diff = now - created_at;
    if (diff < 0) diff = 0;
    int days = (int)(diff / 86400.0);
    if (days == 0) snprintf(buf, bufsz, "today");
    else if (days == 1) snprintf(buf, bufsz, "1d ago");
    else if (days < 7) snprintf(buf, bufsz, "%dd ago", days);
    else if (days < 30) snprintf(buf, bufsz, "%dw ago", days / 7);
    else if (days < 365) snprintf(buf, bufsz, "%dmo ago", days / 30);
    else snprintf(buf, bufsz, "%dy ago", days / 365);
    return buf;
}

/* Review B5: Converted from INJECT_TYPE macro to debuggable static function.
 * Injects relevant memories of a given type prefix into the chat context.
 * Change 3 (arXiv 2605.15184 Finding #6): Enriched rendering — includes
 * temporal recency and confidence metadata alongside memory content. */
static void inject_memory_type(llm_chat_t *chat, tool_ctx_t *tools,
                               memory_results_t *all, const char *label,
                               const char *prefix, int plen, int max_count,
                               llm_msg_type_t mtype) {
    if (max_count <= 0) return;
    int remaining = max_count;
    int added = 0;
    str_t msg = str_new(4096);
    str_appendf(&msg, "%s\n", label);
    for (int j = 0; j < all->count; j++) {
        if (all->entries[j].key &&
            strncmp(all->entries[j].key, prefix, (size_t)plen) == 0) {
            /* Enriched rendering: include recency + confidence metadata.
             * arXiv 2605.15184 Finding #6: how memories are RENDERED matters
             * as much as which ones are retrieved. */
            int hits = all->entries[j].recall_hits;
            int misses = all->entries[j].recall_misses;
            int confidence = (int)(100.0 * (hits + 1.0) / (hits + misses + 2.0));
            char recency_buf[32];
            format_recency(all->entries[j].created_at, recency_buf, sizeof(recency_buf));
            str_appendf(&msg, "\n--- %s (%s, %d recalls, confidence: %d%%) ---\n%s\n",
                all->entries[j].key, recency_buf,
                hits + misses, confidence,
                all->entries[j].value ? all->entries[j].value : "");
            tool_track_recalled_key(tools, all->entries[j].key);
            remaining--;
            added++;
        }
        if (remaining <= 0) break;
    }
    /* FIX #18: Use boolean flag instead of magic strlen+5 check */
    if (added > 0) {
        llm_chat_add_typed(chat, "user", str_cstr(&msg), mtype);
    }
    str_free(&msg);
}

/* FIX #9: Shared helper — injects memory index and pinned knowledge into chat.
 * Used by both react_build_context() and react_checkpoint_restore().
 * Returns mem_summary and pinned via output params for logging (caller frees). */
void react_inject_memory_and_pinned(llm_chat_t *chat, tool_ctx_t *tools,
                                     char **out_mem_summary, char **out_pinned) {
    char *mem_summary = tools->ws
        ? workspace_build_index(tools->ws)
        : memory_build_index(tools->memory);
    if (mem_summary && strlen(mem_summary) > 0) {
        llm_chat_add_formatted(chat, "user", LLM_MSG_MEMORY_INDEX,
            "[MEMORY INDEX]\n%s\n\n"
            "Call memory_search when the answer may depend on user preferences, "
            "prior decisions, ongoing projects, or historical context not visible "
            "in the current conversation.\n"
            "Use memory_list to browse all keys (optionally filtered by type).",
            mem_summary);
    }

    char *pinned = tools->ws
        ? workspace_load_pinned(tools->ws)
        : memory_load_pinned(tools->memory);
    if (pinned && strlen(pinned) > 0) {
        llm_chat_add_formatted(chat, "user", LLM_MSG_PINNED,
            "[PINNED KNOWLEDGE]\n%s", pinned);
    }

    if (out_mem_summary) *out_mem_summary = mem_summary; else free(mem_summary);
    if (out_pinned) *out_pinned = pinned; else free(pinned);
}

/* FIX #15: Extracted from react_build_context — was 65 lines nested 4 deep.
 * Filters scratchpad sections for branching: only R*_result sections from
 * ancestor loops are included. Returns malloc'd serialized string (caller frees).
 * Walks parent chain in journal to build ancestor set, then filters sections. */
static char *scratchpad_filter_for_branch(scratchpad_t *scratch,
                                          const char *session_dir,
                                          int parent_loop,
                                          size_t max_budget) {
    /* Build ancestor set by walking parent chain in journal */
    /* FIX #7: Dynamic allocation replaces fixed ancestors[256] array */
    int anc_cap = 64;
    int *ancestors = malloc(sizeof(int) * (size_t)anc_cap);
    if (!ancestors) return scratchpad_serialize_budget(scratch, max_budget);
    int n_ancestors = 0;
    ancestors[n_ancestors++] = parent_loop;

    char jpath[NASH_PATH_MAX];
    FILE *jf = NULL;
    if (session_dir) {
        snprintf(jpath, sizeof(jpath), "%s/journal.jsonl", session_dir);
        jf = fopen(jpath, "r");
    }
    if (jf) {
        int pmap_cap = 1024;
        int *pmap = malloc(sizeof(int) * (size_t)pmap_cap);
        if (pmap)
            memset(pmap, -1, sizeof(int) * (size_t)pmap_cap);
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
                    cJSON_GetObjectItem(entry, "params"), "parent_loop");
                if (pp && cJSON_IsNumber(pp) && rl >= 0) {
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
            int cur = parent_loop;
            while (cur >= 0 && cur < pmap_cap && pmap[cur] >= 0) {
                if (n_ancestors >= anc_cap) {
                    int new_cap = anc_cap * 2;
                    int *tmp = realloc(ancestors, sizeof(int) * (size_t)new_cap);
                    if (!tmp) break;
                    ancestors = tmp;
                    anc_cap = new_cap;
                }
                cur = pmap[cur];
                ancestors[n_ancestors++] = cur;
            }
            free(pmap);
        }
    }

    /* Build filtered scratchpad copy — only ancestor R*_result sections */
    scratchpad_t filtered;
    scratchpad_init(&filtered);
    for (int si = 0; si < scratch->count; si++) {
        const char *sname = scratch->sections[si].name;
        int rloop = -1;
        if (sname && sscanf(sname, "R%d_result", &rloop) == 1) {
            int is_ancestor = 0;
            for (int ai = 0; ai < n_ancestors; ai++) {
                if (ancestors[ai] == rloop) { is_ancestor = 1; break; }
            }
            if (!is_ancestor) continue;
        }
        scratchpad_write(&filtered, sname,
                         scratch->sections[si].content,
                         scratch->sections[si].priority);
    }
    char *serialized = scratchpad_serialize_budget(&filtered, max_budget);
    scratchpad_free(&filtered);
    free(ancestors);
    return serialized;
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
        /* FIX #9: Use shared helper for memory index + pinned injection */
        char *mem_summary = NULL, *pinned = NULL;
        react_inject_memory_and_pinned(chat, ctx->tools, &mem_summary, &pinned);

        /* ── Change 1: Temporal Event Calendar ──────────────────────────
         * arXiv 2605.15184 Finding #5: temporal event structuring is the
         * most impactful single component. Inject a chronological overview
         * of recent memory activity to enable temporal reasoning. */
        {
            int do_temporal = ctx->tools->cfg
                ? ctx->tools->cfg->temporal_calendar : 1;
            if (do_temporal) {
                int recent_days = ctx->tools->cfg
                    ? ctx->tools->cfg->temporal_recent_days : 7;
                int older_days = ctx->tools->cfg
                    ? ctx->tools->cfg->temporal_older_days : 30;
                int max_entries = ctx->tools->cfg
                    ? ctx->tools->cfg->temporal_max_entries : 20;
                double now = (double)time(NULL);
                double recent_cutoff = now - recent_days * 86400.0;
                double older_cutoff = now - older_days * 86400.0;

                typedef struct { const char *key; const char *desc; double ts; } tcal_entry_t;
                int tcal_cap = max_entries * 2 < 64 ? 64 : max_entries * 2;
                tcal_entry_t *recent = malloc(sizeof(tcal_entry_t) * (size_t)tcal_cap);
                tcal_entry_t *older = malloc(sizeof(tcal_entry_t) * (size_t)tcal_cap);
                int n_recent = 0, n_older = 0;

                memory_t *mem = ctx->tools->ws ? NULL : ctx->tools->memory;
                if (mem && recent && older) {
                    pthread_mutex_lock(&mem->mtx);
                    for (int mi = 0; mi < mem->idx.count; mi++) {
                        const mem_index_entry_t *e = &mem->idx.entries[mi];
                        if (!e->key || !e->description) continue;
                        if (e->created_at >= recent_cutoff && n_recent < tcal_cap) {
                            recent[n_recent++] = (tcal_entry_t){ e->key, e->description, e->created_at };
                        } else if (e->created_at >= older_cutoff && n_older < tcal_cap) {
                            older[n_older++] = (tcal_entry_t){ e->key, e->description, e->created_at };
                        }
                    }
                    pthread_mutex_unlock(&mem->mtx);
                }

                /* Sort by timestamp descending (insertion sort — small N) */
                for (int i = 1; i < n_recent; i++) {
                    tcal_entry_t tmp = recent[i];
                    int j = i - 1;
                    while (j >= 0 && recent[j].ts < tmp.ts) {
                        recent[j + 1] = recent[j]; j--;
                    }
                    recent[j + 1] = tmp;
                }
                for (int i = 1; i < n_older; i++) {
                    tcal_entry_t tmp = older[i];
                    int j = i - 1;
                    while (j >= 0 && older[j].ts < tmp.ts) {
                        older[j + 1] = older[j]; j--;
                    }
                    older[j + 1] = tmp;
                }

                if (n_recent > 0 || n_older > 0) {
                    str_t cal = str_new(2048);
                    str_appendf(&cal, "[TEMPORAL CONTEXT]\n");
                    if (n_recent > 0) {
                        str_appendf(&cal, "Recent (last %d days):\n", recent_days);
                        int limit = n_recent < max_entries / 2 ? n_recent : max_entries / 2;
                        for (int i = 0; i < limit; i++) {
                            time_t ts = (time_t)recent[i].ts;
                            struct tm tm;
                            localtime_r(&ts, &tm);
                            char datebuf[16];
                            strftime(datebuf, sizeof(datebuf), "%Y-%m-%d", &tm);
                            str_appendf(&cal, "  %s  %s — %s\n",
                                datebuf, recent[i].key, recent[i].desc);
                        }
                    }
                    if (n_older > 0) {
                        str_appendf(&cal, "Older (last %d days):\n", older_days);
                        int limit = n_older < max_entries / 2 ? n_older : max_entries / 2;
                        for (int i = 0; i < limit; i++) {
                            time_t ts = (time_t)older[i].ts;
                            struct tm tm;
                            localtime_r(&ts, &tm);
                            char datebuf[16];
                            strftime(datebuf, sizeof(datebuf), "%Y-%m-%d", &tm);
                            str_appendf(&cal, "  %s  %s — %s\n",
                                datebuf, older[i].key, older[i].desc);
                        }
                    }
                    llm_chat_add_typed(chat, "user", str_cstr(&cal), LLM_MSG_TEMPORAL);
                    str_free(&cal);
                }
                free(recent);
                free(older);
            }
        }

        /* ── Change 5: Episodic Recall at Init ─────────────────────────
         * Query session_index for similar past sessions and inject the
         * best matching journal chunks. Unlocks 470MB of journal data
         * that currently sits unused during execution. */
        {
            int do_episodic = ctx->tools->cfg
                ? ctx->tools->cfg->episodic_recall : 1;
            if (do_episodic && ctx->tools->session_idx) {
                memory_t *ep_mem = ctx->tools->ws ? NULL : ctx->tools->memory;
                embed_ctx_t *emb_ctx = ep_mem ? memory_embed_ctx(ep_mem) : NULL;
                if (emb_ctx && emb_ctx->available) {
                    embed_vec_t query_emb = embed_text(emb_ctx, user_query);
                    if (query_emb.data) {
                        int ep_max = ctx->tools->cfg
                            ? ctx->tools->cfg->episodic_max_results : 2;
                        double ep_min = ctx->tools->cfg
                            ? ctx->tools->cfg->episodic_min_score : 0.35;
                        session_index_results_t ses =
                            session_index_search(ctx->tools->session_idx,
                                                 &query_emb, ep_max);
                        for (int si = 0; si < ses.count; si++) {
                            session_index_result_t *sr = &ses.results[si];
                            if (sr->score >= ep_min) {
                                const char *preview = sr->chunk_preview
                                    ? sr->chunk_preview : sr->manifest;
                                if (preview && preview[0]) {
                                    const char *ts_str = strrchr(sr->session_dir, '/');
                                    ts_str = ts_str ? ts_str + 1 : sr->session_dir;
                                    llm_chat_add_formatted(chat, "user",
                                        LLM_MSG_EPISODIC,
                                        "[RECALLED SESSION CHUNK — %s]\n%s\n"
                                        "  -> file_read %s/journal.jsonl for full context",
                                        ts_str, preview, sr->session_dir);
                                }
                            }
                        }
                        session_index_results_free(&ses);
                        embed_vec_free(&query_emb);
                    }
                }
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

        /* ── Change 7: Associative Graph Walk (Depth-1 Ref Following) ──
         * When a recalled memory has refs[], follow them one level deep.
         * MRAgent (ICML 2026): reconstruction via graph traversal outperforms
         * single-query retrieval by 23%. Our refs[] already exist but are only
         * used for score boosting — actually injecting them implements
         * associative recall. */
        {
            int assoc_depth = ctx->tools->cfg
                ? ctx->tools->cfg->associative_depth : 1;
            if (assoc_depth > 0) {
                memory_t *amem = ctx->tools->ws ? NULL : ctx->tools->memory;
                if (amem) {
                    str_t assoc_msg = str_new(2048);
                    int assoc_added = 0;
                    for (int j = 0; j < all_memories.count && assoc_added < 3; j++) {
                        if (all_memories.entries[j].n_refs <= 0) continue;
                        for (int ri = 0; ri < all_memories.entries[j].n_refs && assoc_added < 3; ri++) {
                            const char *ref_key = all_memories.entries[j].refs[ri];
                            if (!ref_key) continue;
                            /* Check not already recalled */
                            int dup = 0;
                            for (int k = 0; k < ctx->tools->n_recalled_keys; k++) {
                                if (strcmp(ctx->tools->recalled_keys[k], ref_key) == 0) {
                                    dup = 1; break;
                                }
                            }
                            if (dup) continue;
                            const mem_index_entry_t *ref_entry = memory_find(amem, ref_key);
                            if (ref_entry && ref_entry->value) {
                                if (assoc_added == 0)
                                    str_appendf(&assoc_msg, "[ASSOCIATED MEMORIES]\n");
                                str_appendf(&assoc_msg,
                                    "\n--- %s (via %s) ---\n%s\n",
                                    ref_key, all_memories.entries[j].key,
                                    ref_entry->value);
                                tool_track_recalled_key(ctx->tools, ref_key);
                                assoc_added++;
                            }
                        }
                    }
                    if (assoc_added > 0) {
                        llm_chat_add_typed(chat, "user", str_cstr(&assoc_msg),
                                          LLM_MSG_MEMORY_HINT);
                    }
                    str_free(&assoc_msg);
                }
            }
        }

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
    eviction_policy_t pol = react_eviction_policy(ctx->tools->cfg);
    long context_budget = react_context_budget(ctx);
    long current_chars = react_calc_total_chars(chat);
    size_t max_scratchpad = react_scratchpad_budget_pol(context_budget, current_chars,
                                                        (size_t)pol.sp_min_chars, &pol);

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
                /* FIX #15: Use extracted helper instead of 65-line inline block */
                serialized = scratchpad_filter_for_branch(
                    &ctx->tools->scratch, ctx->tools->session_dir,
                    ctx->parent_loop, max_scratchpad);
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
            /* FIX #12: Use llm_chat_add_formatted to eliminate alloc pattern */
            llm_chat_add_formatted(chat, "user", LLM_MSG_PREV_RESULT,
                "[PREVIOUS RESULT]\n%s\n"
                "The above is the result of the previous task. "
                "You can reference it for follow-up queries.",
                prev_result);
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
