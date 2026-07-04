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
#include "embedding.h"
#include "repomap.h"

/* ── Algorithm-internal constants (stable, not policy-configurable) ── */
/* Score formula coefficients now in react_internal.h (shared with emergency scorer). */
/* Breadcrumb brief preview truncation (chars — fallback when no structural symbols). */
#define REACT_BREADCRUMB_BRIEF_LEN  80
/* Structural breadcrumb capacity (chars — for repomap symbol extraction). */
#define REACT_BREADCRUMB_STRUCT_LEN 200
/* Minimum tool content length to include in eviction summary. */
#define REACT_SUMMARY_TOOL_MIN_LEN  50
/* Minimum per-component breadcrumb capacity (chars). */
#define REACT_BREADCRUMB_SUBCAP_MIN 512
/* D3 FIX: Padding added to re-injection estimate (chars).
 * Accounts for: eviction-triggered memory re-retrieval (2 × 2048 = 4096),
 * EVICT_COMPACT_HINT (~130 chars), breadcrumb header (~48 chars),
 * REACT_SP_PREFIX_LEN (13 chars), and miscellaneous overhead.
 * Previously 200 — systematically underestimated by ~4KB, causing
 * progressive eviction to fail and fall back to emergency eviction. */
#define REACT_REINJECT_PAD          4500
/* Minimum effective target percentage (prevents target going to 0). */
#define REACT_EFF_TARGET_MIN_PCT    10
/* FIX #10: Maximum store-alias dedup entries in breadcrumb builder.
 * L6 FIX: Increased from 64 to 256 — at 64, long sessions would overflow
 * the dedup array, causing duplicate store references in breadcrumbs. */
#define REACT_BREADCRUMB_MAX_ALIASES 256
/* NOTE: The following constants moved to eviction_policy_t (react_internal.h):
 * REACT_SP_SHRINK_MIN        → pol.sp_shrink_min
 * REACT_BREADCRUMB_INDEX_PCT → pol.bc_index_pct
 * REACT_BREADCRUMB_SUMMARY_PCT → pol.bc_summary_pct
 * REACT_SUMMARY_PER_MSG_MIN  → pol.summary_per_msg_min
 * REACT_SUMMARY_PER_MSG_MAX  → pol.summary_per_msg_max
 * REACT_COMPRESS_THRESH_FIXED→ pol.compress_min_len
 * REACT_COMPRESS_MIN_UNITS   → pol.compress_min_units
 * REACT_COMPRESS_MIN_CHARS   → pol.compress_min_chars
 */

/* ── Comparison functions for qsort ───────────────────── */

/* DEDUP3: Safe three-way comparison macro — avoids overflow from subtraction. */
#define SAFE_CMP(a, b) (((a) > (b)) - ((a) < (b)))

typedef struct { int idx; int len; } compress_cand_t;

/* Sort by length descending (largest first for compress). */
static int cmp_compress_desc(const void *a, const void *b) {
    int la = ((const compress_cand_t *)a)->len;
    int lb = ((const compress_cand_t *)b)->len;
    return SAFE_CMP(lb, la);
}

/* Review B4: Generic candidate type used by evict_mark_candidates(). */
typedef struct { int idx; int score; long chars; } evict_candidate_t;

/* Sort by score ascending (lowest score = evicted first). */
static int cmp_candidate_score_asc(const void *a, const void *b) {
    int sa = ((const evict_candidate_t *)a)->score;
    int sb = ((const evict_candidate_t *)b)->score;
    return SAFE_CMP(sa, sb);
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
    eviction_policy_t pol = react_eviction_policy(ctx->tools->cfg);
    long context_budget = react_context_budget(ctx);
    long current_chars = react_calc_total_chars(chat);
    size_t sp_max = react_scratchpad_budget_pol(context_budget, current_chars,
                                                (size_t)pol.sp_min_chars, &pol);

    char *fresh_sp = scratchpad_serialize_budget(&ctx->tools->scratch, sp_max);
    long injected_chars = react_inject_scratchpad_msg(chat, insert_pos, fresh_sp);
    free(fresh_sp);
    return injected_chars;
}

/* ── Shared Emergency Breadcrumb Injection ────────────── */

/* Inject breadcrumb summary + MEMORY_HINT + scratchpad after emergency eviction.
 * Shared between evict_finalize strategy-2 and react_emergency_evict_and_reinject.
 * BUG #4 FIX: scratchpad re-injection is budget-guarded (only if room permits),
 * fixing inconsistency where react_emergency_evict_and_reinject always re-injected. */
void react_inject_emergency_breadcrumbs(react_ctx_t *ctx, llm_chat_t *chat,
                                         int n_evicted, long context_budget,
                                         int target_pct, int skip_sp) {
    int kh = react_compute_keep_head(chat);

    /* DESIGN1 FIX: Compute SP budget BEFORE injecting breadcrumb + hint.
     * Previously the usage check happened after injection, so the
     * breadcrumb + hint chars inflated the usage %, potentially pushing
     * it above target_pct and preventing SP injection. This created a
     * self-defeating cycle where evict_finalize strategy-2 would fire. */
    long pre_inject_chars = react_calc_total_chars(chat);
    int inject_overhead = 0;
    if (n_evicted > 0)
        inject_overhead = 128 + (int)(sizeof(EVICT_COMPACT_HINT) - 1);
    long chars_after_inject = pre_inject_chars + inject_overhead;
    int can_inject_sp = (context_budget > 0)
        ? (int)(100L * chars_after_inject / context_budget) < target_pct
        : 1;

    if (n_evicted > 0) {
        char emsg[128];
        snprintf(emsg, sizeof(emsg),
                 "[%d messages emergency-evicted to free context]", n_evicted);
        llm_chat_insert_typed(chat, kh, "user", emsg,
                              LLM_MSG_EVICTION_SUMMARY);
        llm_chat_insert_typed(chat, kh + 1,
            "user", EVICT_COMPACT_HINT, LLM_MSG_MEMORY_HINT);
    }
    /* Re-inject scratchpad only if room permits (based on pre-injection budget).
     * BUG 1+2 FIX: skip_sp suppresses re-injection when Strategy 1 already
     * stripped the scratchpad — re-injecting would defeat the strip. */
    if (can_inject_sp && !skip_sp) {
        react_reinject_scratchpad(ctx, chat, kh);
    }
}

/* ── Proposal A: Unified Finalization ─────────────────── */

/* Review C1: Flattened strategy loop replaces 4-level nested if cascade.
 * Review A2: Skip compaction hint when no eviction occurred (breadcrumb_str==NULL).
 * Review A1: Final verification after every re-injection to catch overshoot.
 * breadcrumb_str is consumed (freed) by this function.
 * BUG2+3 FIX: Returns the number of messages emergency-evicted by Strategy 2
 * (0 if Strategy 2 didn't fire). Callers use this for journal + event emission. */
int evict_finalize(react_ctx_t *ctx, llm_chat_t *chat,
                   int keep_head, int target_pct, long context_budget,
                   char *breadcrumb_str, int n_evicted, int step,
                   react_event_fn on_event, void *userdata) {
    eviction_policy_t pol = react_eviction_policy(ctx->tools->cfg);
    int pos = keep_head;
    /* FIX #1: Use explicit n_evicted count instead of inferring from
     * breadcrumb_str != NULL. breadcrumb_str can be NULL even when eviction
     * happened (if all evicted messages were system-role or empty-content),
     * causing silent eviction with no MEMORY_HINT or event emitted. */
    int did_evict = (n_evicted > 0);

    /* D5/S2 FIX: Pre-compute available scratchpad budget BEFORE injection.
     * Previously step 1 injected at full budget, then strategy 1 removed and
     * re-injected at a smaller size — wasting an insert + remove cycle.
     * Now we compute the right size on the first attempt. */
    long current_chars = react_calc_total_chars(chat);
    long target_budget_chars = (context_budget > 0)
        ? context_budget * target_pct / 100 : 0;
    long other_inject_chars = 0;
    if (breadcrumb_str)
        other_inject_chars += (long)strlen(breadcrumb_str);
    if (did_evict)
        other_inject_chars += (long)(sizeof(EVICT_COMPACT_HINT) - 1);

    /* 1. Re-inject scratchpad at right-sized budget */
    if (target_budget_chars > 0) {
        long available_for_sp = target_budget_chars - current_chars - other_inject_chars;
        if (available_for_sp > pol.sp_shrink_min) {
            /* Clamp to normal scratchpad budget if room allows */
            size_t normal_budget = react_scratchpad_budget_pol(
                context_budget, current_chars, (size_t)pol.sp_min_chars, &pol);
            size_t sp_budget = (size_t)available_for_sp < normal_budget
                ? (size_t)available_for_sp : normal_budget;
            char *sp = scratchpad_serialize_budget(&ctx->tools->scratch, sp_budget);
            if (react_inject_scratchpad_msg(chat, pos, sp) > 0) pos++;
            free(sp);
        }
    } else {
        /* No budget info — inject at default budget */
        if (react_reinject_scratchpad(ctx, chat, pos) > 0) pos++;
    }

    /* 2. Inject breadcrumbs (if any) */
    if (breadcrumb_str) {
        /* ── Change 2: Eviction-triggered re-retrieval ──────────────────
         * arXiv 2605.30621: retrieval at init uses the initial query, but
         * the agent's needs evolve. The eviction summary describes exactly
         * what knowledge was just lost — it's the optimal re-retrieval query.
         * Query memory with the breadcrumb text BEFORE freeing it. */
        if (ctx->flags.inject_memory && (ctx->tools->memory || ctx->tools->ws)
            && strlen(breadcrumb_str) > 100) {
            int ev_candidates = ctx->tools->cfg
                ? ctx->tools->cfg->eviction_recall_candidates : 3;
            double ev_min_rel = ctx->tools->cfg
                ? ctx->tools->cfg->eviction_recall_min_relevance : 0.30;

            /* Truncate breadcrumb for recall query (max 400 chars) */
            char ev_query[512];
            snprintf(ev_query, sizeof(ev_query), "%.*s",
                     (int)utf8_clamp(breadcrumb_str, 400), breadcrumb_str);

            memory_results_t ev_mem = ctx->tools->ws
                ? workspace_recall(ctx->tools->ws, ev_query, ev_candidates)
                : memory_query(ctx->tools->memory, ev_query, ev_candidates);

            int ev_injected = 0;
            for (int j = 0; j < ev_mem.count && ev_injected < 2; j++) {
                /* Dedup against recalled_keys */
                int dup = 0;
                for (int k = 0; k < ctx->tools->n_recalled_keys; k++) {
                    if (strcmp(ctx->tools->recalled_keys[k],
                               ev_mem.entries[j].key) == 0) { dup = 1; break; }
                }
                if (!dup && ev_mem.entries[j].relevance > ev_min_rel) {
                    /* Review Issue #5 FIX: Use str_t to avoid silent
                     * truncation of large memory values at 2048 chars. */
                    str_t hint = str_new(256);
                    str_appendf(&hint,
                        "[MEMORY RECOVERY — post-eviction]\n"
                        "--- %s ---\n%s",
                        ev_mem.entries[j].key, ev_mem.entries[j].value);
                    llm_chat_insert_typed(chat, pos, "user",
                        str_cstr(&hint), LLM_MSG_MEMORY_HINT);
                    str_free(&hint);
                    tool_track_recalled_key(ctx->tools, ev_mem.entries[j].key);
                    pos++;
                    ev_injected++;
                }
            }
            memory_results_free(&ev_mem);
        }

        llm_chat_insert_typed(chat, pos,
            "user", breadcrumb_str, LLM_MSG_EVICTION_SUMMARY);
        free(breadcrumb_str);
        pos++;
    }

    /* 3. Review A2: Only inject compaction hint when eviction actually occurred */
    if (did_evict) {
        llm_chat_insert_typed(chat, pos,
            "user", EVICT_COMPACT_HINT, LLM_MSG_MEMORY_HINT);
    }

    /* C1 FIX: Flat if-chain replaces over-engineered strategy enum + loop.
     * D5/S2: Scratchpad was already injected at right-sized budget above.
     * Strategies: strip SP entirely → emergency evict.
     * Each step re-checks usage and exits as soon as budget is met. */
    /* FIX #12: Use react_chat_usage_pct convenience helper */
    int usage_pct = react_chat_usage_pct(chat, context_budget);

    /* Strategy 1: Strip scratchpad entirely
     * FIX #14: Preserve the "evicted_context" scratchpad section across
     * the strip. evict_build_breadcrumbs writes non-recoverable eviction
     * summaries there (priority 2), and losing them means the LLM can
     * never recall what was evicted. Save before strip, restore after. */
    if (usage_pct > target_pct) {
        nash_log("[eviction] post-finalize usage %d%% > target %d%% — "
                 "stripping scratchpad", usage_pct, target_pct);
        /* Save evicted_context before strip */
        char *saved_evicted = NULL;
        int ec_idx = scratchpad_find(&ctx->tools->scratch, "evicted_context");
        if (ec_idx >= 0 && ctx->tools->scratch.sections[ec_idx].content)
            saved_evicted = strdup(ctx->tools->scratch.sections[ec_idx].content);
        llm_chat_remove_by_type(chat, LLM_MSG_SCRATCHPAD);
        /* Restore evicted_context if it existed */
        if (saved_evicted) {
            scratchpad_write(&ctx->tools->scratch, "evicted_context", saved_evicted, 2);
            free(saved_evicted);
        }
        usage_pct = react_chat_usage_pct(chat, context_budget);
    }

    /* Strategy 2: Emergency eviction
     * FIX #6: This indicates the progressive eviction's effective_target_pct
     * underestimated the re-injection overhead. Log a warning. */
    int n_emergency = 0;
    if (usage_pct > target_pct) {
        nash_log("[eviction] WARNING: post-finalize still %d%% > target %d%% — "
                 "progressive eviction underestimated re-injection cost, "
                 "falling back to emergency eviction", usage_pct, target_pct);
        /* BUG1 FIX: Remove EVICTION_SUMMARY (step-2 breadcrumbs) AND
         * MEMORY_HINT (step-3 hint) before emergency eviction. Previously
         * only MEMORY_HINT was removed, leaving the progressive breadcrumb
         * coexisting with the emergency breadcrumb — two EVICTION_SUMMARYs. */
        llm_chat_remove_by_type(chat, LLM_MSG_EVICTION_SUMMARY);
        llm_chat_remove_by_type(chat, LLM_MSG_MEMORY_HINT);
        /* BUG 1+2 FIX: Call react_emergency_evict + inject_breadcrumbs directly
         * with skip_sp=1 to prevent re-injecting the scratchpad that Strategy 1
         * just stripped. Using react_emergency_evict_and_reinject would re-inject
         * SP (skip_sp=0), defeating Strategy 1's strip. */
        n_emergency = react_emergency_evict(chat, context_budget, target_pct,
                                            ctx->tools->cfg);
        react_inject_emergency_breadcrumbs(ctx, chat, n_emergency,
                                           context_budget, target_pct,
                                           /*skip_sp=*/1);
        /* FIX #15: When emergency eviction found nothing (e.g. floor too
         * restrictive, too few evictable messages, or all HIGH importance),
         * retry with a more aggressive target (target_pct - 10, min 50%)
         * which lowers the floor proportionally. This is a last resort
         * before the outer HTTP-400 retry handler takes over. */
        if (n_emergency == 0 &&
            react_chat_usage_pct(chat, context_budget) > target_pct) {
            int aggressive_pct = target_pct - 10;
            if (aggressive_pct < 50) aggressive_pct = 50;
            nash_log("[eviction] WARNING: emergency eviction found nothing "
                     "at target %d%% — retrying with aggressive target %d%%",
                     target_pct, aggressive_pct);
            n_emergency = react_emergency_evict(chat, context_budget,
                                                aggressive_pct, ctx->tools->cfg);
            if (n_emergency > 0) {
                react_inject_emergency_breadcrumbs(ctx, chat, n_emergency,
                                                   context_budget, aggressive_pct,
                                                   /*skip_sp=*/1);
            } else {
                nash_log("[eviction] WARNING: aggressive emergency eviction "
                         "also found nothing, context remains at %d%% > target %d%%",
                         react_chat_usage_pct(chat, context_budget), target_pct);
            }
        }
        usage_pct = react_chat_usage_pct(chat, context_budget);
    }

    /* BUG3 FIX: Emit event when EITHER progressive or emergency eviction
     * occurred. Previously only checked did_evict (progressive count),
     * so emergency-only eviction produced no UI notification. */
    if (on_event && (did_evict || n_emergency > 0)) {
        react_event_t ev = {0};
        ev.react_loop = ctx->tools->react_loop;
        ev.type = REACT_EVENT_WARNING;
        ev.step = step + 1;
        ev.message = "Context compacted (CWL+LCM) — recoverable refs indexed, "
                     "non-recoverable summarized";
        react_emit(on_event, userdata, &ev);
    }
    return n_emergency;
}

/* ── Progressive Scoring Callback ─────────────────────── */

/* Score formula for progressive (non-emergency) eviction.
 *   importance * IMP_WEIGHT - recoverability * REC_WEIGHT - size_bonus + pos_norm
 * Review A7: -rec term means recoverable content gets LOWER score
 * (evicted first), which is the desired behavior.
 * FIX #2: userdata = evict_partner_map_t*. size_bonus now includes partner
 * message cost, so a small tool_call message whose partner is a 10KB
 * tool_result will score lower (more likely to be evicted), which is correct
 * since recoverable content with large payloads should be freed first. */
int evict_score_progressive(const llm_chat_t *chat, int mi, int ri,
                            int n_evictable, void *userdata) {
    int imp = (int)chat->msgs[mi].importance;
    int rec = (int)chat->msgs[mi].recoverability;
    int msg_len = (int)chat->msgs[mi].content_len;
    /* FIX #2+#4: Include partner size only for the primary (tool_call) message.
     * Previously both partners included each other's size, double-counting
     * the pair cost and making the partner eviction logic redundant. */
    const evict_partner_map_t *pmap = (const evict_partner_map_t *)userdata;
    if (pmap && mi < pmap->n_msgs && pmap->partner[mi] >= 0
        && chat->msgs[mi].tool_calls_json) {
        int pi = pmap->partner[mi];
        msg_len += (int)chat->msgs[pi].content_len;
    }
    int pos_norm = (n_evictable > 1)
        ? (ri * REACT_SCORE_POS_RANGE / (n_evictable - 1)) : 0;
    /* FLAW 4 FIX: Apply size bonus for non-recoverable content too (at half
     * rate). Previously a 50KB rec=0 message scored the same as a 200-char one,
     * causing bloated non-recoverable LOW-importance messages to survive while
     * smaller important content was evicted. */
    int size_bonus = 0;
    if (msg_len > REACT_SCORE_SIZE_THRESH) {
        if (rec > 0)
            size_bonus = (msg_len / REACT_SCORE_SIZE_DIV) * rec;
        else
            size_bonus = msg_len / (REACT_SCORE_SIZE_DIV * 2);
    }
    if (size_bonus > REACT_SCORE_SIZE_MAX) size_bonus = REACT_SCORE_SIZE_MAX;
    return imp * REACT_SCORE_IMP_WEIGHT
         - rec * REACT_SCORE_REC_WEIGHT
         - size_bonus + pos_norm;
}

/* ── Semantic-Aware Progressive Scoring ──────────────── */

/* Adds a semantic relevance bonus to the base progressive score.
 * The bonus (0..REACT_SCORE_SEMANTIC_WEIGHT) is derived from pre-computed
 * cosine similarities between each message and the current task+reasoning.
 * A LOW-importance message highly relevant to the current task (sim ~1.0)
 * gets +40, potentially surviving over irrelevant NORMAL messages.
 * The bonus can never override HIGH importance (those are skipped entirely
 * by evict_mark_candidates before scoring).
 *
 * userdata = evict_score_ctx_t* (partner map + pre-computed similarities).
 * When similarities is NULL, falls back to base formula (zero overhead). */
int evict_score_progressive_semantic(const llm_chat_t *chat, int mi, int ri,
                                     int n_evictable, void *userdata) {
    const evict_score_ctx_t *sctx = (const evict_score_ctx_t *)userdata;

    /* Compute base score (same formula as evict_score_progressive) */
    int imp = (int)chat->msgs[mi].importance;
    int rec = (int)chat->msgs[mi].recoverability;
    int msg_len = (int)chat->msgs[mi].content_len;

    /* Include partner size for tool_call messages */
    const evict_partner_map_t *pmap = sctx ? sctx->pmap : NULL;
    if (pmap && mi < pmap->n_msgs && pmap->partner[mi] >= 0
        && chat->msgs[mi].tool_calls_json) {
        int pi = pmap->partner[mi];
        msg_len += (int)chat->msgs[pi].content_len;
    }

    int pos_norm = (n_evictable > 1)
        ? (ri * REACT_SCORE_POS_RANGE / (n_evictable - 1)) : 0;

    int size_bonus = 0;
    if (msg_len > REACT_SCORE_SIZE_THRESH) {
        if (rec > 0)
            size_bonus = (msg_len / REACT_SCORE_SIZE_DIV) * rec;
        else
            size_bonus = msg_len / (REACT_SCORE_SIZE_DIV * 2);
    }
    if (size_bonus > REACT_SCORE_SIZE_MAX) size_bonus = REACT_SCORE_SIZE_MAX;

    int base_score = imp * REACT_SCORE_IMP_WEIGHT
                   - rec * REACT_SCORE_REC_WEIGHT
                   - size_bonus + pos_norm;

    /* Semantic relevance bonus from pre-computed similarities */
    int semantic_bonus = 0;
    if (sctx && sctx->similarities && mi < sctx->n_msgs) {
        float sim = sctx->similarities[mi];
        if (sim > 0.0f) {
            semantic_bonus = (int)(sim * (float)REACT_SCORE_SEMANTIC_WEIGHT);
            if (semantic_bonus > REACT_SCORE_SEMANTIC_WEIGHT)
                semantic_bonus = REACT_SCORE_SEMANTIC_WEIGHT;
        }
    }

    return base_score + semantic_bonus;
}

/* ── Review B4: Generic Mark-Candidates ───────────────── */

/* Score, sort, and mark evictable messages using a caller-supplied scoring
 * function. Shared between progressive and emergency eviction.
 * See react_internal.h for full parameter documentation.
 * Returns the number of messages marked for eviction. */
int evict_mark_candidates(const llm_chat_t *chat,
                          int evict_start, int evict_end,
                          const evict_partner_map_t *pmap,
                          long floor_chars,
                          long remaining_nonhead,
                          long tail_chars,
                          long target_remaining,
                          evict_score_fn score_fn, void *score_ud,
                          int *evict_mark) {
    int n_evictable = evict_end - evict_start;
    if (n_evictable <= 0) return 0;

    /* Build scored candidate array (skip HIGH/CRITICAL) */
    evict_candidate_t *cands = malloc((size_t)n_evictable * sizeof(evict_candidate_t));
    if (!cands) return 0;

    int n_cands = 0;
    for (int i = 0; i < n_evictable; i++) {
        int mi = evict_start + i;
        if (chat->msgs[mi].importance >= LLM_MSG_IMPORTANCE_HIGH) continue;
        cands[n_cands].idx   = i;  /* relative to evict_start */
        cands[n_cands].score = score_fn(chat, mi, i, n_evictable, score_ud);
        cands[n_cands].chars = (long)chat->msgs[mi].content_len;
        n_cands++;
    }

    if (n_cands > 1)
        qsort(cands, (size_t)n_cands, sizeof(evict_candidate_t),
              cmp_candidate_score_asc);

    /* Mark candidates, respecting floor + partner pairing + target.
     * Floor check subtracts tail_chars because `remaining` includes protected
     * tail content that can never be evicted. Without this, the floor is
     * defeated when tail_chars >= floor_chars, allowing all evictable content
     * to be removed. */
    int n_marked = 0;
    long remaining = remaining_nonhead;

    for (int ci = 0; ci < n_cands; ci++) {
        int ri = cands[ci].idx;
        if (evict_mark[ri]) continue;

        long msg_chars = cands[ci].chars;
        if (remaining - tail_chars - msg_chars < floor_chars) continue;

        /* Partner lookup via pre-built map (Proposal E) */
        int mi = evict_start + ri;
        int partner_mi = (pmap && mi < pmap->n_msgs) ? pmap->partner[mi] : -1;
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

        /* FLAW 5 FIX + L2 FIX: When pair violates floor, decide based on
         * message type. Never orphan a tool_result (confuses LLM — answer
         * without question). A tool_call without its result is tolerable
         * (LLM sees "I asked but didn't get an answer"). */
        int evict_partner = 0;
        if (pair_ri >= 0 && !evict_mark[pair_ri]) {
            if (remaining - tail_chars - msg_chars - pair_chars >= floor_chars) {
                evict_partner = 1;
            } else if (chat->msgs[mi].tool_call_id) {
                /* L2 FIX: This is a tool_result — skip it entirely rather
                 * than orphaning it (a result without its question is worse
                 * than a question without its answer). */
                continue;
            }
            /* else: tool_call message — evict alone, partner (result) stays */
        }

        evict_mark[ri] = 1;
        remaining -= msg_chars;
        n_marked++;

        if (evict_partner) {
            evict_mark[pair_ri] = 1;
            remaining -= pair_chars;
            n_marked++;
        }

        /* Stop when we've reached the target */
        if (remaining <= target_remaining) break;
    }
    free(cands);
    return n_marked;
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
                          long context_budget,
                          const eviction_policy_t *pol) {
    int upper = chat->n_msgs - keep_tail;
    if (upper <= keep_head) return 0;

    /* Compress threshold from policy (default 800) */
    int compress_threshold = pol->compress_min_len;

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
        if (scaled_units < pol->compress_min_units) scaled_units = pol->compress_min_units;
        int scaled_chars = old_len / 4;
        if (scaled_chars < pol->compress_min_chars) scaled_chars = pol->compress_min_chars;
        char *compressed = compress_to_relevant(
            chat->msgs[i].content, bm25_query, scaled_units, scaled_chars);
        if (compressed) {
            int new_len = (int)strlen(compressed);
            /* FLAW 7 FIX: Guard against compress returning empty string,
             * which would effectively delete the message content.
             * FIX #13: Require at least 10% reduction — trivial compression
             * (e.g. 1 char shorter) wastes the original content's coherence
             * without meaningful space savings. */
            if (new_len > 0 && new_len <= old_len * 9 / 10) {
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
 * Returns a malloc'd breadcrumb string (caller frees), or NULL.
 * FIX #7: SIDE EFFECT — also writes an "evicted_context" section to the
 * scratchpad with summary text for non-alias evicted messages. */
static char *evict_build_breadcrumbs(react_ctx_t *ctx, const llm_chat_t *chat,
                                     int evict_start, int n_evictable,
                                     const int *evict_mark, int n_to_evict,
                                     long breadcrumb_index_cap,
                                     long breadcrumb_summary_cap) {
    eviction_policy_t pol = react_eviction_policy(ctx->tools->cfg);
    str_t breadcrumb = str_new(512);
    str_t summary = str_new((size_t)breadcrumb_summary_cap);
    int max_per_msg = n_to_evict > 0
        ? (int)(breadcrumb_summary_cap / n_to_evict) : pol.summary_per_msg_min;
    if (max_per_msg < pol.summary_per_msg_min) max_per_msg = pol.summary_per_msg_min;
    if (max_per_msg > pol.summary_per_msg_max) max_per_msg = pol.summary_per_msg_max;

    str_append_cstr(&breadcrumb, "[EVICTED CONTEXT — recoverable via file_read]\n");
    size_t header_len = breadcrumb.len;  /* BUG 1 FIX: track header-only length */

    /* FIX #14: Simplified alias dedup — linear scan replaces CRC32 hash table.
     * With n_to_evict typically 5-20, O(n²) is negligible (~200 comparisons max)
     * and saves ~30 lines of hash table code + the CRC32 dependency. */
    const char *seen_aliases[REACT_BREADCRUMB_MAX_ALIASES];
    int n_seen = 0;

    for (int ei = 0; ei < n_evictable; ei++) {
        if (!evict_mark[ei]) continue;
        int mi = evict_start + ei;
        const char *content = chat->msgs[mi].content;
        const char *role = chat->msgs[mi].role;
        if (!content || !content[0] || !role) continue;
        if (strcmp(role, "system") == 0) continue;

        if (chat->msgs[mi].store_alias) {
            /* FIX FLAW 5: Use separate index budget.
             * BUG5 FIX: Subtract header_len so the cap applies to index
             * content only, not the fixed header prefix. */
            if ((long)(breadcrumb.len - header_len) >= breadcrumb_index_cap) continue;

            /* FIX #14: Simple linear dedup */
            const char *alias = chat->msgs[mi].store_alias;
            int dup = 0;
            for (int s = 0; s < n_seen; s++) {
                if (strcmp(seen_aliases[s], alias) == 0) { dup = 1; break; }
            }
            if (!dup && n_seen < REACT_BREADCRUMB_MAX_ALIASES)
                seen_aliases[n_seen++] = alias;
            if (dup) continue;

            char brief[REACT_BREADCRUMB_STRUCT_LEN + 1];
            int msg_clen = (int)chat->msgs[mi].content_len;
            int blen = 0;

            /* Try structural extraction via repomap symbol parser.
             * Gives "func1(), struct_t, MACRO" instead of raw first-N-bytes. */
            if (chat->msgs[mi].tool_name
                && strcmp(chat->msgs[mi].tool_name, "file_read") == 0
                && chat->msgs[mi].tool_path) {
                blen = repomap_file_symbols(content, msg_clen,
                                            chat->msgs[mi].tool_path,
                                            brief, REACT_BREADCRUMB_STRUCT_LEN);
            }

            /* Fallback: first N raw bytes (original behavior) */
            if (blen == 0) {
                blen = msg_clen > REACT_BREADCRUMB_BRIEF_LEN
                    ? REACT_BREADCRUMB_BRIEF_LEN : msg_clen;
                blen = (int)utf8_clamp(content, (size_t)blen);
                memcpy(brief, content, (size_t)blen);
                brief[blen] = '\0';
                for (int b = 0; brief[b]; b++)
                    if (brief[b] == '\n' || brief[b] == '\r') brief[b] = ' ';
            }

            str_appendf(&breadcrumb, "- %s: %s (%s, %d chars)\n",
                chat->msgs[mi].store_alias, brief, role, msg_clen);
            continue;
        }

        /* FIX FLAW 5: Use separate summary budget */
        int msg_clen = (int)chat->msgs[mi].content_len;
        if (strcmp(role, "tool") == 0 && msg_clen < REACT_SUMMARY_TOOL_MIN_LEN)
            continue;
        int clen = msg_clen > max_per_msg ? max_per_msg : msg_clen;
        /* Clamp to UTF-8 boundary to avoid splitting multi-byte chars */
        clen = (int)utf8_clamp(content, (size_t)clen);
        str_appendf(&summary, "[%s]: ", role);
        str_append(&summary, content, (size_t)clen);
        if (msg_clen > max_per_msg)
            str_append_cstr(&summary, "...[truncated]");
        str_append_cstr(&summary, "\n");
        if ((long)summary.len >= breadcrumb_summary_cap) break;
    }

    /* SIMP3 FIX: Simplified — str_steal always returns a valid pointer
     * (empty string if nothing was appended), and free handles it. */
    {
        char *summ_str = str_steal(&summary);
        if (summ_str[0])
            scratchpad_write(&ctx->tools->scratch, "evicted_context", summ_str, 2);
        /* D4 FIX: Defer scratchpad_save() — finalize may strip scratchpad
         * entirely (strategy 2), making this disk write wasted I/O.
         * Save is now done after finalize confirms scratchpad survives. */
        free(summ_str);
    }
    /* BUG 1 FIX: Only return breadcrumb string if entries were added beyond
     * the header. Previously always returned non-NULL (header alone = 48 chars),
     * causing phantom eviction events with useless MEMORY_HINT injection. */
    if (breadcrumb.len > header_len)
        return str_steal(&breadcrumb);
    str_free(&breadcrumb);
    return NULL;
}

/* ── Step 2.5: Tool Lifecycle — Stale Read Detection ───── */

/* Pichay [arXiv:2603.09023] + Headroom read_lifecycle:
 * Detect file_read results that are STALE (file was subsequently edited)
 * or SUPERSEDED (same file was re-read later). Replace stale content with
 * a compact marker ("paging handle") and downgrade importance to LOW.
 *
 * Pichay empirical data: 67% of file reads are stale, 12% superseded.
 * Replacing them with ~80-char markers yields up to 93% context reduction.
 * Fault rate (model needs to re-read): 0.025% across 1.4M evictions.
 *
 * Must run BEFORE the mark phase so stale reads score lowest and get
 * evicted first. Inline replacement gives immediate space savings even
 * before mark-then-sweep fires. */
void evict_lifecycle_stale_reads(llm_chat_t *chat,
                                int evict_start, int evict_end) {
    if (evict_end <= evict_start) return;

    /* Walk forward through evictable region. For each file_read, check if
     * any LATER message has a file_edit/file_write or another file_read on
     * the same path. Simple O(n²) scan — n_evictable is typically < 200. */
    int n_replaced = 0;
    for (int i = evict_start; i < evict_end; i++) {
        const llm_msg_t *m = &chat->msgs[i];
        if (!m->tool_name || !m->tool_path) continue;
        if (strcmp(m->tool_name, "file_read") != 0) continue;
        if (m->importance >= LLM_MSG_IMPORTANCE_HIGH) continue;

        /* Check if any later message edits or re-reads this path */
        int stale_reason = 0;  /* 1=edited, 2=superseded */
        int stale_by = -1;     /* index of the message that made it stale */
        for (int j = i + 1; j < chat->n_msgs; j++) {
            const llm_msg_t *later = &chat->msgs[j];
            if (!later->tool_name || !later->tool_path) continue;
            if (strcmp(later->tool_path, m->tool_path) != 0) continue;
            if (strcmp(later->tool_name, "file_edit") == 0 ||
                strcmp(later->tool_name, "file_write") == 0) {
                stale_reason = 1;
                stale_by = j;
                break;  /* edited — definitely stale */
            }
            if (strcmp(later->tool_name, "file_read") == 0) {
                stale_reason = 2;
                stale_by = j;
                /* Don't break — a later edit is more definitive */
            }
        }

        if (!stale_reason) continue;

        /* Replace content with compact paging handle.
         * The original content is still accessible via store_alias if needed. */
        const char *reason_str = (stale_reason == 1) ? "edited" : "re-read";
        char marker[256];
        snprintf(marker, sizeof(marker),
                 "[Stale: read %s (%zu chars). File was %s in msg %d. "
                 "Re-read if needed.]",
                 m->tool_path, m->content_len, reason_str, stale_by + 1);

        /* Replace content — llm_chat_replace_content updates content_len
         * and total_chars incrementally. */
        llm_chat_replace_content(chat, i, strdup(marker));

        /* Downgrade importance so mark phase evicts these first */
        chat->msgs[i].importance = LLM_MSG_IMPORTANCE_LOW;
        chat->msgs[i].recoverability = LLM_RECOVER_FILE;
        n_replaced++;
    }

    if (n_replaced > 0)
        nash_log("[lifecycle] replaced %d stale/superseded file reads "
                 "with compact markers\n", n_replaced);
}

/* ── Step 4.5: Type-Aware Pre-Compression ──────────────── */

/* CWL [arXiv:2606.11213] graduated compression + Complexity Trap
 * [arXiv:2508.21433] validation: simple type-aware masking matches
 * LLM summarization quality while being free (no model calls).
 *
 * Applied AFTER sweep (marked messages already removed) but BEFORE
 * BM25 compression. Targets specific tool output patterns that
 * compress poorly with generic BM25 but well with structure-aware logic:
 *   - grep_search: head+tail truncation for large result sets
 *   - shell_exec: head+tail truncation for long outputs
 *   - glob_search: truncate long file lists
 *
 * Returns number of messages compressed. */
int evict_type_compress(llm_chat_t *chat, int keep_head, int keep_tail,
                        int compress_min_len) {
    int upper = chat->n_msgs - keep_tail;
    if (upper <= keep_head) return 0;

    int did_compress = 0;

    for (int i = keep_head; i < upper; i++) {
        llm_msg_t *m = &chat->msgs[i];
        if (!m->tool_name || !m->content) continue;
        if (m->importance > LLM_MSG_IMPORTANCE_NORMAL) continue;
        if ((int)m->content_len < compress_min_len) continue;

        /* ── shell_exec: head + tail truncation ── */
        if (strcmp(m->tool_name, "shell_exec") == 0 && (int)m->content_len > 2000) {
            const char *content = m->content;
            int len = (int)m->content_len;
            int head_keep = 500;
            int tail_keep = 500;

            /* Find line boundary near head_keep */
            while (head_keep < len && content[head_keep] != '\n') head_keep++;
            if (head_keep < len) head_keep++;  /* include the newline */

            /* Find line boundary near len - tail_keep */
            int tail_start = len - tail_keep;
            while (tail_start > head_keep && content[tail_start] != '\n') tail_start--;
            if (tail_start > head_keep) tail_start++;  /* start after newline */

            if (tail_start <= head_keep) continue;  /* not enough to truncate */

            int truncated_lines = 0;
            for (int c = head_keep; c < tail_start; c++)
                if (content[c] == '\n') truncated_lines++;

            size_t new_len = (size_t)head_keep + 60 + (size_t)(len - tail_start);
            char *compressed = malloc(new_len + 1);
            if (!compressed) continue;

            int written = snprintf(compressed, new_len + 1,
                "%.*s\n...[ %d lines truncated ]...\n%s",
                head_keep, content, truncated_lines, content + tail_start);
            if (written > 0 && written <= (int)(len * 9 / 10)) {
                llm_chat_replace_content(chat, i, compressed);
                did_compress++;
            } else {
                free(compressed);
            }
        }

        /* ── glob_search: truncate long file lists ── */
        else if (strcmp(m->tool_name, "glob_search") == 0) {
            const char *content = m->content;
            int len = (int)m->content_len;

            /* Count lines (each line is a file path) */
            int n_lines = 0;
            for (int c = 0; c < len; c++)
                if (content[c] == '\n') n_lines++;

            if (n_lines <= 50) continue;  /* not worth truncating */

            /* Keep first 20 lines + last 10 lines */
            int keep_first = 20, keep_last = 10;
            int line = 0;
            int head_end = 0;
            for (int c = 0; c < len && line < keep_first; c++) {
                if (content[c] == '\n') { line++; head_end = c + 1; }
            }

            int tail_lines_seen = 0;
            int tail_begin = len;
            for (int c = len - 1; c >= head_end; c--) {
                if (content[c] == '\n') {
                    tail_lines_seen++;
                    if (tail_lines_seen >= keep_last) { tail_begin = c + 1; break; }
                }
            }

            if (tail_begin <= head_end) continue;

            int omitted = n_lines - keep_first - keep_last;
            size_t new_len = (size_t)head_end + 80 + (size_t)(len - tail_begin);
            char *compressed = malloc(new_len + 1);
            if (!compressed) continue;

            int written = snprintf(compressed, new_len + 1,
                "%.*s...[ %d more files omitted (%d total) ]...\n%s",
                head_end, content, omitted, n_lines, content + tail_begin);
            if (written > 0 && written <= (int)(len * 9 / 10)) {
                llm_chat_replace_content(chat, i, compressed);
                did_compress++;
            } else {
                free(compressed);
            }
        }

        /* ── grep_search: head+tail truncation for large result sets ── */
        else if (strcmp(m->tool_name, "grep_search") == 0 && (int)m->content_len > 2000) {
            const char *content = m->content;
            int len = (int)m->content_len;
            int head_keep = 800;
            int tail_keep = 400;

            /* Find line boundary near head_keep */
            while (head_keep < len && content[head_keep] != '\n') head_keep++;
            if (head_keep < len) head_keep++;

            int tail_start = len - tail_keep;
            while (tail_start > head_keep && content[tail_start] != '\n') tail_start--;
            if (tail_start > head_keep) tail_start++;

            if (tail_start <= head_keep) continue;

            int omitted_lines = 0;
            for (int c = head_keep; c < tail_start; c++)
                if (content[c] == '\n') omitted_lines++;

            size_t new_len = (size_t)head_keep + 60 + (size_t)(len - tail_start);
            char *compressed = malloc(new_len + 1);
            if (!compressed) continue;

            int written = snprintf(compressed, new_len + 1,
                "%.*s\n...[ %d matches omitted ]...\n%s",
                head_keep, content, omitted_lines, content + tail_start);
            if (written > 0 && written <= (int)(len * 9 / 10)) {
                llm_chat_replace_content(chat, i, compressed);
                did_compress++;
            } else {
                free(compressed);
            }
        }
    }

    if (did_compress > 0)
        nash_log("[lifecycle] type-aware compression: %d messages compressed\n",
                 did_compress);
    return did_compress;
}

/* ── Main Entry Point: Mark-then-Sweep ────────────────── */

/* Proposal B: Replace 3-pass + post-verify with mark-then-sweep.
 * All proposals (A–E) and bug fixes (FLAW 1–8) are implemented here.
 *
 * Architecture:
 *   1.   Cleanup: Remove stale injected messages
 *   2.   Partner map: Build once (Proposal E)
 *   2.5  Lifecycle: Replace stale/superseded file reads [Pichay 2603.09023]
 *   3.   Mark: Score all evictable messages, mark lowest for eviction
 *   4.   Sweep: Remove marked messages in reverse order
 *   4.5  Type-compress: Structure-aware pre-compression [CWL 2606.11213]
 *   5.   Compress: BM25 compress surviving messages (Proposal D: target_pct)
 *   6.   Finalize: Re-inject scratchpad + breadcrumbs + hint (Proposal A)
 */
void react_maybe_evict(react_ctx_t *ctx, llm_chat_t *chat, int step,
                       const char *user_query,
                       react_event_fn on_event, void *userdata) {

    /* F1 FIX: Guard against NULL provider before dereferencing cfg. */
    if (!ctx->flags.enable_compaction || !ctx->provider ||
        ctx->provider->cfg.context_size <= 0)
        return;

    eviction_policy_t pol = react_eviction_policy(ctx->tools->cfg);
    long context_budget = react_context_budget(ctx);
    int eviction_pct = pol.trigger_pct;
    int target_pct = pol.target_pct;

    /* DUP3 FIX: Use react_chat_usage_pct convenience helper */
    int usage_pct = react_chat_usage_pct(chat, context_budget);

    int keep_head = react_compute_keep_head(chat);
    int keep_tail = react_compute_keep_tail(chat);

    if (usage_pct <= eviction_pct || chat->n_msgs <= keep_head + keep_tail + 1)
        return;

    /* ── Step 1: Cleanup stale injected messages ── */
    /* D1 FIX: Single-pass multi-type removal instead of 3× O(n) scans. */
    {
        llm_msg_type_t cleanup_types[] = {
            LLM_MSG_SCRATCHPAD, LLM_MSG_EVICTION_SUMMARY, LLM_MSG_MEMORY_HINT
        };
        llm_chat_remove_by_types(chat, cleanup_types, 3);
    }

    /* Recompute after cleanup (indices shifted) */
    keep_head = react_compute_keep_head(chat);
    keep_tail = react_compute_keep_tail(chat);

    long total_chars = react_calc_total_chars(chat);
    usage_pct = react_usage_pct(total_chars, context_budget);

    /* FIX #8: Capture before_pct AFTER cleanup to avoid phantom journal entries.
     * Previously captured before cleanup, so cleanup alone (removing stale SP/
     * summary/hint messages) would cause before_pct != after_pct, triggering
     * a "compaction" journal entry even when no messages were actually evicted. */
    int before_msgs = chat->n_msgs;
    int before_pct = usage_pct;
    /* BUG #2 FIX: Track whether actual eviction/compression occurred.
     * Without this, SP re-injection in finalize_no_evict changes n_msgs,
     * triggering a spurious "compaction" journal entry.
     * Renamed from did_actual_evict: set for both eviction and compression. */
    int did_compact = 0;

    /* FIX #13: Consolidated early-return path — all three "nothing to evict"
     * cases jump here instead of duplicating evict_finalize(NULL) + goto. */
    if (usage_pct <= target_pct)
        goto finalize_no_evict;

    /* ── Step 2: Build partner index (Proposal E) ── */
    int evict_start = keep_head;
    int evict_end = chat->n_msgs - keep_tail;

    /* F2/DD1 FIX: Use shared pair-safe boundary adjustment. */
    evict_adjust_boundaries(chat, &evict_start, &evict_end);

    int n_evictable = evict_end - evict_start;
    if (n_evictable <= 0)
        goto finalize_no_evict;

    evict_partner_map_t pmap = evict_build_partner_map(chat, evict_start, evict_end);

    /* ── Step 2.5: Lifecycle — replace stale/superseded file reads ── */
    evict_lifecycle_stale_reads(chat, evict_start, evict_end);

    /* ── Step 3: Mark phase — score and select messages for eviction ── */
    int *evict_mark = calloc((size_t)n_evictable, sizeof(int));
    if (!evict_mark) {
        evict_free_partner_map(&pmap);
        goto finalize_no_evict;
    }

    /* DUP2 FIX: Use existing helpers instead of manual single-pass loop.
     * O(2n) vs O(n) is negligible since n_msgs is typically < 200. */
    long head_chars = react_head_chars(chat, evict_start);
    long tail_chars = react_tail_chars(chat, evict_end);
    long evictable_chars = total_chars - head_chars - tail_chars;

    /* Compaction floor — minimum evictable content to retain
     * FIX #5: Pass tail_chars so floor is based on evictable capacity only. */
    long floor_chars = react_calc_floor_chars_pol(chat, evict_start, evict_end,
                                                   context_budget,
                                                   head_chars, tail_chars, &pol);

    /* Proposal D: Compute effective target for sweep phase only.
     * Compress phase targets target_pct (it doesn't re-inject).
     * Sweep phase targets effective_target_pct (accounts for re-injection). */
    long actual_sp_size = (long)scratchpad_total_size(&ctx->tools->scratch);
    /* D5 FIX: Cap SP contribution to reinject_est at the absolute SP budget.
     * After cleanup removes the old SP, remaining space is artificially large,
     * inflating rel_cap and thus reinject_est. Using min(actual, abs_cap)
     * gives a deterministic, non-inflated estimate. */
    long sp_abs_cap = (context_budget > 0)
        ? context_budget * pol.sp_budget_pct / 100 : pol.sp_fallback;
    long sp_reinject_est = actual_sp_size < sp_abs_cap
        ? actual_sp_size : sp_abs_cap;
    /* Breadcrumb budget caps from policy */
    long bc_index_cap = react_budget_cap(context_budget,
                                          pol.bc_index_pct,
                                          REACT_BREADCRUMB_SUBCAP_MIN);
    long bc_summary_cap = react_budget_cap(context_budget,
                                            pol.bc_summary_pct,
                                            REACT_BREADCRUMB_SUBCAP_MIN);
    long reinject_est = sp_reinject_est + bc_index_cap + bc_summary_cap + REACT_REINJECT_PAD;

    int effective_target_pct = target_pct;
    if (context_budget > 0) {
        int reinject_pct = (int)(100L * reinject_est / context_budget);
        effective_target_pct = target_pct - reinject_pct;
        if (effective_target_pct < REACT_EFF_TARGET_MIN_PCT)
            effective_target_pct = REACT_EFF_TARGET_MIN_PCT;
    }

    /* Review B4: Use generic mark-candidates with progressive scoring callback.
     * target_remaining = budget * effective_target_pct / 100 - head_chars
     * represents the maximum non-head chars we want to retain. */
    long remaining_nonhead = tail_chars + evictable_chars;
    long target_remaining = (context_budget > 0)
        ? (context_budget * effective_target_pct / 100 - head_chars)
        : (remaining_nonhead * effective_target_pct / 100);
    if (target_remaining < floor_chars) target_remaining = floor_chars;

    /* ── Step 3.1: Build semantic scoring context ── */
    /* Pre-compute cosine similarities between task and each evictable message.
     * Uses existing ONNX embedding infrastructure (embedding.h).
     * Graceful fallback: if embeddings unavailable, similarities stays NULL
     * and the scorer uses the base formula only (zero overhead). */
    evict_score_ctx_t score_ctx = {0};
    score_ctx.pmap = &pmap;

    embed_ctx_t *emb = memory_embed_ctx(ctx->tools->memory);
    if (emb && emb->available) {
        /* Build task text: user query + last assistant thought.
         * This captures both WHAT the user asked and WHERE the model is. */
        str_t task_text = str_new(512);

        /* Find user query */
        for (int i = 0; i < chat->n_msgs; i++) {
            if (chat->msgs[i].msg_type == LLM_MSG_USER_QUERY
                && chat->msgs[i].content) {
                str_append_cstr(&task_text, chat->msgs[i].content);
                break;
            }
        }

        /* Append last assistant thought (recent reasoning direction) */
        for (int i = chat->n_msgs - 1; i >= 0; i--) {
            if (chat->msgs[i].role && strcmp(chat->msgs[i].role, "assistant") == 0
                && chat->msgs[i].content && chat->msgs[i].content[0]) {
                str_append_cstr(&task_text, "\n");
                int tlen = (int)chat->msgs[i].content_len;
                if (tlen > REACT_THOUGHT_TRUNC_LEN) tlen = REACT_THOUGHT_TRUNC_LEN;
                str_append(&task_text, chat->msgs[i].content, (size_t)tlen);
                break;
            }
        }

        if (task_text.len > 0) {
            /* Truncate to embedding model's max input */
            int max_chars = embed_max_input_chars(emb);
            if ((int)task_text.len > max_chars)
                task_text.data[utf8_clamp(task_text.data, (size_t)max_chars)] = '\0';

            embed_vec_t task_vec = embed_text(emb, task_text.data);
            if (task_vec.data) {
                /* Batch-embed all evictable messages (first 500 chars each) */
                const char **texts = malloc((size_t)n_evictable * sizeof(char *));
                char **trunc_bufs = calloc((size_t)n_evictable, sizeof(char *));

                if (texts && trunc_bufs) {
                    for (int i = 0; i < n_evictable; i++) {
                        int mi = evict_start + i;
                        const char *c = chat->msgs[mi].content;
                        int cl = (int)chat->msgs[mi].content_len;
                        if (!c || cl < 50) {
                            texts[i] = "";
                            continue;
                        }
                        if (cl > REACT_EMBED_TRUNC_CHARS) {
                            trunc_bufs[i] = malloc(REACT_EMBED_TRUNC_CHARS + 1);
                            if (trunc_bufs[i]) {
                                size_t safe = utf8_clamp(c, REACT_EMBED_TRUNC_CHARS);
                                memcpy(trunc_bufs[i], c, safe);
                                trunc_bufs[i][safe] = '\0';
                                texts[i] = trunc_bufs[i];
                            } else {
                                texts[i] = "";
                            }
                        } else {
                            texts[i] = c;
                        }
                    }

                    int out_count = 0;
                    embed_vec_t *msg_vecs = embed_text_batch(emb, texts, n_evictable, &out_count);

                    if (msg_vecs && out_count == n_evictable) {
                        score_ctx.similarities = calloc((size_t)chat->n_msgs, sizeof(float));
                        score_ctx.n_msgs = chat->n_msgs;
                        if (score_ctx.similarities) {
                            for (int i = 0; i < n_evictable; i++) {
                                int mi = evict_start + i;
                                score_ctx.similarities[mi] = embed_cosine_sim(
                                    &task_vec, &msg_vecs[i]);
                            }
                        }
                    }

                    /* Cleanup batch results */
                    if (msg_vecs) {
                        for (int i = 0; i < out_count; i++)
                            embed_vec_free(&msg_vecs[i]);
                        free(msg_vecs);
                    }
                    for (int i = 0; i < n_evictable; i++)
                        free(trunc_bufs[i]);
                }
                free(trunc_bufs);
                free(texts);
                embed_vec_free(&task_vec);
            }
        }
        str_free(&task_text);
    }

    int n_to_evict = evict_mark_candidates(chat, evict_start, evict_end,
                                            &pmap, floor_chars,
                                            remaining_nonhead, tail_chars,
                                            target_remaining,
                                            evict_score_progressive_semantic,
                                            &score_ctx,
                                            evict_mark);

    free(score_ctx.similarities);  /* NULL-safe */

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

    /* ── Step 4.5: Type-aware pre-compression [arXiv:2508.21433] ── */
    /* CWL graduated compression: apply structure-aware truncation BEFORE
     * generic BM25. Shell output, grep results, and glob lists compress
     * much better with type-specific head+tail logic than BM25 extraction. */
    evict_type_compress(chat, keep_head, keep_tail, pol.compress_min_len);

    /* ── Step 5: Compress phase — BM25 compress surviving messages ── */
    /* Flaw 1 FIX: Compress runs AFTER sweep so it only processes messages
     * that survived eviction. Previously compress ran before sweep, wasting
     * CPU on marked messages and causing incorrect budget tracking. */
    /* Proposal D: Compress targets target_pct (not effective_target_pct) */
    /* FIX #9: Reuse keep_head/keep_tail from step 2 — sweep only removes
     * messages from the evictable region (between head and tail), so head
     * CRITICAL count and tail count are unchanged. */
    total_chars = react_calc_total_chars(chat);
    usage_pct = react_usage_pct(total_chars, context_budget);
    char *bm25_query = react_build_bm25_query(chat, user_query, &ctx->tools->scratch);
    if (usage_pct > target_pct) {
        evict_compress(chat, keep_head, keep_tail, bm25_query,
                       target_pct, context_budget, &pol);
    }
    free(bm25_query);

    /* ── Step 5.5: Reasoning summarization [Rec #6: Hybrid context management]
     * Terminus 2 uses 3-step summarization for reasoning chains; Nash uses
     * selective eviction. This hybrid approach compresses ASSISTANT reasoning
     * messages (where narrative coherence > exact text) while preserving
     * tool results exactly (where exact details matter).
     *
     * Heuristic: for long assistant messages, keep the first paragraph
     * (initial reasoning/plan) and last paragraph (conclusion/decision),
     * replacing the middle with a brief marker. No LLM call required. */
    {
        int reasoning_min = 1200; /* only compress assistant msgs > this */
        total_chars = react_calc_total_chars(chat);
        usage_pct = react_usage_pct(total_chars, context_budget);
        if (usage_pct > target_pct) {
            for (int mi = keep_head; mi < chat->n_msgs - keep_tail; mi++) {
                if (!chat->msgs[mi].role ||
                    strcmp(chat->msgs[mi].role, "assistant") != 0)
                    continue;
                if ((int)chat->msgs[mi].content_len < reasoning_min)
                    continue;
                /* Skip if it contains tool_calls JSON (those are structural) */
                if (chat->msgs[mi].tool_calls_json)
                    continue;

                const char *c = chat->msgs[mi].content;
                int clen = (int)chat->msgs[mi].content_len;

                /* Find end of first paragraph (double newline or first 400 chars) */
                const char *first_end = strstr(c, "\n\n");
                int first_len;
                if (first_end && (first_end - c) < 400)
                    first_len = (int)(first_end - c);
                else
                    first_len = clen < 400 ? clen : 400;

                /* Find start of last paragraph */
                const char *last_start = c + clen;
                for (int i = clen - 2; i > first_len; i--) {
                    if (c[i] == '\n' && c[i+1] == '\n') {
                        last_start = c + i + 2;
                        break;
                    }
                }
                int last_len = (int)(c + clen - last_start);
                if (last_len > 400) {
                    last_start = c + clen - 400;
                    last_len = 400;
                }

                /* Only compress if we'd save significant space */
                int new_len = first_len + last_len + 40;
                if (new_len >= clen - 200) continue;

                /* Build compressed version */
                str_t compressed = str_new(new_len + 60);
                str_append(&compressed, c, first_len);
                str_append_cstr(&compressed,
                    "\n\n[...reasoning compressed...]\n\n");
                str_append(&compressed, last_start, last_len);

                llm_chat_replace_content(chat, mi,
                    str_steal(&compressed));
                /* str_steal() took ownership — no str_free needed */

                /* Re-check if we've compressed enough */
                total_chars = react_calc_total_chars(chat);
                usage_pct = react_usage_pct(total_chars, context_budget);
                if (usage_pct <= target_pct) break;
            }
        }
    }

    /* ── Step 6: Finalize (Proposal A) ── */
    /* B4 FIX: Reuse keep_head from step 5 — evict_compress only changes
     * content (llm_chat_replace_content), not message structure. */
    (void)evict_finalize(ctx, chat, keep_head, target_pct, context_budget,
                         bc_str, n_to_evict, step, on_event, userdata);

    /* D4 FIX: Save scratchpad to disk after finalize confirms it survived.
     * Previously saved in evict_build_breadcrumbs before finalize could strip it. */
    scratchpad_save(&ctx->tools->scratch, ctx->tools->session_dir);
    did_compact = 1;
    goto journal;

finalize_no_evict:
    /* FIX #13 + D4 FIX: Pre-check SP injection feasibility before calling
     * evict_finalize. Previously, finalize_no_evict unconditionally called
     * evict_finalize which injected SP, potentially pushing usage above
     * target_pct and paradoxically triggering emergency eviction.
     * Now we only inject SP if it won't overshoot, avoiding the paradox. */
    {
        long cur_chars = react_calc_total_chars(chat);
        long sp_size = (long)scratchpad_total_size(&ctx->tools->scratch);
        long target_chars = (context_budget > 0)
            ? context_budget * target_pct / 100 : 0;
        /* If SP injection would push us over target, inject a smaller SP
         * or skip finalize entirely to avoid the emergency eviction paradox. */
        if (target_chars > 0 && cur_chars + sp_size + REACT_SP_PREFIX_LEN > target_chars) {
            /* Only inject SP if there's room — at reduced budget */
            long room = target_chars - cur_chars - REACT_SP_PREFIX_LEN;
            if (room > (long)pol.sp_shrink_min) {
                char *sp = scratchpad_serialize_budget(&ctx->tools->scratch, (size_t)room);
                react_inject_scratchpad_msg(chat, keep_head, sp);
                free(sp);
            }
            /* Skip evict_finalize — no eviction needed, SP handled above */
        } else {
            int n_emergency = evict_finalize(ctx, chat, keep_head, target_pct,
                                             context_budget, NULL, 0, step,
                                             on_event, userdata);
            if (n_emergency > 0)
                did_compact = 1;
        }
    }

journal:
    /* BUG #2 FIX: Only log compaction journal entry when actual eviction or
     * compression occurred. Previously SP re-injection in finalize_no_evict
     * changed n_msgs, producing spurious "compaction" entries. */
    if (ctx->tools->journal && did_compact) {
        long after_chars = react_calc_total_chars(chat);
        int after_pct = react_usage_pct(after_chars, context_budget);
        if (after_pct != before_pct || chat->n_msgs != before_msgs) {
            cJSON *params = cJSON_CreateObject();
            cJSON_AddNumberToObject(params, "before_msgs", before_msgs);
            cJSON_AddNumberToObject(params, "after_msgs", chat->n_msgs);
            cJSON_AddNumberToObject(params, "before_pct", before_pct);
            cJSON_AddNumberToObject(params, "after_pct", after_pct);
            journal_append(ctx->tools->journal, ctx->tools->react_loop,
                           step, "compaction", params, NULL, 0, 0, NULL, NULL, 0);
            cJSON_Delete(params);
        }
    }
}
