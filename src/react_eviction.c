/* react_eviction.c — Progressive context eviction (Harness-1 §3.5).
 * Extracted from react.c to reduce file size.
 * Implements multi-pass importance-aware context rendering:
 *   Pass 1: Strip LOW importance messages
 *   Pass 2: Compress NORMAL messages to top-4 sentences (BM25)
 *   Pass 3: Recoverability-aware eviction with breadcrumb generation
 *
 * Bug/Design fixes applied:
 *   BUG 1:  Pair-eviction now checks floor for both primary and partner
 *   BUG 2:  Pass 3 skips HIGH+ (not just CRITICAL) — aligns with emergency
 *   BUG 3:  Stale EVICTION_SUMMARY/SCRATCHPAD/MEMORY_HINT cleanup runs
 *           unconditionally when eviction triggers, not only inside Pass 3
 *   BUG 7:  context_budget uses long instead of int
 *   BUG 9:  Explicit guard against n_evictable <= 0 (no calloc(0))
 *   DESIGN 1: Post-eviction budget verification loop
 *   DESIGN 2: Eviction targets a low-watermark (eviction_pct - 10)
 *   DESIGN 3: Pass 2 compresses largest messages first
 *   DESIGN 5: Pass 1 already uses reverse removal (was previously fixed)
 *   DESIGN 6: Score formula normalizes position to not overwhelm size_bonus
 *   DESIGN 7: Breadcrumb string capped at 4096 chars
 *   DESIGN 10: Low-watermark provides hysteresis (trigger@pct, target@pct-10)
 */

#include "react_internal.h"
#include "compress.h"

/* ── Progressive Context Eviction ──────────────────────── */

/* Helper: recalculate total chars in chat. */
static long react_calc_total_chars(const llm_chat_t *chat) {
    long total = 0;
    for (int i = 0; i < chat->n_msgs; i++)
        if (chat->msgs[i].content)
            total += (long)strlen(chat->msgs[i].content);
    return total;
}

/* Helper: re-inject scratchpad at insert_pos in chat.
 * Returns the serialized scratchpad size in chars (0 if nothing injected).
 * Shared between progressive eviction (BUG A FIX) and emergency eviction
 * (BUG C FIX) to ensure scratchpad is always restored after any eviction. */
long react_reinject_scratchpad(react_ctx_t *ctx, llm_chat_t *chat,
                               int insert_pos) {
    double cpt = (double)react_get_chars_per_token(ctx);
    long context_budget = (ctx->provider->cfg.context_size > 0)
        ? (long)(ctx->provider->cfg.context_size * cpt) : 0;

    /* D1 FIX: Cap scratchpad to the LESSER of:
     *   - SCRATCHPAD_BUDGET_PCT% of total context (absolute cap)
     *   - SCRATCHPAD_MAX_OF_REMAINING_PCT% of post-eviction content
     * This prevents scratchpad from drowning out conversation after
     * heavy eviction where 15% of total >> remaining conversation. */
    size_t sp_max;
    if (context_budget > 0) {
        size_t abs_cap = (size_t)(context_budget
                                 * REACT_SCRATCHPAD_BUDGET_PCT / 100);
        /* Calculate remaining chars to bound scratchpad proportionally */
        long remaining = 0;
        for (int i = 0; i < chat->n_msgs; i++)
            if (chat->msgs[i].content)
                remaining += (long)strlen(chat->msgs[i].content);
        size_t rel_cap = (size_t)(remaining
                                 * REACT_SCRATCHPAD_MAX_OF_REMAINING_PCT / 100);
        sp_max = abs_cap < rel_cap ? abs_cap : rel_cap;
        if (sp_max < 2048) sp_max = 2048; /* reasonable minimum */
    } else {
        sp_max = 8192;
    }

    char *fresh_sp = scratchpad_serialize_budget(&ctx->tools->scratch, sp_max);
    long injected_chars = 0;
    if (fresh_sp && fresh_sp[0]) {
        size_t slen = strlen(fresh_sp);
        char *sp_msg = malloc(slen + 32);
        if (sp_msg) {
            snprintf(sp_msg, slen + 32, "[SCRATCHPAD]\n%s", fresh_sp);
            llm_chat_insert_typed(chat, insert_pos,
                "user", sp_msg, LLM_MSG_SCRATCHPAD);
            injected_chars = (long)strlen(sp_msg);
            free(sp_msg);
        }
    }
    free(fresh_sp);
    return injected_chars;
}

/* Check context usage and evict old messages if over threshold.
 * Includes importance-aware multi-pass eviction, pair-safe boundaries,
 * scratchpad re-injection, and breadcrumb generation (LCM-Lite). */
void react_maybe_evict(react_ctx_t *ctx, llm_chat_t *chat, int step,
                       const char *user_query,
                       react_event_fn on_event, void *userdata) {

    if (!ctx->flags.enable_compaction || ctx->provider->cfg.context_size <= 0)
        return;

    /* D7 FIX: Build enriched BM25 query incorporating user_query AND recent
     * agent thoughts/scratchpad. Without this, BM25 compression retains chunks
     * relevant to the ORIGINAL question, not the agent's current investigation.
     * For long tasks where focus shifts, this drastically improves retention. */
    str_t bm25_buf = str_new(1024);
    if (user_query && user_query[0])
        str_append_cstr(&bm25_buf, user_query);
    /* Fallback: extract query from chat if user_query is short/empty */
    if (bm25_buf.len < 10) {
        for (int i = 0; i < chat->n_msgs; i++) {
            if (chat->msgs[i].msg_type == LLM_MSG_USER_QUERY &&
                chat->msgs[i].content && strlen(chat->msgs[i].content) >= 10) {
                str_append_cstr(&bm25_buf, chat->msgs[i].content);
                break;
            }
        }
    }
    /* Augment with recent assistant thoughts (last 3 exchanges) */
    {
        int thought_count = 0;
        for (int i = chat->n_msgs - 1; i >= 0 && thought_count < 3; i--) {
            if (chat->msgs[i].role && strcmp(chat->msgs[i].role, "assistant") == 0 &&
                chat->msgs[i].content && strlen(chat->msgs[i].content) > 20) {
                str_append_cstr(&bm25_buf, " ");
                /* Only take first 200 chars of thought to avoid bloat */
                size_t tlen = strlen(chat->msgs[i].content);
                str_append(&bm25_buf, chat->msgs[i].content,
                           tlen > 200 ? 200 : tlen);
                thought_count++;
            }
        }
    }
    /* Augment with scratchpad content if available */
    if (ctx->tools->scratch.count > 0) {
        char *sp = scratchpad_serialize_budget(&ctx->tools->scratch, 500);
        if (sp && sp[0]) {
            str_append_cstr(&bm25_buf, " ");
            str_append_cstr(&bm25_buf, sp);
        }
        free(sp);
    }
    const char *bm25_query = str_cstr(&bm25_buf);

    double cpt_ev = (double)react_get_chars_per_token(ctx);
    long context_budget = (long)(ctx->provider->cfg.context_size * cpt_ev);
    int eviction_pct = ctx->tools->cfg ? ctx->tools->cfg->context_eviction_pct : 70;
    int hysteresis_gap = eviction_pct / REACT_HYSTERESIS_DIVISOR;
    if (hysteresis_gap < REACT_HYSTERESIS_MIN_GAP)
        hysteresis_gap = REACT_HYSTERESIS_MIN_GAP;
    int target_pct = eviction_pct - hysteresis_gap;
    /* H2/H3 FIX: Compute keep_head/keep_tail dynamically from actual chat
     * structure instead of hardcoded constants that assumed fixed header/tail. */
    int keep_head = react_compute_keep_head(chat);
    int keep_tail = react_compute_keep_tail(chat);

    /* Recalculate total chars */
    long total_chars = react_calc_total_chars(chat);
    int usage_pct = (context_budget > 0)
        ? (int)(100L * total_chars / context_budget) : 0;

    if (usage_pct <= eviction_pct || chat->n_msgs <= keep_head + keep_tail + 1)
        return;

    /* Capture pre-compaction state for journal logging */
    int before_msgs = chat->n_msgs;
    int before_pct = usage_pct;

    int did_evict = 0;
    long actual_sp_size = 0;  /* computed by Pass 2/3 for D4 proportional strip */
    long breadcrumb_cap = 1024;  /* default, recomputed when budget is known */

    /* BUG 3 FIX: Always clean stale injected messages when eviction triggers,
     * not only when Pass 3 fires. Previously, if Pass 1+2 were sufficient,
     * CRITICAL-importance eviction summaries from prior cycles persisted. */
    llm_chat_remove_by_type(chat, LLM_MSG_SCRATCHPAD);
    llm_chat_remove_by_type(chat, LLM_MSG_EVICTION_SUMMARY);
    llm_chat_remove_by_type(chat, LLM_MSG_MEMORY_HINT);
    /* Recalculate after cleanup */
    total_chars = react_calc_total_chars(chat);
    usage_pct = (context_budget > 0)
        ? (int)(100L * total_chars / context_budget) : 0;

    /* BUG A FIX: If cleanup alone brought us under target, re-inject the
     * scratchpad and return early. Previously, the scratchpad was removed
     * unconditionally above but only re-injected inside Pass 3's conditional
     * block — if Pass 1+2 (or even cleanup alone) were sufficient, the
     * scratchpad was silently lost from the LLM context. */
    if (usage_pct <= target_pct) {
        react_reinject_scratchpad(ctx, chat, keep_head);
        did_evict = 1; /* cleanup removed messages */
        goto post_eviction;
    }

    /* ── Pass 1: Strip LOW importance messages (errors, stale hints, deduped)
     * These have the least value and may actively degrade performance.
     * Remove in reverse order to avoid index shifting issues.
     * B1 FIX: Pair-safe — when removing a LOW message that is part of a
     * tool_call/tool_result pair, remove both halves to avoid orphaning. */
    for (int i = chat->n_msgs - keep_tail - 1; i >= keep_head; i--) {
        if (chat->msgs[i].importance == LLM_MSG_IMPORTANCE_LOW) {
            /* Check if this is a tool_result with a preceding tool_call */
            if (chat->msgs[i].tool_call_id && i > keep_head &&
                chat->msgs[i - 1].tool_calls_json) {
                llm_chat_remove_range(chat, i - 1, i + 1); /* remove pair */
                i--;  /* skip the partner we just removed */
            }
            /* Check if this is a tool_call with a following tool_result */
            else if (chat->msgs[i].tool_calls_json &&
                     i + 1 < chat->n_msgs - keep_tail &&
                     chat->msgs[i + 1].tool_call_id) {
                llm_chat_remove_range(chat, i, i + 2); /* remove pair */
            } else {
                llm_chat_remove_range(chat, i, i + 1);
            }
            did_evict = 1;
        }
    }
    if (did_evict) {
        total_chars = react_calc_total_chars(chat);
        usage_pct = (context_budget > 0)
            ? (int)(100L * total_chars / context_budget) : 0;
        /* BUG A FIX (cont.): If Pass 1 brought us under target, re-inject
         * scratchpad and skip remaining passes. */
        if (usage_pct <= target_pct) {
            react_reinject_scratchpad(ctx, chat, keep_head);
            goto post_eviction;
        }
    }

    /* D9 FIX: Compute re-injection estimate using the ACTUAL scratchpad size
     * (not worst-case budget). The scratchpad was removed above, so we can
     * measure what will actually be re-injected. Also uses proportional
     * breadcrumb cap (1/3 of scratchpad budget). */
    {
        char *sp_preview = scratchpad_serialize_budget(&ctx->tools->scratch, SIZE_MAX);
        if (sp_preview) {
            actual_sp_size = (long)strlen(sp_preview);
            free(sp_preview);
        }
    }
    /* Breadcrumb cap scales with scratchpad budget */
    breadcrumb_cap = (long)(context_budget * REACT_SCRATCHPAD_BUDGET_PCT / 300);
    if (breadcrumb_cap < 1024) breadcrumb_cap = 1024;
    long reinject_est = actual_sp_size + breadcrumb_cap + 200;
    /* Effective target accounts for re-injection headroom */
    int effective_target_pct = target_pct;
    if (context_budget > 0) {
        int reinject_pct = (int)(100L * reinject_est / context_budget);
        effective_target_pct = target_pct - reinject_pct;
        if (effective_target_pct < 10) effective_target_pct = 10;
    }

    /* H7 FIX: Dynamic compression threshold based on average message size.
     * Previously hardcoded at 500 chars — messages of 400 chars accumulated
     * unchecked. Now: threshold = max(200, average_msg_len / 2). Messages
     * below this are too small for BM25 to improve meaningfully. */
    int compress_threshold;
    {
        int n_middle = chat->n_msgs - keep_head - keep_tail;
        if (n_middle > 0) {
            long middle_chars = 0;
            for (int i = keep_head; i < chat->n_msgs - keep_tail; i++)
                if (chat->msgs[i].content)
                    middle_chars += (long)strlen(chat->msgs[i].content);
            compress_threshold = (int)(middle_chars / n_middle / 2);
            if (compress_threshold < 200) compress_threshold = 200;
        } else {
            compress_threshold = 200;
        }
    }

    /* ── Pass 2: Compress NORMAL messages via BM25 relevance scoring.
     * H1 FIX: Compression params scale with message size — larger messages
     * get proportionally more chunks/chars retained. Previously all messages
     * were compressed to 4 chunks / 400 chars regardless of original size.
     * B5 FIX: Guard against expansion — if BM25 re-scoring produces larger
     * output (can happen with previously compressed text), keep the original.
     * Sorted by length (largest first) for maximum space savings. */
    if (usage_pct > target_pct) {
        int n_candidates = 0;
        typedef struct { int idx; int len; } compress_cand_t;
        compress_cand_t *candidates = NULL;
        int cand_cap = 0;

        for (int i = keep_head; i < chat->n_msgs - keep_tail; i++) {
            if (chat->msgs[i].importance <= LLM_MSG_IMPORTANCE_NORMAL &&
                chat->msgs[i].content &&
                (int)strlen(chat->msgs[i].content) > compress_threshold) {
                if (n_candidates >= cand_cap) {
                    cand_cap = cand_cap ? cand_cap * 2 : 16;
                    compress_cand_t *tmp = realloc(candidates,
                        (size_t)cand_cap * sizeof(compress_cand_t));
                    if (!tmp) break;
                    candidates = tmp;
                }
                candidates[n_candidates].idx = i;
                candidates[n_candidates].len = (int)strlen(chat->msgs[i].content);
                n_candidates++;
            }
        }
        /* Sort by length descending (largest first for most space savings) */
        for (int i = 1; i < n_candidates; i++) {
            compress_cand_t key = candidates[i];
            int j = i - 1;
            while (j >= 0 && candidates[j].len < key.len) {
                candidates[j + 1] = candidates[j];
                j--;
            }
            candidates[j + 1] = key;
        }

        for (int ci = 0; ci < n_candidates; ci++) {
            int i = candidates[ci].idx;
            int old_len = (int)strlen(chat->msgs[i].content);
            /* H1 FIX: Scale compression params with message size */
            int scaled_units = old_len / 1000;
            if (scaled_units < 4) scaled_units = 4;
            int scaled_chars = old_len / 4;
            if (scaled_chars < 400) scaled_chars = 400;
            char *compressed = compress_to_relevant(
                chat->msgs[i].content, bm25_query,
                scaled_units, scaled_chars);
            if (compressed) {
                int new_len = (int)strlen(compressed);
                /* B5 FIX: Only apply if compression actually reduced size */
                if (new_len < old_len) {
                    free(chat->msgs[i].content);
                    chat->msgs[i].content = compressed;
                    total_chars -= (old_len - new_len);
                    did_evict = 1;
                } else {
                    free(compressed);  /* discard — expansion would increase context */
                }
                usage_pct = (context_budget > 0)
                    ? (int)(100L * total_chars / context_budget) : 0;
                if (usage_pct <= effective_target_pct)
                    break;
            }
        }
        free(candidates);
    }

    /* ── Pass 3: Recoverability-aware eviction of NORMAL middle messages.
     * CWL [arXiv:2606.11213]: Sort evictable messages so those with
     * higher recoverability (content persisted elsewhere) are evicted
     * FIRST — they can be recovered via file_read.
     * LCM-Lite [arXiv:2605.04050]: Generate a breadcrumb index of
     * evicted store refs so the agent knows how to recover content.
     * Uses target_pct (low watermark) for hysteresis. */
    if (usage_pct > target_pct) {
        int evict_start = keep_head;
        int evict_end = chat->n_msgs - keep_tail;

        /* FLAW 3 FIX: Adjust eviction boundary to not split tool_call/result
         * pairs. Now tries to ADVANCE past a complete pair first (including
         * it in the eviction pool) before retreating. Previously only
         * decremented evict_end, which could shrink the pool significantly
         * when the tail boundary fell in a chain of tool_call/result pairs. */
        for (int adj_iter = 0; evict_end > evict_start && adj_iter < 20; adj_iter++) {
            if (evict_end < chat->n_msgs && chat->msgs[evict_end].tool_call_id) {
                /* We're at a tool_result — its paired tool_call is at evict_end-1.
                 * Try advancing past this result to include the complete pair. */
                if (evict_end + 1 <= chat->n_msgs - keep_tail) {
                    evict_end++; /* include the tool_result in eviction pool */
                } else {
                    evict_end--; /* can't advance — retreat past the tool_call too */
                }
                continue;
            }
            if (evict_end - 1 >= evict_start &&
                chat->msgs[evict_end - 1].tool_calls_json) {
                /* The last evictable msg is a tool_call with no result in pool.
                 * Try advancing to include the result. */
                if (evict_end + 1 <= chat->n_msgs - keep_tail) {
                    evict_end++; /* include the tool_result */
                } else {
                    evict_end--; /* can't advance — exclude the orphan tool_call */
                }
                continue;
            }
            break;
        }

        int n_evictable = evict_end - evict_start;
        /* BUG 9 FIX: Explicit guard — skip scoring when range is empty.
         * Previously calloc(0, ...) had implementation-defined behavior. */
        if (n_evictable > 0) {
            int *evict_mark = calloc((size_t)n_evictable, sizeof(int));
            int n_to_evict = 0;

            if (evict_mark) {
                typedef struct { int idx; int score; } evict_scored_t;
                evict_scored_t *scored = malloc((size_t)n_evictable * sizeof(evict_scored_t));
                if (scored) {
                    for (int i = 0; i < n_evictable; i++) {
                        int mi = evict_start + i;
                        int imp = (int)chat->msgs[mi].importance;
                        int rec = (int)chat->msgs[mi].recoverability;
                        int msg_len = chat->msgs[mi].content
                            ? (int)strlen(chat->msgs[mi].content) : 0;
                        scored[i].idx = i;
                        int pos_norm = (n_evictable > 1)
                            ? (i * 19 / (n_evictable - 1)) : 0;
                        int size_bonus = (rec > 0 && msg_len > 200)
                            ? (msg_len / 500) * rec : 0;
                        /* B2 FIX: Clamp size_bonus so it never crosses an
                         * importance tier boundary (100 points). Without this,
                         * a large recoverable NORMAL message could score below
                         * a LOW message, violating the invariant that importance
                         * tiers are inviolable eviction boundaries. */
                        if (size_bonus > 90) size_bonus = 90;
                        scored[i].score = imp * 100 - rec * 10 - size_bonus + pos_norm;
                    }
                    /* Sort by score ascending (lowest = evict first) */
                    for (int i = 1; i < n_evictable; i++) {
                        evict_scored_t key = scored[i];
                        int j = i - 1;
                        while (j >= 0 && scored[j].score > key.score) {
                            scored[j + 1] = scored[j];
                            j--;
                        }
                        scored[j + 1] = key;
                    }

                    /* Compaction floor: keep at least FLOOR_PCT% of non-head context.
                     * Uses long to avoid int overflow for large contexts. */
                    long head_chars = 0;
                    for (int ki = 0; ki < evict_start; ki++)
                        if (chat->msgs[ki].content)
                            head_chars += (long)strlen(chat->msgs[ki].content);
                    long floor_chars = (context_budget - head_chars)
                                     * REACT_EVICT_FLOOR_PCT / 100;
                    if (floor_chars < REACT_EVICT_FLOOR_MIN_CHARS)
                        floor_chars = REACT_EVICT_FLOOR_MIN_CHARS;
                    /* D5 FIX: Don't add reinject_est to floor. The scratchpad
                     * was already removed before eviction, so effective_target_pct
                     * (which accounts for re-injection) handles the headroom.
                     * Adding it to floor double-counted, making the floor ~15%
                     * too generous and reducing effective eviction budget. */

                    long tail_chars = 0;
                    for (int ki = evict_end; ki < chat->n_msgs; ki++)
                        if (chat->msgs[ki].content)
                            tail_chars += (long)strlen(chat->msgs[ki].content);

                    long evictable_chars = 0;
                    for (int ki = evict_start; ki < evict_end; ki++)
                        if (chat->msgs[ki].content)
                            evictable_chars += (long)strlen(chat->msgs[ki].content);

                    long remaining_chars = tail_chars + evictable_chars;
                    for (int si = 0; si < n_evictable; si++) {
                        int ri = scored[si].idx;
                        int mi = evict_start + ri;
                        long msg_chars = chat->msgs[mi].content
                            ? (long)strlen(chat->msgs[mi].content) : 0;

                        if (evict_mark[ri]) continue;

                        /* Skip HIGH and CRITICAL messages. */
                        if (chat->msgs[mi].importance >= LLM_MSG_IMPORTANCE_HIGH)
                            continue;

                        /* Compaction floor check */
                        if (remaining_chars - msg_chars < floor_chars)
                            break;

                        /* D6 FIX: Pair-safety — find partner by scanning rather
                         * than assuming adjacency. Previously only checked ri±1,
                         * which breaks if messages were inserted between a
                         * tool_call and its tool_result (error recovery, hints). */
                        int pair_ri = -1;
                        long pair_chars = 0;

                        if (chat->msgs[mi].tool_calls_json) {
                            /* Assistant with tool_calls: find matching tool_result
                             * by scanning forward. D8 FIX: Don't stop at non-tool
                             * messages — error recovery hints, multi-tool corrections
                             * can be inserted between tool_call and tool_result. */
                            for (int pi = ri + 1; pi < n_evictable; pi++) {
                                int pmi = evict_start + pi;
                                if (chat->msgs[pmi].tool_call_id && !evict_mark[pi]) {
                                    if (chat->msgs[pmi].importance >= LLM_MSG_IMPORTANCE_HIGH)
                                        break;  /* can't evict partner */
                                    pair_ri = pi;
                                    pair_chars = chat->msgs[pmi].content
                                        ? (long)strlen(chat->msgs[pmi].content) : 0;
                                    break;
                                }
                                /* D8 FIX: Continue scanning past non-tool messages
                                 * (hints, error recovery) — they may sit between
                                 * a tool_call and its tool_result. */
                            }
                        }
                        if (chat->msgs[mi].tool_call_id) {
                            /* Tool_result: find matching tool_call by scanning backward. */
                            for (int pi = ri - 1; pi >= 0; pi--) {
                                int pmi = evict_start + pi;
                                if (chat->msgs[pmi].tool_calls_json && !evict_mark[pi]) {
                                    if (chat->msgs[pmi].importance >= LLM_MSG_IMPORTANCE_HIGH)
                                        break;
                                    pair_ri = pi;
                                    pair_chars = chat->msgs[pmi].content
                                        ? (long)strlen(chat->msgs[pmi].content) : 0;
                                    break;
                                }
                                /* D8 FIX: Continue scanning past non-tool messages */
                            }
                        }

                        /* Check floor for total pair cost */
                        long total_pair_chars = msg_chars + pair_chars;
                        if (remaining_chars - total_pair_chars < floor_chars)
                            break;

                        /* Mark primary for eviction */
                        evict_mark[ri] = 1;
                        remaining_chars -= msg_chars;
                        n_to_evict++;

                        /* Mark pair partner if found */
                        if (pair_ri >= 0) {
                            evict_mark[pair_ri] = 1;
                            remaining_chars -= pair_chars;
                            n_to_evict++;
                        }
                    }
                    if (n_to_evict > 0) did_evict = 1;
                    free(scored);
                }
            }

            if (n_to_evict > 0 && evict_mark) {
                /* LCM-Lite: Build breadcrumb index of evicted store refs. */
                str_t breadcrumb = str_new(512);
                {
                    size_t sp_budget = (size_t)(context_budget * REACT_SCRATCHPAD_BUDGET_PCT / 100);
                    size_t summary_budget = sp_budget / 3;
                    if (summary_budget > (size_t)breadcrumb_cap)
                        summary_budget = (size_t)breadcrumb_cap;
                    str_t summary = str_new(summary_budget);
                    int max_per_msg = n_to_evict > 0
                        ? (int)(summary_budget / (unsigned)n_to_evict) : 200;
                    if (max_per_msg < 200) max_per_msg = 200;
                    if (max_per_msg > 1000) max_per_msg = 1000;

                    str_append_cstr(&breadcrumb,
                        "[EVICTED CONTEXT — recoverable via file_read]\n");
                    int n_breadcrumbs = 0;

                    /* H6 FIX: Dynamic seen_aliases array — grows as needed
                     * instead of fixed 64 entries that silently stopped
                     * deduplicating in sessions with many eviction cycles. */
                    int seen_cap = n_to_evict > 16 ? n_to_evict : 16;
                    const char **seen_aliases = malloc((size_t)seen_cap * sizeof(const char *));
                    int n_seen = 0;

                    for (int ei = 0; ei < n_evictable; ei++) {
                        if (!evict_mark[ei]) continue;
                        int mi = evict_start + ei;
                        const char *content = chat->msgs[mi].content;
                        const char *role = chat->msgs[mi].role;
                        if (!content || !content[0] || !role) continue;
                        if (strcmp(role, "system") == 0) continue;

                        if (chat->msgs[mi].store_alias) {
                            /* Cap breadcrumb size */
                            if ((int)breadcrumb.len >= breadcrumb_cap) continue;

                            /* Skip duplicate aliases */
                            int dup = 0;
                            if (seen_aliases) {
                                for (int di = 0; di < n_seen; di++) {
                                    if (strcmp(seen_aliases[di], chat->msgs[mi].store_alias) == 0) {
                                        dup = 1; break;
                                    }
                                }
                            }
                            if (dup) continue;
                            if (seen_aliases) {
                                if (n_seen >= seen_cap) {
                                    int new_cap = seen_cap * 2;
                                    const char **tmp = realloc(seen_aliases,
                                        (size_t)new_cap * sizeof(const char *));
                                    if (tmp) { seen_aliases = tmp; seen_cap = new_cap; }
                                }
                                if (n_seen < seen_cap)
                                    seen_aliases[n_seen++] = chat->msgs[mi].store_alias;
                            }

                            char brief[81];
                            int blen = (int)strlen(content);
                            if (blen > 80) blen = 80;
                            memcpy(brief, content, (size_t)blen);
                            brief[blen] = '\0';
                            for (int b = 0; brief[b]; b++)
                                if (brief[b] == '\n' || brief[b] == '\r')
                                    brief[b] = ' ';
                            str_appendf(&breadcrumb, "- %s: %s (%s, %d chars)\n",
                                chat->msgs[mi].store_alias, brief, role,
                                (int)strlen(content));
                            n_breadcrumbs++;
                            continue;
                        }

                        if (strcmp(role, "tool") == 0 && strlen(content) < 50) continue;
                        int clen = (int)strlen(content);
                        if (clen > max_per_msg) clen = max_per_msg;
                        str_appendf(&summary, "[%s]: ", role);
                        str_append(&summary, content, (size_t)clen);
                        if ((int)strlen(content) > max_per_msg)
                            str_append_cstr(&summary, "...[truncated]");
                        str_append_cstr(&summary, "\n");
                        if (summary.len >= summary_budget) break;
                    }

                    if (n_breadcrumbs == 0) {
                        str_free(&breadcrumb);
                        breadcrumb = str_new(0);
                    }

                    if (summary.len > 0) {
                        char *summ_str = str_steal(&summary);
                        scratchpad_write(&ctx->tools->scratch, "evicted_context",
                                         summ_str, 2);
                        scratchpad_save(&ctx->tools->scratch, ctx->tools->session_dir);
                        free(summ_str);
                    } else {
                        str_free(&summary);
                    }
                    free(seen_aliases);
                }

                /* Remove marked messages in REVERSE order to preserve indices. */
                for (int ri = n_evictable - 1; ri >= 0; ri--) {
                    if (evict_mark[ri])
                        llm_chat_remove_range(chat, evict_start + ri, evict_start + ri + 1);
                }

                /* Recover tool_call threading from surviving messages */
                free(chat->last_tool_call_id);
                chat->last_tool_call_id = NULL;
                free(chat->last_tool_calls_json);
                chat->last_tool_calls_json = NULL;
                for (int ri = chat->n_msgs - 1; ri >= 0; ri--) {
                    if (chat->msgs[ri].tool_calls_json) {
                        chat->last_tool_calls_json = strdup(chat->msgs[ri].tool_calls_json);
                        if (ri + 1 < chat->n_msgs && chat->msgs[ri + 1].tool_call_id)
                            chat->last_tool_call_id = strdup(chat->msgs[ri + 1].tool_call_id);
                        break;
                    }
                }

                /* Recalculate evict_start since removals shifted indices */
                evict_start = keep_head;

                /* Re-inject scratchpad (uses shared helper — BUG A FIX) */
                react_reinject_scratchpad(ctx, chat, evict_start);

                int insert_pos = evict_start + 1;  /* after scratchpad */

                if (breadcrumb.len > 0) {
                    char *bc_str = str_steal(&breadcrumb);
                    llm_chat_insert_typed(chat, insert_pos,
                        "user", bc_str, LLM_MSG_EVICTION_SUMMARY);
                    free(bc_str);
                    insert_pos++;
                } else {
                    str_free(&breadcrumb);
                }

                /* v4 unified memory: post-compaction hint */
                llm_chat_insert_typed(chat, insert_pos,
                    "user",
                    "[Context compacted. Use memory_recall to recover lost "
                    "context — it searches both stored knowledge and past "
                    "session history.]",
                    LLM_MSG_MEMORY_HINT);

                react_event_t ev = {0};
                ev.react_loop = ctx->tools->react_loop;
                ev.type = REACT_EVENT_WARNING;
                ev.step = step + 1;
                ev.message = "Context compacted (CWL+LCM) — recoverable refs indexed, non-recoverable summarized";
                react_emit(on_event, userdata, &ev);
            }
            free(evict_mark);
        } /* end n_evictable > 0 */
    }

post_eviction:
    /* L6 FIX: Post-eviction budget verification with RECOVERY.
     * D4 FIX: Proportional scratchpad stripping instead of all-or-nothing.
     * First try re-injecting a smaller scratchpad. Only strip entirely
     * if proportional reduction doesn't help. */
    if (did_evict) {
        total_chars = react_calc_total_chars(chat);
        usage_pct = (context_budget > 0)
            ? (int)(100L * total_chars / context_budget) : 0;
        if (usage_pct > eviction_pct) {
            /* D4 FIX: Try proportional reduction first — re-inject at half budget */
            nash_log("[eviction] post-eviction usage %d%% > trigger %d%% — "
                     "shrinking scratchpad", usage_pct, eviction_pct);
            llm_chat_remove_by_type(chat, LLM_MSG_SCRATCHPAD);
            /* Re-inject at half the normal budget */
            {
                long overshoot = total_chars - (context_budget * eviction_pct / 100);
                long sp_chars = actual_sp_size - overshoot;
                if (sp_chars > 512) {
                    char *small_sp = scratchpad_serialize_budget(
                        &ctx->tools->scratch, (size_t)sp_chars);
                    if (small_sp && small_sp[0]) {
                        size_t slen = strlen(small_sp);
                        char *sp_msg = malloc(slen + 32);
                        if (sp_msg) {
                            snprintf(sp_msg, slen + 32, "[SCRATCHPAD]\n%s", small_sp);
                            llm_chat_insert_typed(chat, keep_head,
                                "user", sp_msg, LLM_MSG_SCRATCHPAD);
                            free(sp_msg);
                        }
                    }
                    free(small_sp);
                }
            }
            total_chars = react_calc_total_chars(chat);
            usage_pct = (context_budget > 0)
                ? (int)(100L * total_chars / context_budget) : 0;
            if (usage_pct > eviction_pct) {
                nash_log("[eviction] still %d%% after scratchpad shrink — "
                         "stripping entirely", usage_pct);
                llm_chat_remove_by_type(chat, LLM_MSG_SCRATCHPAD);
                total_chars = react_calc_total_chars(chat);
                usage_pct = (context_budget > 0)
                    ? (int)(100L * total_chars / context_budget) : 0;
                if (usage_pct > eviction_pct) {
                    nash_log("[eviction] still %d%% — emergency eviction", usage_pct);
                    react_emergency_evict(chat, context_budget);
                }
            }
        }
    }

    /* Log compaction event to journal if anything changed */
    if (did_evict && ctx->tools->journal) {
        int after_msgs = chat->n_msgs;
        long after_chars = react_calc_total_chars(chat);
        int after_pct = (context_budget > 0)
            ? (int)(100L * after_chars / context_budget) : 0;

        cJSON *params = cJSON_CreateObject();
        cJSON_AddNumberToObject(params, "before_msgs", before_msgs);
        cJSON_AddNumberToObject(params, "after_msgs", after_msgs);
        cJSON_AddNumberToObject(params, "before_pct", before_pct);
        cJSON_AddNumberToObject(params, "after_pct", after_pct);
        journal_append(ctx->tools->journal, ctx->tools->react_loop,
                       step, "compaction", params, NULL, 0, 0, NULL, NULL);
        cJSON_Delete(params);
    }
    str_free(&bm25_buf);
}
