/* react_eviction.c — Mark-then-sweep context eviction (Proposals A–E).
 *
 * Replaces the old 3-pass progressive pipeline with a cleaner architecture:
 *   1. Mark:     Score all evictable messages once, mark lowest-scored for removal
 *   2. Sweep:    Remove marked messages in reverse order, build breadcrumbs
 *   3. Compress: BM25 compress surviving messages (largest first)
 *   4. Finalize: Re-inject scratchpad + breadcrumbs + hint, verify budget
 *
 * Key improvements:
 *   Proposal A: Unified finalize() replaces 4 separate re-injection paths
 *   Proposal B: Mark-then-sweep eliminates mid-iteration index corruption
 *   Proposal D: Per-phase target percentages (compress targets target_pct,
 *               only sweep accounts for re-injection cost)
 *   Proposal E: Partner index built once, eliminates O(n²) partner scanning
 *   FIX FLAW 1: No backward iteration with removal — mark phase is read-only
 *   FIX FLAW 2: No shrinking search range — partner map is pre-computed
 *   FIX FLAW 5: Separate breadcrumb index and summary budgets
 *   FIX FLAW 6: Pass 2 targets target_pct (not effective_target_pct)
 *   FIX FLAW 8: Fixed compress threshold instead of average-based
 */

#include "react_internal.h"
#include "compress.h"

/* ── Comparison functions for qsort ───────────────────── */

typedef struct { int idx; int len; } compress_cand_t;

static int cmp_compress_desc(const void *a, const void *b) {
    return ((const compress_cand_t *)b)->len - ((const compress_cand_t *)a)->len;
}

typedef struct { int idx; int score; int len; } evict_scored_t;  /* Review B3: len cached from scoring */

static int cmp_evict_score_asc(const void *a, const void *b) {
    return ((const evict_scored_t *)a)->score - ((const evict_scored_t *)b)->score;
}

/* ── Proposal E: Partner Index ────────────────────────── */

/* Build a partner map for all messages in [range_start, range_end).
 * Uses cached tool_call_id_outbound (Proposal C) for O(1) ID lookup.
 * partner[i] = absolute index of partner message, or -1 if none.
 * The map covers ALL messages (0..n_msgs-1) for uniform indexing. */
evict_partner_map_t evict_build_partner_map(const llm_chat_t *chat,
                                             int range_start, int range_end) {
    evict_partner_map_t map = {0};
    map.n_msgs = chat->n_msgs;
    map.partner = malloc((size_t)chat->n_msgs * sizeof(int));
    if (!map.partner) { map.n_msgs = 0; return map; }

    for (int i = 0; i < chat->n_msgs; i++)
        map.partner[i] = -1;

    /* D1 FIX: Delegate to react_find_tool_partner() to eliminate duplicated
     * ID extraction + JSON fallback + forward scanning logic (~30 lines).
     * Note: react_find_tool_partner() checks importance >= HIGH and returns -1
     * for protected partners, which is the correct behavior (BUG 2 fix). */
    for (int i = range_start; i < range_end && i < chat->n_msgs; i++) {
        if (!chat->msgs[i].tool_calls_json) continue;
        if (map.partner[i] >= 0) continue;  /* already matched */

        int pi = react_find_tool_partner(chat, i, range_start, range_end);
        if (pi >= 0) {
            map.partner[i] = pi;
            map.partner[pi] = i;
        }
    }
    return map;
}

void evict_free_partner_map(evict_partner_map_t *map) {
    free(map->partner);
    map->partner = NULL;
    map->n_msgs = 0;
}

/* ── D3 FIX: Shared Mark-Sweep Helper ─────────────────── */

/* Review C2: O(n) single-pass compaction replaces O(k×n) reverse-order removal.
 * Previously called llm_chat_remove_range() per marked message, each doing a
 * memmove of the remaining tail. Now compacts in-place in a single forward pass.
 * Shared between progressive eviction and emergency eviction. */
int evict_sweep_marked(llm_chat_t *chat, int evict_start,
                       const int *evict_mark, int n_evictable) {
    int removed = 0;
    int evict_end = evict_start + n_evictable;

    /* Free marked messages and update total_chars */
    for (int i = 0; i < n_evictable; i++) {
        if (evict_mark[i]) {
            int mi = evict_start + i;
            chat->total_chars -= (long)chat->msgs[mi].content_len;
            llm_msg_free_fields(&chat->msgs[mi]);
            removed++;
        }
    }

    if (removed > 0) {
        /* Single-pass compaction within evictable region */
        int dst = evict_start;
        for (int src = evict_start; src < evict_end; src++) {
            if (!evict_mark[src - evict_start]) {
                if (dst != src)
                    chat->msgs[dst] = chat->msgs[src];
                dst++;
            }
        }
        /* Move tail (messages after evict_end) into place */
        int tail = chat->n_msgs - evict_end;
        if (tail > 0)
            memmove(&chat->msgs[dst], &chat->msgs[evict_end],
                    (size_t)tail * sizeof(llm_msg_t));
        chat->n_msgs -= removed;

        react_recover_tool_threading(chat);
    }
    return removed;
}

/* ── Scratchpad Re-injection ──────────────────────────── */

/* Re-inject scratchpad at insert_pos in chat.
 * Returns the serialized scratchpad size in chars (0 if nothing injected).
 * Shared between progressive and emergency eviction. */
long react_reinject_scratchpad(react_ctx_t *ctx, llm_chat_t *chat,
                               int insert_pos) {
    long context_budget = react_context_budget(ctx);
    long current_chars = react_calc_total_chars(chat);
    size_t sp_max = react_scratchpad_budget(context_budget, current_chars,
                                             REACT_SP_MIN);

    char *fresh_sp = scratchpad_serialize_budget(&ctx->tools->scratch, sp_max);
    long injected_chars = react_inject_scratchpad_msg(chat, insert_pos, fresh_sp);
    free(fresh_sp);
    return injected_chars;
}

/* ── Proposal A: Unified Finalization ─────────────────── */

/* Review C1: Flattened strategy loop replaces 4-level nested if cascade.
 * Review A2: Skip compaction hint when no eviction occurred (breadcrumb_str==NULL).
 * Review A1: Final verification after every re-injection to catch overshoot.
 * breadcrumb_str is consumed (freed) by this function. */
void evict_finalize(react_ctx_t *ctx, llm_chat_t *chat,
                   int keep_head, int target_pct, long context_budget,
                   char *breadcrumb_str, int step,
                   react_event_fn on_event, void *userdata) {
    int pos = keep_head;
    int did_evict = (breadcrumb_str != NULL);

    /* 1. Re-inject scratchpad */
    if (react_reinject_scratchpad(ctx, chat, pos) > 0) pos++;

    /* 2. Inject breadcrumbs (if any) */
    if (breadcrumb_str) {
        llm_chat_insert_typed(chat, pos,
            "user", breadcrumb_str, LLM_MSG_EVICTION_SUMMARY);
        free(breadcrumb_str);
        pos++;
    }

    /* 3. Review A2: Only inject compaction hint when eviction actually occurred */
    if (did_evict) {
        llm_chat_insert_typed(chat, pos,
            "user",
            "[Context compacted. Use memory_recall to recover lost "
            "context — it searches both stored knowledge and past "
            "session history.]",
            LLM_MSG_MEMORY_HINT);
    }

    /* 4. Review C1: Flattened strategy loop for budget verification.
     * Strategies tried in order: shrink SP → strip SP → emergency evict.
     * Loop exits as soon as usage drops to or below target. */
    typedef enum { STRAT_SHRINK_SP, STRAT_STRIP_SP, STRAT_EMERGENCY, STRAT_DONE } budget_strategy_t;
    int usage_pct = react_usage_pct(react_calc_total_chars(chat), context_budget);

    for (budget_strategy_t strat = STRAT_SHRINK_SP;
         strat < STRAT_DONE && usage_pct > target_pct;
         strat++) {
        switch (strat) {
        case STRAT_SHRINK_SP: {
            nash_log("[eviction] post-finalize usage %d%% > target %d%% — "
                     "shrinking scratchpad", usage_pct, target_pct);
            llm_chat_remove_by_type(chat, LLM_MSG_SCRATCHPAD);
            long target_budget = context_budget * target_pct / 100;
            long sp_chars = target_budget - react_calc_total_chars(chat);
            if (sp_chars > REACT_SP_SHRINK_MIN) {
                char *small_sp = scratchpad_serialize_budget(
                    &ctx->tools->scratch, (size_t)sp_chars);
                int kh = react_compute_keep_head(chat);
                react_inject_scratchpad_msg(chat, kh, small_sp);
                free(small_sp);
            }
            break;
        }
        case STRAT_STRIP_SP:
            nash_log("[eviction] still %d%% after scratchpad shrink — "
                     "stripping entirely", usage_pct);
            llm_chat_remove_by_type(chat, LLM_MSG_SCRATCHPAD);
            break;

        case STRAT_EMERGENCY:
            nash_log("[eviction] still %d%% — emergency eviction", usage_pct);
            react_emergency_evict(chat, context_budget, target_pct);
            /* Review A1: Re-inject scratchpad only if room permits */
            if (react_usage_pct(react_calc_total_chars(chat), context_budget) < target_pct) {
                int kh = react_compute_keep_head(chat);
                react_reinject_scratchpad(ctx, chat, kh);
            }
            break;

        case STRAT_DONE:
            break;
        }
        /* Review A1: Re-check after every strategy (catches overshoot from re-injection) */
        usage_pct = react_usage_pct(react_calc_total_chars(chat), context_budget);
    }

    /* Emit event */
    if (on_event && did_evict) {
        react_event_t ev = {0};
        ev.react_loop = ctx->tools->react_loop;
        ev.type = REACT_EVENT_WARNING;
        ev.step = step + 1;
        ev.message = "Context compacted (CWL+LCM) — recoverable refs indexed, "
                     "non-recoverable summarized";
        react_emit(on_event, userdata, &ev);
    }
}

/* ── Mark Phase: Score all evictable messages ─────────── */

/* Compute eviction scores for messages in [evict_start, evict_end).
 * Lower score = evict first. Score formula:
 *   importance * IMP_WEIGHT - recoverability * REC_WEIGHT - size_bonus + pos_norm
 *
 * LOW-importance messages get the lowest scores (evicted first), replacing
 * the old Pass 1 which had index-corruption bugs (FLAW 1, FLAW 2).
 * Returns malloc'd array of scored entries, or NULL. Sets *n_scored. */
static evict_scored_t *evict_score_messages(const llm_chat_t *chat,
                                            int evict_start, int evict_end,
                                            int *n_scored) {
    int n = evict_end - evict_start;
    if (n <= 0) { *n_scored = 0; return NULL; }

    evict_scored_t *scored = malloc((size_t)n * sizeof(evict_scored_t));
    if (!scored) { *n_scored = 0; return NULL; }

    int n_actual = 0;
    for (int i = 0; i < n; i++) {
        int mi = evict_start + i;
        /* Skip HIGH/CRITICAL — never evicted */
        if (chat->msgs[mi].importance >= LLM_MSG_IMPORTANCE_HIGH) continue;

        int imp = (int)chat->msgs[mi].importance;
        int rec = (int)chat->msgs[mi].recoverability;
        int msg_len = (int)chat->msgs[mi].content_len;
        int pos_norm = (n > 1) ? (i * REACT_SCORE_POS_RANGE / (n - 1)) : 0;
        int size_bonus = (rec > 0 && msg_len > REACT_SCORE_SIZE_THRESH)
            ? (msg_len / REACT_SCORE_SIZE_DIV) * rec : 0;
        if (size_bonus > REACT_SCORE_SIZE_MAX) size_bonus = REACT_SCORE_SIZE_MAX;

        scored[n_actual].idx = i;  /* relative to evict_start */
        scored[n_actual].len = msg_len;  /* Review B3: cache for mark phase */
        /* Review A7: -rec term means recoverable content gets LOWER score
         * (evicted first), which is the desired behavior. */
        scored[n_actual].score = imp * REACT_SCORE_IMP_WEIGHT
                               - rec * REACT_SCORE_REC_WEIGHT
                               - size_bonus + pos_norm;
        n_actual++;
    }

    if (n_actual > 1)
        qsort(scored, (size_t)n_actual, sizeof(evict_scored_t), cmp_evict_score_asc);
    *n_scored = n_actual;
    return scored;
}

/* ── Compress Phase: BM25 compression ─────────────────── */

/* FIX FLAW 8: Uses REACT_COMPRESS_THRESH_FIXED instead of average-based
 * threshold. Previously small messages (300 chars) were compressed
 * unnecessarily with negligible savings. Now only messages > 800 chars
 * are candidates.
 *
 * Proposal D: Targets target_pct (not effective_target_pct) since compress
 * phase doesn't re-inject anything. Previously over-compressed content. */
static int evict_compress(llm_chat_t *chat, int keep_head, int keep_tail,
                          const char *bm25_query, int target_pct,
                          long context_budget) {
    int upper = chat->n_msgs - keep_tail;
    if (upper <= keep_head) return 0;

    /* FIX FLAW 8: Fixed threshold — don't depend on average message size */
    /* S1 FIX: REACT_COMPRESS_THRESH_FIXED (800) is the sole threshold */
    int compress_threshold = REACT_COMPRESS_THRESH_FIXED;

    /* Collect candidates */
    int n_candidates = 0, cand_cap = 0;
    compress_cand_t *candidates = NULL;

    for (int i = keep_head; i < upper; i++) {
        if (chat->msgs[i].importance <= LLM_MSG_IMPORTANCE_NORMAL &&
            chat->msgs[i].content &&
            (int)chat->msgs[i].content_len > compress_threshold) {
            if (n_candidates >= cand_cap) {
                cand_cap = cand_cap ? cand_cap * 2 : 16;
                compress_cand_t *tmp = realloc(candidates,
                    (size_t)cand_cap * sizeof(compress_cand_t));
                if (!tmp) break;
                candidates = tmp;
            }
            candidates[n_candidates].idx = i;
            candidates[n_candidates].len = (int)chat->msgs[i].content_len;
            n_candidates++;
        }
    }

    if (n_candidates > 1)
        qsort(candidates, (size_t)n_candidates, sizeof(compress_cand_t), cmp_compress_desc);

    int did_compress = 0;
    long total_chars = react_calc_total_chars(chat);

    for (int ci = 0; ci < n_candidates; ci++) {
        int i = candidates[ci].idx;
        int old_len = (int)chat->msgs[i].content_len;
        int scaled_units = old_len / 1000;
        if (scaled_units < REACT_COMPRESS_MIN_UNITS) scaled_units = REACT_COMPRESS_MIN_UNITS;
        int scaled_chars = old_len / 4;
        if (scaled_chars < REACT_COMPRESS_MIN_CHARS) scaled_chars = REACT_COMPRESS_MIN_CHARS;
        char *compressed = compress_to_relevant(
            chat->msgs[i].content, bm25_query, scaled_units, scaled_chars);
        if (compressed) {
            int new_len = (int)strlen(compressed);
            if (new_len < old_len) {
                llm_chat_replace_content(chat, i, compressed);
                total_chars = chat->total_chars;
                did_compress = 1;
            } else {
                free(compressed);
            }
            /* Proposal D: target_pct, not effective_target_pct */
            if (react_usage_pct(total_chars, context_budget) <= target_pct)
                break;
        }
    }
    free(candidates);
    return did_compress;
}

/* ── Sweep Phase: Remove marked messages + build breadcrumbs ── */

/* FIX FLAW 5: Build breadcrumbs with separate budgets for index and summary.
 * breadcrumb_index_cap limits the store-alias reference list.
 * breadcrumb_summary_cap limits the eviction summary text.
 * Returns a malloc'd breadcrumb string (caller frees), or NULL. */
static char *evict_build_breadcrumbs(react_ctx_t *ctx, const llm_chat_t *chat,
                                     int evict_start, int n_evictable,
                                     const int *evict_mark, int n_to_evict,
                                     long breadcrumb_index_cap,
                                     long breadcrumb_summary_cap) {
    str_t breadcrumb = str_new(512);
    str_t summary = str_new((size_t)breadcrumb_summary_cap);
    int max_per_msg = n_to_evict > 0
        ? (int)((size_t)breadcrumb_summary_cap / (unsigned)n_to_evict) : REACT_SUMMARY_PER_MSG_MIN;
    if (max_per_msg < REACT_SUMMARY_PER_MSG_MIN) max_per_msg = REACT_SUMMARY_PER_MSG_MIN;
    if (max_per_msg > REACT_SUMMARY_PER_MSG_MAX) max_per_msg = REACT_SUMMARY_PER_MSG_MAX;

    str_append_cstr(&breadcrumb, "[EVICTED CONTEXT — recoverable via file_read]\n");

    /* Dynamic seen_aliases array */
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
            /* FIX FLAW 5: Use separate index budget */
            if ((long)breadcrumb.len >= breadcrumb_index_cap) continue;

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

            char brief[REACT_BREADCRUMB_BRIEF_LEN + 1];
            int msg_clen = (int)chat->msgs[mi].content_len;
            int blen = msg_clen > REACT_BREADCRUMB_BRIEF_LEN
                ? REACT_BREADCRUMB_BRIEF_LEN : msg_clen;
            memcpy(brief, content, (size_t)blen);
            brief[blen] = '\0';
            for (int b = 0; brief[b]; b++)
                if (brief[b] == '\n' || brief[b] == '\r') brief[b] = ' ';
            str_appendf(&breadcrumb, "- %s: %s (%s, %d chars)\n",
                chat->msgs[mi].store_alias, brief, role, msg_clen);
            continue;
        }

        /* FIX FLAW 5: Use separate summary budget */
        int msg_clen = (int)chat->msgs[mi].content_len;
        if (strcmp(role, "tool") == 0 && msg_clen < REACT_SUMMARY_TOOL_MIN_LEN)
            continue;
        int clen = msg_clen > max_per_msg ? max_per_msg : msg_clen;
        str_appendf(&summary, "[%s]: ", role);
        str_append(&summary, content, (size_t)clen);
        if (msg_clen > max_per_msg)
            str_append_cstr(&summary, "...[truncated]");
        str_append_cstr(&summary, "\n");
        if ((long)summary.len >= breadcrumb_summary_cap) break;
    }

    if (summary.len > 0) {
        char *summ_str = str_steal(&summary);
        scratchpad_write(&ctx->tools->scratch, "evicted_context", summ_str, 2);
        scratchpad_save(&ctx->tools->scratch, ctx->tools->session_dir);
        free(summ_str);
    } else {
        str_free(&summary);
    }
    free(seen_aliases);

    if (breadcrumb.len > 0)
        return str_steal(&breadcrumb);
    str_free(&breadcrumb);
    return NULL;
}

/* ── Main Entry Point: Mark-then-Sweep ────────────────── */

/* Proposal B: Replace 3-pass + post-verify with mark-then-sweep.
 * All proposals (A–E) and bug fixes (FLAW 1–8) are implemented here.
 *
 * Architecture:
 *   1. Cleanup: Remove stale injected messages
 *   2. Partner map: Build once (Proposal E)
 *   3. Mark: Score all evictable messages, mark lowest for eviction
 *   4. Sweep: Remove marked messages in reverse order
 *   5. Compress: BM25 compress surviving messages (Proposal D: target_pct)
 *   6. Finalize: Re-inject scratchpad + breadcrumbs + hint (Proposal A)
 */
void react_maybe_evict(react_ctx_t *ctx, llm_chat_t *chat, int step,
                       const char *user_query,
                       react_event_fn on_event, void *userdata) {

    if (!ctx->flags.enable_compaction || ctx->provider->cfg.context_size <= 0)
        return;

    long context_budget = react_context_budget(ctx);
    int eviction_pct = ctx->tools->cfg ? ctx->tools->cfg->context_eviction_pct : 70;
    int hysteresis_gap = eviction_pct / REACT_HYSTERESIS_DIVISOR;
    if (hysteresis_gap < REACT_HYSTERESIS_MIN_GAP)
        hysteresis_gap = REACT_HYSTERESIS_MIN_GAP;
    int target_pct = eviction_pct - hysteresis_gap;

    long total_chars = react_calc_total_chars(chat);
    int usage_pct = react_usage_pct(total_chars, context_budget);

    int keep_head = react_compute_keep_head(chat);
    int keep_tail = react_compute_keep_tail(chat);

    if (usage_pct <= eviction_pct || chat->n_msgs <= keep_head + keep_tail + 1)
        return;

    /* Capture pre-compaction state for journal logging */
    int before_msgs = chat->n_msgs;
    int before_pct = usage_pct;

    /* ── Step 1: Cleanup stale injected messages ── */
    llm_chat_remove_by_type(chat, LLM_MSG_SCRATCHPAD);
    llm_chat_remove_by_type(chat, LLM_MSG_EVICTION_SUMMARY);
    llm_chat_remove_by_type(chat, LLM_MSG_MEMORY_HINT);

    /* Recompute after cleanup (indices shifted) */
    keep_head = react_compute_keep_head(chat);
    keep_tail = react_compute_keep_tail(chat);

    total_chars = react_calc_total_chars(chat);
    usage_pct = react_usage_pct(total_chars, context_budget);

    /* If cleanup alone brought us under target, finalize and return */
    if (usage_pct <= target_pct) {
        evict_finalize(ctx, chat, keep_head, target_pct, context_budget,
                      NULL, step, on_event, userdata);
        goto journal;
    }

    /* ── Step 2: Build partner index (Proposal E) ── */
    int evict_start = keep_head;
    int evict_end = chat->n_msgs - keep_tail;

    /* Adjust boundary to avoid splitting pairs at edges.
     * Review A3: Guard prevents excessive shrinking — stop if range
     * drops below 2 messages to avoid reducing n_evictable to 0. */
    while (evict_end > evict_start + 1) {
        if (evict_end < chat->n_msgs && chat->msgs[evict_end].tool_call_id) {
            evict_end--;
            continue;
        }
        if (evict_end - 1 >= evict_start &&
            chat->msgs[evict_end - 1].tool_calls_json) {
            evict_end--;
            continue;
        }
        break;
    }

    int n_evictable = evict_end - evict_start;
    if (n_evictable <= 0) {
        evict_finalize(ctx, chat, keep_head, target_pct, context_budget,
                      NULL, step, on_event, userdata);
        goto journal;
    }

    evict_partner_map_t pmap = evict_build_partner_map(chat, evict_start, evict_end);

    /* ── Step 3: Mark phase — score and select messages for eviction ── */
    int n_scored;
    evict_scored_t *scored = evict_score_messages(chat, evict_start, evict_end, &n_scored);

    int *evict_mark = calloc((size_t)n_evictable, sizeof(int));
    if (!evict_mark) {
        free(scored);
        evict_free_partner_map(&pmap);
        evict_finalize(ctx, chat, keep_head, target_pct, context_budget,
                      NULL, step, on_event, userdata);
        goto journal;
    }

    /* Compute head/tail/evictable chars in a single pass (using cached content_len) */
    long head_chars = 0, tail_chars = 0, evictable_chars = 0;
    for (int ki = 0; ki < chat->n_msgs; ki++) {
        long mc = (long)chat->msgs[ki].content_len;
        if (ki < evict_start)
            head_chars += mc;
        else if (ki >= evict_end)
            tail_chars += mc;
        else
            evictable_chars += mc;
    }

    /* Compaction floor — minimum evictable content to retain */
    long floor_chars = react_calc_floor_chars(chat, evict_start, context_budget,
                                              head_chars);

    /* Proposal D: Compute effective target for sweep phase only.
     * Compress phase targets target_pct (it doesn't re-inject).
     * Sweep phase targets effective_target_pct (accounts for re-injection). */
    long actual_sp_size = (long)scratchpad_total_size(&ctx->tools->scratch);
    long bc_index_cap = (long)(context_budget * REACT_BREADCRUMB_INDEX_PCT / 100);
    long bc_summary_cap = (long)(context_budget * REACT_BREADCRUMB_SUMMARY_PCT / 100);
    if (bc_index_cap < REACT_BREADCRUMB_CAP_MIN / 2)
        bc_index_cap = REACT_BREADCRUMB_CAP_MIN / 2;
    if (bc_summary_cap < REACT_BREADCRUMB_CAP_MIN / 2)
        bc_summary_cap = REACT_BREADCRUMB_CAP_MIN / 2;
    long reinject_est = actual_sp_size + bc_index_cap + bc_summary_cap + REACT_REINJECT_PAD;

    int effective_target_pct = target_pct;
    if (context_budget > 0) {
        int reinject_pct = (int)(100L * reinject_est / context_budget);
        effective_target_pct = target_pct - reinject_pct;
        if (effective_target_pct < REACT_EFF_TARGET_MIN_PCT)
            effective_target_pct = REACT_EFF_TARGET_MIN_PCT;
    }

    /* Mark messages for eviction (lowest-scored first) */
    long remaining_chars = tail_chars + evictable_chars;
    int n_to_evict = 0;

    if (scored) {
        for (int si = 0; si < n_scored; si++) {
            int ri = scored[si].idx;  /* relative to evict_start */
            int mi = evict_start + ri;
            long msg_chars = (long)chat->msgs[mi].content_len;

            if (evict_mark[ri]) continue;
            if (remaining_chars - msg_chars < floor_chars) continue;

            /* Check partner using pre-built map (Proposal E) */
            int partner_mi = (mi < pmap.n_msgs) ? pmap.partner[mi] : -1;
            int pair_ri = -1;
            long pair_chars = 0;

            if (partner_mi >= evict_start && partner_mi < evict_end) {
                pair_ri = partner_mi - evict_start;
                /* BUG 2 FIX: Skip HIGH/CRITICAL partners — never evict them */
                if (chat->msgs[partner_mi].importance >= LLM_MSG_IMPORTANCE_HIGH) {
                    pair_ri = -1;  /* partner is protected */
                } else if (!evict_mark[pair_ri]) {
                    pair_chars = (long)chat->msgs[partner_mi].content_len;
                } else {
                    pair_ri = -1;  /* partner already marked */
                }
            }

            /* Check floor for total pair cost */
            if (remaining_chars - msg_chars - pair_chars < floor_chars) continue;

            evict_mark[ri] = 1;
            remaining_chars -= msg_chars;
            n_to_evict++;

            if (pair_ri >= 0 && !evict_mark[pair_ri]) {
                evict_mark[pair_ri] = 1;
                remaining_chars -= pair_chars;
                n_to_evict++;
            }

            /* Proposal D: Use effective_target_pct for sweep phase */
            if (react_usage_pct(head_chars + remaining_chars, context_budget)
                <= effective_target_pct)
                break;
        }
    }
    free(scored);

    /* ── Step 4: Sweep phase — remove marked messages + build breadcrumbs ── */
    char *bc_str = NULL;
    if (n_to_evict > 0) {
        /* Build breadcrumbs before removing messages */
        bc_str = evict_build_breadcrumbs(ctx, chat, evict_start, n_evictable,
                                          evict_mark, n_to_evict,
                                          bc_index_cap, bc_summary_cap);

        /* D3 FIX: Use shared sweep helper */
        evict_sweep_marked(chat, evict_start, evict_mark, n_evictable);
    }

    free(evict_mark);
    evict_free_partner_map(&pmap);

    /* ── Step 5: Compress phase — BM25 compress surviving messages ── */
    /* Flaw 1 FIX: Compress runs AFTER sweep so it only processes messages
     * that survived eviction. Previously compress ran before sweep, wasting
     * CPU on marked messages and causing incorrect budget tracking. */
    /* Proposal D: Compress targets target_pct (not effective_target_pct) */
    keep_head = react_compute_keep_head(chat);
    keep_tail = react_compute_keep_tail(chat);
    total_chars = react_calc_total_chars(chat);
    usage_pct = react_usage_pct(total_chars, context_budget);
    char *bm25_query = react_build_bm25_query(chat, user_query, &ctx->tools->scratch);
    if (usage_pct > target_pct) {
        evict_compress(chat, keep_head, keep_tail, bm25_query,
                       target_pct, context_budget);
    }
    free(bm25_query);

    /* ── Step 6: Finalize (Proposal A) ── */
    keep_head = react_compute_keep_head(chat);
    evict_finalize(ctx, chat, keep_head, target_pct, context_budget,
                  bc_str, step, on_event, userdata);

journal:
    /* Log compaction event to journal if anything changed */
    if (ctx->tools->journal) {
        long after_chars = react_calc_total_chars(chat);
        int after_pct = react_usage_pct(after_chars, context_budget);
        if (after_pct != before_pct || chat->n_msgs != before_msgs) {
            cJSON *params = cJSON_CreateObject();
            cJSON_AddNumberToObject(params, "before_msgs", before_msgs);
            cJSON_AddNumberToObject(params, "after_msgs", chat->n_msgs);
            cJSON_AddNumberToObject(params, "before_pct", before_pct);
            cJSON_AddNumberToObject(params, "after_pct", after_pct);
            journal_append(ctx->tools->journal, ctx->tools->react_loop,
                           step, "compaction", params, NULL, 0, 0, NULL, NULL);
            cJSON_Delete(params);
        }
    }
}
