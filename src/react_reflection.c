/* react_reflection.c — Post-loop phases: validation scoring, reflection,
 * scratchpad promotion, scratchpad pruning.
 * Extracted from react.c (P1 decomposition). */
#include "react_internal.h"
#include "session_index.h"

/* ── reflection deduplication (in-memory index scan) ───── */

/* Log a reflection dedup decision to journal + store.
 * Shared by both "allowed_failure_correction" and "skipped" branches. */
static void log_dedup_decision(react_ctx_t *ctx, const char *key,
                               float sim, const char *action_str) {
    cJSON *dup_p = cJSON_CreateObject();
    cJSON_AddStringToObject(dup_p, "key", key);
    cJSON_AddNumberToObject(dup_p, "similarity", (double)sim);
    cJSON_AddStringToObject(dup_p, "action", action_str);
    char *dup_str = cJSON_PrintUnformatted(dup_p);
    char *dup_hash = dup_str ? store_save(ctx->tools->store, dup_str) : NULL;
    char *dup_ref = dup_hash ? tool_register_alias(ctx->tools, dup_hash) : NULL;
    journal_append(ctx->tools->journal,
        ctx->tools->react_loop, ctx->tools->step,
        "reflection_dedup", dup_p, dup_ref, 0, 0, NULL, NULL);
    free(dup_str);
    free(dup_hash);
    free(dup_ref);
    cJSON_Delete(dup_p);
}

/* Scans the in-memory embedding index instead of loading every .emb
 * file from disk.  O(N) cosine comparisons with zero I/O.
 * Returns 1 if a near-duplicate was found and should_store was updated. */
static int reflection_dedup_index_scan(
        react_ctx_t *ctx, cJSON *rkey_j,
        const embed_multi_vec_t *new_emb,
        int *should_store, int task_succeeded) {
    memory_t *mem = ctx->tools->memory;
    float dedup_thresh = (ctx->tools->cfg && ctx->tools->cfg->dedup_threshold > 0)
                          ? ctx->tools->cfg->dedup_threshold : 0.90f;
    int found = 0;

    pthread_mutex_lock(&mem->mtx);
    for (int ei = 0; ei < memory_count(mem) && !found; ei++) {
        mem_index_entry_t *ie = &mem->idx.entries[ei];
        if (!ie->has_emb || ie->emb.dim != new_emb->dim)
            continue;
        float sim = embed_cosine_sim_multi_multi(new_emb, &ie->emb);
        if (sim <= dedup_thresh)
            continue;

        found = 1;
        /* When a task FAILED, the reflection may produce a corrective insight
         * that contradicts an existing entry. Since contradictions have high
         * embedding similarity (same topic, opposite conclusion), we must
         * allow the store — memory_try_consolidate will classify it as
         * SUPERSEDES and delete the old entry. Only block for successes. */
        if (!task_succeeded) {
            log_dedup_decision(ctx, rkey_j->valuestring, sim,
                               "allowed_failure_correction");
            /* should_store stays 1 */
        } else {
            *should_store = 0;
            log_dedup_decision(ctx, rkey_j->valuestring, sim, "skipped");
        }
    }
    pthread_mutex_unlock(&mem->mtx);
    return found;
}

/* ── Post-loop: scoring, reflection, promotion, pruning ── */

void react_post_loop(react_ctx_t *ctx, const char *user_query,
                     const char *final_result, int task_succeeded,
                     react_event_fn on_event, void *userdata) {
    (void)user_query;  /* reserved for future use */
    /* Validation scoring: update recall_hits / recall_misses for recalled memories.
     * Counter bumps are written to JSON files but NOT git-committed —
     * these are high-frequency, low-value changes that pollute the git log
     * (access_count, recall_hits, recall_misses). Git history is reserved
     * for meaningful content changes (store, delete, prune, consolidate).
     *
     * Symmetric scoring: increment hits on success, misses on failure.
     * Without misses, vscore = (hits+1)/(hits+misses+2) can only increase
     * monotonically, making Bayesian pruning impossible — every memory
     * that participates in enough successful tasks converges to vscore≈1.0
     * regardless of whether it actually contributed to success.
     *
     * The noise objection (failure is rarely caused by recalled memories)
     * is valid but outweighed: with symmetric scoring, vscore converges
     * to the background success rate of tasks where the memory was recalled.
     * This is a meaningful signal — memories recalled during consistently
     * failing task types get demoted, while memories recalled during
     * consistently succeeding task types get promoted. The cold-start
     * exponent (vscore_exponent=0.3) already dampens vscore's influence
     * to limit the impact of noisy early signals. */
    if (ctx->flags.enable_scoring && ctx->tools->memory &&
        ctx->tools->n_recalled_keys > 0) {
        for (int i = 0; i < ctx->tools->n_recalled_keys; i++) {
            if (task_succeeded) {
                memory_increment_hits(ctx->tools->memory,
                                      ctx->tools->recalled_keys[i]);
            } else {
                memory_increment_misses(ctx->tools->memory,
                                        ctx->tools->recalled_keys[i]);
            }
        }
    }

    /* EvolveMem [arXiv:2605.13941]: Log memory quality telemetry.
     * Records which memories were recalled, task outcome, and usage stats
     * for post-hoc retrieval quality diagnosis by the self-harness. */
    if (ctx->flags.enable_scoring && ctx->tools->memory &&
        ctx->tools->n_recalled_keys > 0 && ctx->tools->journal) {
        cJSON *mq = cJSON_CreateObject();
        cJSON_AddNumberToObject(mq, "n_recalled", ctx->tools->n_recalled_keys);
        cJSON_AddBoolToObject(mq, "task_succeeded", task_succeeded);

        /* Build array of recalled keys */
        cJSON *keys_arr = cJSON_CreateArray();
        int cold_start_count = 0;  /* keys with vscore=0.5 (no evidence) */
        for (int i = 0; i < ctx->tools->n_recalled_keys; i++) {
            cJSON_AddItemToArray(keys_arr,
                cJSON_CreateString(ctx->tools->recalled_keys[i]));
            /* Check if this key has zero evidence */
            if (ctx->tools->memory) {
                /* FIX CRITICAL #2: memory_find returns owned copy */
                mem_index_entry_t *ie = memory_find(
                    ctx->tools->memory, ctx->tools->recalled_keys[i]);
                if (ie && ie->recall_hits == 0 && ie->recall_misses == 0)
                    cold_start_count++;
                memory_find_free(ie);
            }
        }
        cJSON_AddItemToObject(mq, "recalled_keys", keys_arr);
        cJSON_AddNumberToObject(mq, "cold_start_count", cold_start_count);
        double cold_start_pct = ctx->tools->n_recalled_keys > 0
            ? (double)cold_start_count / (double)ctx->tools->n_recalled_keys
            : 0.0;
        cJSON_AddNumberToObject(mq, "cold_start_pct", cold_start_pct);

        char *mq_str = cJSON_PrintUnformatted(mq);
        char *mq_hash = mq_str ? store_save(ctx->tools->store, mq_str) : NULL;
        char *mq_ref = mq_hash ? tool_register_alias(ctx->tools, mq_hash) : NULL;
        journal_append(ctx->tools->journal, ctx->tools->react_loop,
                       ctx->tools->step, "memory_quality", mq,
                       mq_ref, 0, 0, NULL, NULL);
        free(mq_str);
        free(mq_hash);
        free(mq_ref);
        cJSON_Delete(mq);
    }

    /* FIX CRIT1: Flush deferred memory consolidations AFTER scoring
     * but BEFORE reflection. This runs the LLM-based consolidation calls
     * that were queued during the react loop, outside the hot path.
     * Reflection may create new memories that also get consolidated. */
    if (ctx->tools->memory && ctx->tools->n_deferred_consol > 0) {
        tool_flush_deferred_consolidations(ctx->tools);
    }

    /* FIX D2: Skip reflection when max_reflection_steps == 0 */
    int max_refl = ctx->tools->cfg ? ctx->tools->cfg->max_reflection_steps : 4;
    int refl_gate = ctx->tools->cfg ? ctx->tools->cfg->reflection_gate : 0;
    /* reflection_gate: 0=user_ask (only after user_ask), 1=always, 2=never.
     * When gated on user_ask, reflection only fires if the model had to ask
     * the user for help — indicating genuine learning opportunity. Tasks
     * solved autonomously don't warrant persistent memory storage. */
    int gate_ok = (refl_gate == 1) ||                   /* always */
                  (refl_gate == 0 && ctx->user_ask_used); /* user_ask gate */
    if (ctx->flags.enable_reflection && ctx->tools->step > 2 && ctx->tools->memory && max_refl > 0 && gate_ok) {
        llm_chat_t *reflect = llm_chat_new();
        if (task_succeeded) {
            llm_chat_add(reflect, "system",
                "You just completed a task successfully. Perform CAUSAL ANALYSIS "
                "(not narrative summary) by answering these questions:\n"
                "1. What assumptions held or almost failed?\n"
                "2. What hidden variables or context mattered most?\n"
                "3. What observations were initially ignored or underweighted?\n"
                "4. What search branches were pruned — correctly or incorrectly?\n"
                "5. What representation or mental model was key to success?\n"
                "6. What reusable invariant or principle generalizes beyond this task?\n\n"
                "Extract 0-3 reusable lessons, strategies, or skills. Each MUST identify "
                "a causal mechanism (X because Y), not just a narrative (I learned X).\n"
                "For each, call memory_store with:\n"
                "- key: lesson:short-name, strategy:short-name, or skill:short-name\n"
                "- value: the causal insight — state the assumption/variable/invariant "
                "explicitly (for skills: include approach, pitfalls, verification)\n"
""
                "Skills are reusable multi-step procedures (e.g. skill:compile-and-test-c).\n"
                "\n"
                "P5: SKILL EXTRACTION — if this task involved 5+ tool calls, extract a\n"
                "reusable skill with this structure:\n"
                "## When to apply\n"
                "<trigger condition — when should this skill be used?>\n"
                "## Steps\n"
                "1. step (tool) — WHY: rationale for this step\n"
                "2. step (tool) — WHY: rationale\n"
                "## Pitfalls\n"
                "- pitfall: what to do instead\n"
                "## Verification\n"
                "- how to confirm success\n"
                "\n"
                "Research basis for skill structure:\n"
                "  Letta Skill Learning [May 2026] — +36.8%% improvement on Terminal-Bench\n"
                "    from learned skills with approach, pitfalls, verification.\n"
                "  CODESKILL [arXiv:2605.25430, May 2026] — skill extraction from\n"
                "    trajectories with pitfalls as key component.\n"
                "  Bayesian-Agent [arXiv:2606.08348, Jun 2026] — posterior-guided\n"
                "    skill evolution from experience.\n"
                "\n"
                "If nothing worth storing, call done immediately.\n"
                "Respond with ONE JSON object per turn: "
                "{\"thought\":\"...\",\"action\":\"memory_store\"|\"done\",...}");
        } else {
            /* P5: Negative memory / anti-patterns — extract what NOT to do
             * from task failures. Anti-patterns are the defensive complement
             * to positive lessons: 3 well-placed warnings can prevent 85%
             * of repeated mistakes.
             *
             * Research basis:
             *   MemMorph [arXiv:2605.26154, May 2026] — showed that just
             *     3 injected records can redirect agent behavior 85.9% of
             *     the time. Anti-patterns use this same mechanism
             *     defensively to prevent repeated mistakes.
             *   MemFail [arXiv:2605.26667, May 2026] — diagnostic benchmark
             *     formalizing memory as summarization + storage + retrieval.
             *     Anti-patterns address the summarization failure mode by
             *     explicitly capturing what went wrong.
             *   Reflexion [Shinn et al., 2023] — trajectory memory storing
             *     failed attempts + reflections. Anti-patterns are the
             *     persistent, cross-session version of this.
             *   CODESKILL [arXiv:2605.25430, May 2026] — skill extraction
             *     includes "pitfalls" as a key component. Anti-patterns
             *     are standalone pitfall memories. */
            llm_chat_add(reflect, "system",
                "The task FAILED or was not completed (hit max steps, error, or timeout). "
                "Perform CAUSAL ANALYSIS (not narrative) by answering:\n"
                "1. What assumption failed? (the root cause, not the symptom)\n"
                "2. What hidden variable mattered that was not accounted for?\n"
                "3. What observation was available but ignored or misinterpreted?\n"
                "4. What search branch was pruned incorrectly? (wrong tool, wrong approach)\n"
                "5. What representation or mental model was insufficient?\n"
                "6. What reusable invariant would prevent this class of failure?\n\n"
                "Extract 1-3 items. For each, decide if it is:\n"
                "  (a) A LESSON (positive insight: \"do X because Y\"), or\n"
                "  (b) An ANTI-PATTERN (negative warning: \"NEVER do X because Y\").\n\n"
                "For lessons, call memory_store with:\n"
                "- key: lesson:short-name\n"
                "- value: the causal chain — root assumption, what broke it, the fix\n"
"\n"
                "For anti-patterns, call memory_store with:\n"
                "- key: anti-pattern:short-name (e.g. anti-pattern:never-grep-binary-files)\n"
                "- value: Start with 'NEVER' or 'AVOID'. State: what NOT to do, WHY it "
                "fails, and what to do INSTEAD. Include the trigger condition "
                "(when_NOT_to_apply).\n"
"\n"
                "Anti-patterns are MORE VALUABLE than lessons for preventing repeated "
                "mistakes. Prefer anti-patterns when the failure has a clear 'never do X' "
                "pattern. If nothing worth storing, call done immediately.\n"
                "Respond with ONE JSON object per turn: "
                "{\"thought\":\"...\",\"action\":\"memory_store\"|\"done\",...}");
        }

        /* Inject journal manifest as context for reflection */
        char *manifest = journal_manifest(ctx->tools->journal, 50);
        if (manifest) {
            llm_chat_add(reflect, "user", manifest);
            free(manifest);
        }

        /* FIX #10: Include scratchpad in reflection context — it often
         * contains the most important findings from the task */
        {
            char *sp_text = NULL;
            if (ctx->tools->scratch.count > 0) {
                sp_text = scratchpad_serialize(&ctx->tools->scratch);
            }
            if (sp_text && sp_text[0]) {
                char *sp_msg = react_format_scratchpad_msg(sp_text);
                if (sp_msg) {
                    llm_chat_add(reflect, "user", sp_msg);
                    free(sp_msg);
                }
            }
            free(sp_text);
        }

        /* FIX B3: Inject final_result into reflection context.
         * Without this, the reflection LLM doesn't know what the task
         * actually produced - it can only infer from tool call sequences.
         * This degrades reflection quality significantly. */
        if (final_result) {
            size_t fr_len = strlen(final_result);
            size_t show_len = fr_len > 2000 ? 2000 : fr_len;
            char *fr_msg = malloc(show_len + 64);
            if (fr_msg) {
                snprintf(fr_msg, show_len + 64, "[TASK RESULT]\n%.*s%s",
                         (int)show_len, final_result,
                         fr_len > 2000 ? "\n[truncated]" : "");
                llm_chat_add(reflect, "user", fr_msg);
                free(fr_msg);
            }
        }

        llm_chat_add(reflect, "user",
            task_succeeded
                ? "Analyze the causal chain of this task. What assumptions held? "
                  "What hidden variables mattered? What invariant generalizes? "
                  "Store 0-3 causal lessons via memory_store, or call done if none."
                : "Trace the causal chain of this failure. What root assumption broke? "
                  "What was the hidden variable? What invariant would prevent this "
                  "class of failure? Store 1-3 causal lessons via memory_store, or "
                  "call done if none.");

        /* FIX #9: Disable thinking mode for reflection — it's a lightweight
         * extraction task that doesn't need chain-of-thought. Without this,
         * EDRM thinking mode leaks from the main task into reflection. */
        int saved_thinking = ctx->provider->cfg.enable_thinking;
        ctx->provider->cfg.enable_thinking = 0;

        /* Mini react loop for reflection (max 4 steps) */
        for (int rstep = 0; rstep < (ctx->tools->cfg ? ctx->tools->cfg->max_reflection_steps : 4); rstep++) {
            llm_stats_t rstats = {0};
            int max_resp = ctx->tools->cfg ? ctx->tools->cfg->llm_max_response : 10*1024*1024;
            int rep_thresh = ctx->tools->cfg ? ctx->tools->cfg->llm_repeat_threshold : 100;
            char *rresp = provider_complete_stream(ctx->provider, reflect, &rstats,
                    NULL, NULL, max_resp, rep_thresh, NULL, NULL);
            if (!rresp) break;

            cJSON *raction = llm_parse_action(rresp, NULL);
            if (!raction) { free(rresp); break; }

            const char *ract = NULL;
            cJSON *act_item = cJSON_GetObjectItemCaseSensitive(raction, "action");
            if (act_item && cJSON_IsString(act_item)) ract = act_item->valuestring;

            if (!ract || strcmp(ract, "done") == 0) {
                cJSON_Delete(raction);
                free(rresp);
                break;
            }

            int should_store = 1;  /* declared outside if-block for use in feedback message */
            if (strcmp(ract, "memory_store") == 0) {
                /* FIX #4+B4: Deduplication guard — check if a very similar memory
                 * already exists before storing. This prevents reflection from
                 * creating near-duplicate entries on every task.
                 * B4 fix: Load existing entry's cached .emb file directly instead
                 * of calling memory_query() (which generates a query embedding)
                 * and then re-embedding the existing entry. Saves 2 API calls. */
                cJSON *rkey_j = cJSON_GetObjectItem(raction, "key");
                cJSON *rval_j = cJSON_GetObjectItem(raction, "value");
                if (rkey_j && rkey_j->valuestring && rval_j && rval_j->valuestring &&
                    memory_has_embeddings(ctx->tools->memory)) {
                    embed_ctx_t *emb = memory_embed_ctx(ctx->tools->memory);
                    /* FIX BUG2: Generate a multi-vec embedding (1 chunk) so the
                     * dedup guard uses embed_cosine_sim_multi_multi — the same
                     * similarity function as consolidation_cb in tools.c.
                     * Previously used embed_text (single vec) which produces a
                     * different similarity metric than consolidation. */
                    int mic = embed_max_input_chars(emb);
                    char *prep = embed_prepare_text(rkey_j->valuestring,
                                                     rval_j->valuestring,
                                                     mic);
                    if (prep) {
                        embed_vec_t single = embed_text(emb, prep);
                        free(prep);
                        if (single.data) {
                            /* Wrap single vec into 1-chunk multi-vec */
                            embed_multi_vec_t new_emb = {
                                .data = single.data,
                                .dim = single.dim,
                                .n_chunks = 1,
                            };
                            /* Scan in-memory index for high similarity
                             * instead of loading every .emb file from disk.
                             * O(N) comparisons with zero I/O. */
                            reflection_dedup_index_scan(
                                ctx, rkey_j, &new_emb,
                                &should_store, task_succeeded);
                            embed_vec_free(&single);
                            /* FIX B5: Nullify aliased pointer to prevent
                             * use-after-free if new_emb is accessed later. */
                            new_emb.data = NULL;
                        }
                    }
                }

                if (should_store) {
                    tool_result_t tr = tool_execute(ctx->tools, "memory_store", raction);
                    /* Emit event so frontend can show it */
                    react_event_t ev = {0};
                    ev.react_loop = ctx->tools->react_loop;
                    ev.type = REACT_EVENT_STEP_COMPLETE;
                    ev.action = "memory_store";
                    ev.description = "[reflection]";
                    react_emit(on_event, userdata, &ev);
                    tool_result_free(&tr);
                }
            }

            llm_chat_add(reflect, "assistant", rresp);
            llm_chat_add(reflect, "user",
                should_store
                    ? "Stored. Any more causal insights? What other assumptions, "
                      "hidden variables, or invariants should be captured? "
                      "Call memory_store or done."
                    : "Skipped (too similar to existing memory). Any other "
                      "causal insights? Call memory_store or done.");
            cJSON_Delete(raction);
            free(rresp);
        }
        llm_chat_free(reflect);

        /* FIX #9: Restore thinking mode after reflection */
        ctx->provider->cfg.enable_thinking = saved_thinking;
    }

    /* P4 scratchpad-to-memory promotion REMOVED (v4 unified memory).
     * Automatic promotion was identified as a source of memory pollution —
     * it created low-quality fact: entries from session-specific scratchpad
     * content. In v4, memory grows only through explicit agent/user action
     * or LLM reflection. Session history provides the "long tail" — anything
     * not worth a permanent memory entry is still findable via session search. */

    /* Post-reflection scratchpad pruning — remove solved/stale data so the
     * next react loop starts with a clean, focused scratchpad. Uses an LLM call
     * to intelligently merge and prune instead of blind accumulation.
     *
     * NOTE: The done result (R<N>_result section) is excluded from pruning.
     * It is preserved for cross-loop follow-ups via result.txt. The LLM pruning
     * should only remove task-specific working notes, not the final result. */
    if (ctx->flags.enable_pruning && final_result && ctx->tools->scratch.count > 0) {
        /* Extract the R<N>_result section to preserve it across pruning */
        char result_sec_name[32];
        snprintf(result_sec_name, sizeof(result_sec_name), "R%d_result",
                 ctx->tools->react_loop);
        char *preserved_result = NULL;
        int preserved_priority = 1;

        /* Save original priorities so we can restore them after LLM pruning.
         * The LLM sees serialized text (## headers) but not the priority
         * metadata, so we must reattach priorities after parsing its output. */
        typedef struct { char name[256]; int priority; } sec_pri_t;
        int n_orig = ctx->tools->scratch.count;
        sec_pri_t *orig_priorities = calloc((size_t)(n_orig > 0 ? n_orig : 1), sizeof(sec_pri_t));
        for (int i = 0; i < n_orig; i++) {
            size_t nlen = strlen(ctx->tools->scratch.sections[i].name);
            if (nlen >= sizeof(orig_priorities[0].name))
                nlen = sizeof(orig_priorities[0].name) - 1;
            memcpy(orig_priorities[i].name,
                   ctx->tools->scratch.sections[i].name, nlen);
            orig_priorities[i].name[nlen] = '\0';
            orig_priorities[i].priority = ctx->tools->scratch.sections[i].priority;
        }

        for (int i = 0; i < ctx->tools->scratch.count; i++) {
            if (strcmp(ctx->tools->scratch.sections[i].name, result_sec_name) == 0) {
                preserved_result = strdup(ctx->tools->scratch.sections[i].content);
                preserved_priority = ctx->tools->scratch.sections[i].priority;
                break;
            }
        }

        char *full_sp = scratchpad_serialize(&ctx->tools->scratch);

        if (full_sp && strlen(full_sp) > 0) {
            str_t prune_prompt = str_new(strlen(full_sp) + strlen(final_result) + 2048);
            str_appendf(&prune_prompt,
                "A task just completed. Remove ONLY information from the scratchpad "
                "that was resolved or completed by this task. Keep everything else "
                "exactly as-is — do not rewrite, merge, summarize, or reformat.\n\n"
                "Task result:\n"
                "---\n%s\n---\n\n"
                "Current scratchpad:\n"
                "---\n%s\n---\n\n"
                "IMPORTANT: Preserve the section named \"%s\" (the task result). "
                "Do NOT remove or modify it.\n"
                "Preserve ALL \"## section_name\" headers for sections you keep. "
                "Remove an entire section (header + body) only if fully resolved.\n"
                "Output the scratchpad with resolved items removed, nothing else changed.\n",
                final_result, full_sp, result_sec_name);

            llm_chat_t *prune_chat = llm_chat_new();
            llm_chat_add(prune_chat, "user", str_cstr(&prune_prompt));
            char *raw_cleaned = provider_complete(ctx->provider, prune_chat, NULL);
            llm_chat_free(prune_chat);
            str_free(&prune_prompt);

            /* Extract text from LLM output — accepts both plain markdown
             * and JSON tool-call format (extracts "content" field). */
            char *cleaned = react_extract_llm_text_output(raw_cleaned);
            free(raw_cleaned);

            if (cleaned) {
                /* BUG FIX: Parse the LLM output back into individual sections
                 * instead of merging everything into a single "pruned" blob.
                 * scratchpad_parse() splits on "## " headers (the format
                 * scratchpad_serialize() produces), preserving the section-based
                 * API contract. If the LLM stripped all headers, falls back to
                 * a single "pruned" section. */
                scratchpad_parse(&ctx->tools->scratch, cleaned, "pruned", 5);

                /* FIX DESIGN2: Validate LLM output preserved section structure.
                 * If the LLM dropped all ## headers, scratchpad_parse collapses
                 * everything into a single "pruned" section — destroying the
                 * original section boundaries. When this happens (original had
                 * multiple sections but result is 1 "pruned" blob), revert to
                 * the original scratchpad to prevent data loss. */
                if (ctx->tools->scratch.count == 1 && n_orig > 1 &&
                    strcmp(ctx->tools->scratch.sections[0].name, "pruned") == 0) {
                    /* LLM stripped all headers — revert to original */
                    scratchpad_parse(&ctx->tools->scratch, full_sp, "pruned", 5);
                }

                /* Restore original priorities for sections that survived.
                 * The LLM doesn't see priority metadata, so we reattach it. */
                for (int i = 0; i < ctx->tools->scratch.count; i++) {
                    for (int j = 0; j < n_orig; j++) {
                        if (strcmp(ctx->tools->scratch.sections[i].name,
                                   orig_priorities[j].name) == 0) {
                            ctx->tools->scratch.sections[i].priority =
                                orig_priorities[j].priority;
                            break;
                        }
                    }
                }

                /* FIX D7: Restore high-priority sections the LLM silently dropped.
                 * The LLM can subtly corrupt the scratchpad by omitting sections
                 * it shouldn't remove.  Re-add any original section with priority ≤ 2
                 * that is missing from the pruned output — these are important enough
                 * that the LLM should never unilaterally remove them. */
                for (int j = 0; j < n_orig; j++) {
                    if (orig_priorities[j].priority > 2) continue;
                    int found = 0;
                    for (int i = 0; i < ctx->tools->scratch.count; i++) {
                        if (strcmp(ctx->tools->scratch.sections[i].name,
                                   orig_priorities[j].name) == 0) {
                            found = 1;
                            break;
                        }
                    }
                    if (!found) {
                        /* Find the original content from full_sp and restore it */
                        char header[270];
                        snprintf(header, sizeof(header), "## %s\n",
                                 orig_priorities[j].name);
                        const char *sec_start = strstr(full_sp, header);
                        if (sec_start) {
                            const char *body = sec_start + strlen(header);
                            const char *sec_end = strstr(body, "\n## ");
                            size_t blen = sec_end
                                ? (size_t)(sec_end - body)
                                : strlen(body);
                            char *restored = malloc(blen + 1);
                            if (restored) {
                                memcpy(restored, body, blen);
                                restored[blen] = '\0';
                                scratchpad_write(&ctx->tools->scratch,
                                    orig_priorities[j].name, restored,
                                    orig_priorities[j].priority);
                                free(restored);
                            }
                        }
                    }
                }
                /* Re-add the preserved result section so it survives pruning.
                 * If scratchpad_parse() already parsed it from LLM output,
                 * scratchpad_write() will overwrite with the original content
                 * (safer than trusting the LLM's copy). */
                if (preserved_result) {
                    scratchpad_write(&ctx->tools->scratch, result_sec_name,
                                     preserved_result, preserved_priority);
                }
                scratchpad_save(&ctx->tools->scratch, ctx->tools->session_dir);
            }
            free(cleaned);
        }
        free(orig_priorities);
        free(preserved_result);
        free(full_sp);
    }

    /* ── Session summary + chunk generation (v4.1 unified memory) ────
     * At session end:
     * 1. Write summary.txt (journal manifest) for human-readable display
     * 2. Write summary.emb (single embedding of manifest) for legacy compat
     * 3. Write chunks.emb + chunks.idx (per-chunk embeddings of agent
     *    thoughts + tool outputs) for granular MaxSim retrieval
     * 4. Add to in-memory session index for immediate searchability */
    if (ctx->tools->journal && ctx->tools->session_dir) {
        char *manifest = journal_manifest(ctx->tools->journal, 0);
        if (manifest && strlen(manifest) > 30) {
            /* Write summary.txt */
            char spath[PATH_MAX];
            snprintf(spath, sizeof(spath), "%s/summary.txt",
                     ctx->tools->session_dir);
            FILE *sf = fopen(spath, "w");
            if (sf) {
                fputs(manifest, sf);
                fclose(sf);
            }

            if (ctx->tools->memory && memory_has_embeddings(ctx->tools->memory)) {
                embed_ctx_t *embed = memory_embed_ctx(ctx->tools->memory);
                if (embed) {
                    /* Write summary.emb (legacy single-vector) */
                    embed_vec_t emb = embed_text(embed, manifest);
                    if (emb.data) {
                        char epath[PATH_MAX];
                        snprintf(epath, sizeof(epath), "%s/summary.emb",
                                 ctx->tools->session_dir);
                        embed_vec_save(&emb, epath);
                        embed_vec_free(&emb);
                    }

                    /* v4.1: Generate per-chunk embeddings */
                    journal_chunks_t jc = journal_extract_chunks(
                        ctx->tools->session_dir, 900, 50);
                    if (jc.n_chunks > 0) {
                        int out_count = 0;
                        embed_vec_t *vecs = embed_text_batch(embed,
                            (const char **)jc.texts, jc.n_chunks, &out_count);

                        if (vecs && out_count > 0) {
                            /* Build multi-vector */
                            int dim = 0, valid = 0;
                            for (int ci = 0; ci < out_count; ci++) {
                                if (vecs[ci].data) {
                                    if (dim == 0) dim = vecs[ci].dim;
                                    valid++;
                                }
                            }

                            if (valid > 0 && dim > 0) {
                                embed_multi_vec_t mv = {0};
                                mv.dim = dim;
                                mv.n_chunks = valid;
                                mv.data = malloc((size_t)dim * (size_t)valid * sizeof(float));
                                char **previews = calloc((size_t)valid, sizeof(char *));

                                if (mv.data && previews) {
                                    int vi = 0;
                                    for (int ci = 0; ci < out_count && vi < valid; ci++) {
                                        if (!vecs[ci].data) continue;
                                        memcpy(mv.data + vi * dim, vecs[ci].data,
                                               (size_t)dim * sizeof(float));
                                        /* Build preview: skip prefix line */
                                        const char *body = strchr(jc.texts[ci], '\n');
                                        if (body) body++; else body = jc.texts[ci];
                                        char pbuf[201];
                                        size_t plen = strlen(body);
                                        if (plen > 200) plen = 200;
                                        while (plen > 0 && ((unsigned char)body[plen] & 0xC0) == 0x80) plen--;
                                        memcpy(pbuf, body, plen);
                                        pbuf[plen] = '\0';
                                        previews[vi] = strdup(pbuf);
                                        vi++;
                                    }

                                    /* Save chunks.emb */
                                    char cepath[PATH_MAX];
                                    snprintf(cepath, sizeof(cepath), "%s/chunks.emb",
                                             ctx->tools->session_dir);
                                    embed_multi_vec_save(&mv, cepath);

                                    /* Save chunks.idx */
                                    char cipath[PATH_MAX];
                                    snprintf(cipath, sizeof(cipath), "%s/chunks.idx",
                                             ctx->tools->session_dir);
                                    /* Write JSONL index */
                                    FILE *idxf = fopen(cipath, "w");
                                    if (idxf) {
                                        for (int pi = 0; pi < valid; pi++) {
                                            cJSON *js = cJSON_CreateString(previews[pi] ? previews[pi] : "");
                                            char *esc = cJSON_PrintUnformatted(js);
                                            fprintf(idxf, "{\"id\":%d,\"preview\":%s}\n", pi, esc);
                                            free(esc);
                                            cJSON_Delete(js);
                                        }
                                        fclose(idxf);
                                    }

                                    /* Add to in-memory session index */
                                    if (ctx->tools->session_idx) {
                                        session_index_add(ctx->tools->session_idx,
                                            ctx->tools->session_dir, manifest,
                                            NULL, &mv, previews, valid);
                                    }
                                }

                                for (int pi = 0; pi < valid; pi++) free(previews[pi]);
                                free(previews);
                                embed_multi_vec_free(&mv);
                            }

                            for (int ci = 0; ci < out_count; ci++)
                                embed_vec_free(&vecs[ci]);
                            free(vecs);
                        }
                        journal_chunks_free(&jc);
                    }
                }
            }
        }
        free(manifest);
    }

    /* FIX D2: recalled_keys cleanup is handled exclusively by react_run()
     * after react_post_loop() returns.  Removing the partial cleanup here
     * (which freed keys but not the array) eliminates the split ownership
     * ambiguity and latent double-free risk. */
}
