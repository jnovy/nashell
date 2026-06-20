/* react_eviction.c — Progressive context eviction (Harness-1 §3.5).
 * Implements multi-pass importance-aware context compaction:
 *   Pass 1: Strip LOW importance messages (pair-safe with scanning)
 *   Pass 2: Compress NORMAL messages to top-N sentences (BM25)
 *   Pass 3: Recoverability-aware scored eviction with breadcrumb generation
 *
 * Decomposed from monolithic react_maybe_evict() into separate pass functions
 * for testability and readability. All magic numbers centralized in
 * react_internal.h as REACT_* constants.
 */

#include "react_internal.h"
#include "compress.h"

/* ── Comparison functions for qsort ───────────────────── */

typedef struct { int idx; int len; } compress_cand_t;

static int cmp_compress_desc(const void *a, const void *b) {
    return ((const compress_cand_t *)b)->len - ((const compress_cand_t *)a)->len;
}

typedef struct { int idx; int score; } evict_scored_t;

static int cmp_evict_score_asc(const void *a, const void *b) {
    return ((const evict_scored_t *)a)->score - ((const evict_scored_t *)b)->score;
}

/* ── Scratchpad Re-injection ──────────────────────────── */

/* Re-inject scratchpad at insert_pos in chat.
 * Returns the serialized scratchpad size in chars (0 if nothing injected).
 * Shared between progressive and emergency eviction. */
long react_reinject_scratchpad(react_ctx_t *ctx, llm_chat_t *chat,
                               int insert_pos) {
    long context_budget = react_context_budget(ctx);

    /* Cap scratchpad to the LESSER of:
     *   - SCRATCHPAD_BUDGET_PCT% of total context (absolute cap)
     *   - SCRATCHPAD_MAX_OF_REMAINING_PCT% of post-eviction content
     * Prevents scratchpad from drowning out conversation after heavy eviction. */
    size_t sp_max;
    if (context_budget > 0) {
        size_t abs_cap = (size_t)(context_budget
                                 * REACT_SCRATCHPAD_BUDGET_PCT / 100);
        long remaining = react_calc_total_chars(chat);
        size_t rel_cap = (size_t)(remaining
                                 * REACT_SCRATCHPAD_MAX_OF_REMAINING_PCT / 100);
        sp_max = abs_cap < rel_cap ? abs_cap : rel_cap;
        if (sp_max < REACT_SP_MIN) sp_max = REACT_SP_MIN;
    } else {
        sp_max = REACT_SP_FALLBACK;
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

/* ── Pass 1: Strip LOW importance messages ────────────── */

/* Remove all LOW-importance messages in the evictable range [keep_head, n-keep_tail).
 * Pair-safe: uses react_find_tool_partner() scanning (not adjacency).
 * Removes in reverse order to preserve indices.
 * Returns count of messages removed. */
static int evict_pass1_strip_low(llm_chat_t *chat, int keep_head, int keep_tail) {
    int removed = 0;
    for (int i = chat->n_msgs - keep_tail - 1; i >= keep_head; i--) {
        if (chat->msgs[i].importance != LLM_MSG_IMPORTANCE_LOW)
            continue;

        /* Find partner using scanning (fixes bug #2: adjacency assumption) */
        int partner = react_find_tool_partner(chat, i, keep_head,
                                               chat->n_msgs - keep_tail);

        if (partner >= 0 && partner > i) {
            /* Partner is after us — remove it first to preserve our index */
            llm_chat_remove_range(chat, partner, partner + 1);
            llm_chat_remove_range(chat, i, i + 1);
            removed += 2;
        } else if (partner >= 0 && partner < i) {
            /* Partner is before us — we already passed it, remove both */
            llm_chat_remove_range(chat, i, i + 1);
            llm_chat_remove_range(chat, partner, partner + 1);
            i--;  /* adjust for the removed partner before us */
            removed += 2;
        } else {
            /* Standalone message */
            llm_chat_remove_range(chat, i, i + 1);
            removed++;
        }
    }
    return removed;
}

/* ── Pass 2: BM25 compression of NORMAL messages ─────── */

/* Compress NORMAL-importance messages using BM25 relevance scoring.
 * Targets the largest messages first for maximum space savings.
 * Returns 1 if any compression was applied, 0 otherwise. */
static int evict_pass2_compress(llm_chat_t *chat, int keep_head, int keep_tail,
                                const char *bm25_query, int effective_target_pct,
                                long context_budget) {
    int n_middle = chat->n_msgs - keep_head - keep_tail;
    if (n_middle <= 0) return 0;

    /* Dynamic compression threshold based on average message size */
    int compress_threshold;
    {
        long middle_chars = 0;
        for (int i = keep_head; i < chat->n_msgs - keep_tail; i++)
            if (chat->msgs[i].content)
                middle_chars += (long)strlen(chat->msgs[i].content);
        compress_threshold = (int)(middle_chars / n_middle / 2);
        if (compress_threshold < REACT_COMPRESS_THRESH_MIN)
            compress_threshold = REACT_COMPRESS_THRESH_MIN;
    }

    /* Collect compression candidates */
    int n_candidates = 0, cand_cap = 0;
    compress_cand_t *candidates = NULL;

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

    /* Sort by length descending (largest first) */
    if (n_candidates > 1)
        qsort(candidates, (size_t)n_candidates, sizeof(compress_cand_t), cmp_compress_desc);

    int did_compress = 0;
    long total_chars = react_calc_total_chars(chat);

    for (int ci = 0; ci < n_candidates; ci++) {
        int i = candidates[ci].idx;
        int old_len = (int)strlen(chat->msgs[i].content);
        /* Scale compression params with message size */
        int scaled_units = old_len / 1000;
        if (scaled_units < REACT_COMPRESS_MIN_UNITS) scaled_units = REACT_COMPRESS_MIN_UNITS;
        int scaled_chars = old_len / 4;
        if (scaled_chars < REACT_COMPRESS_MIN_CHARS) scaled_chars = REACT_COMPRESS_MIN_CHARS;
        char *compressed = compress_to_relevant(
            chat->msgs[i].content, bm25_query, scaled_units, scaled_chars);
        if (compressed) {
            int new_len = (int)strlen(compressed);
            if (new_len < old_len) {
                free(chat->msgs[i].content);
                chat->msgs[i].content = compressed;
                total_chars -= (old_len - new_len);
                did_compress = 1;
            } else {
                free(compressed);
            }
            if (react_usage_pct(total_chars, context_budget) <= effective_target_pct)
                break;
        }
    }
    free(candidates);
    return did_compress;
}

/* ── Pass 3: Scored eviction with breadcrumb generation ── */

/* Compute eviction scores for messages in [evict_start, evict_end).
 * Lower score = evict first. Score formula:
 *   importance * IMP_WEIGHT - recoverability * REC_WEIGHT - size_bonus + pos_norm
 * Returns malloc'd array of scored entries, or NULL. Sets *n_scored. */
static evict_scored_t *evict_score_messages(const llm_chat_t *chat,
                                            int evict_start, int evict_end,
                                            int *n_scored) {
    int n = evict_end - evict_start;
    if (n <= 0) { *n_scored = 0; return NULL; }

    evict_scored_t *scored = malloc((size_t)n * sizeof(evict_scored_t));
    if (!scored) { *n_scored = 0; return NULL; }

    for (int i = 0; i < n; i++) {
        int mi = evict_start + i;
        int imp = (int)chat->msgs[mi].importance;
        int rec = (int)chat->msgs[mi].recoverability;
        int msg_len = chat->msgs[mi].content
            ? (int)strlen(chat->msgs[mi].content) : 0;
        scored[i].idx = i;
        int pos_norm = (n > 1) ? (i * REACT_SCORE_POS_RANGE / (n - 1)) : 0;
        int size_bonus = (rec > 0 && msg_len > REACT_SCORE_SIZE_THRESH)
            ? (msg_len / REACT_SCORE_SIZE_DIV) * rec : 0;
        if (size_bonus > REACT_SCORE_SIZE_MAX) size_bonus = REACT_SCORE_SIZE_MAX;
        scored[i].score = imp * REACT_SCORE_IMP_WEIGHT
                        - rec * REACT_SCORE_REC_WEIGHT
                        - size_bonus + pos_norm;
    }

    qsort(scored, (size_t)n, sizeof(evict_scored_t), cmp_evict_score_asc);
    *n_scored = n;
    return scored;
}

/* Adjust eviction boundary to avoid splitting tool_call/result pairs.
 * Uses a bounded loop with monotonic progress tracking to prevent oscillation
 * (fixes bug #6). Returns adjusted evict_end. */
static int evict_adjust_boundary(const llm_chat_t *chat, int evict_start,
                                 int evict_end, int keep_tail) {
    int max_end = chat->n_msgs - keep_tail;
    for (int iter = 0; evict_end > evict_start && iter < REACT_BOUNDARY_ADJ_MAX; iter++) {
        if (evict_end < chat->n_msgs && chat->msgs[evict_end].tool_call_id) {
            /* At a tool_result — try to include the complete pair */
            if (evict_end + 1 <= max_end) {
                evict_end++;
            } else {
                evict_end--;
            }
            continue;
        }
        if (evict_end - 1 >= evict_start &&
            chat->msgs[evict_end - 1].tool_calls_json) {
            /* Last evictable is a tool_call with no result in pool */
            if (evict_end + 1 <= max_end) {
                evict_end++;
            } else {
                evict_end--;
            }
            continue;
        }
        break;
    }
    return evict_end;
}

/* Build breadcrumb index and eviction summary for evicted messages.
 * Writes evicted_context section to scratchpad.
 * Returns a malloc'd breadcrumb string (caller frees), or NULL. */
static char *evict_build_breadcrumbs(react_ctx_t *ctx, const llm_chat_t *chat,
                                     int evict_start, int n_evictable,
                                     const int *evict_mark, int n_to_evict,
                                     long breadcrumb_cap, long context_budget) {
    str_t breadcrumb = str_new(512);
    size_t sp_budget = (size_t)(context_budget * REACT_SCRATCHPAD_BUDGET_PCT / 100);
    size_t summary_budget = sp_budget / 3;
    if (summary_budget > (size_t)breadcrumb_cap)
        summary_budget = (size_t)breadcrumb_cap;
    str_t summary = str_new(summary_budget);
    int max_per_msg = n_to_evict > 0
        ? (int)(summary_budget / (unsigned)n_to_evict) : REACT_SUMMARY_PER_MSG_MIN;
    if (max_per_msg < REACT_SUMMARY_PER_MSG_MIN) max_per_msg = REACT_SUMMARY_PER_MSG_MIN;
    if (max_per_msg > REACT_SUMMARY_PER_MSG_MAX) max_per_msg = REACT_SUMMARY_PER_MSG_MAX;

    str_append_cstr(&breadcrumb, "[EVICTED CONTEXT — recoverable via file_read]\n");
    int n_breadcrumbs = 0;

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

            char brief[REACT_BREADCRUMB_BRIEF_LEN + 1];
            int blen = (int)strlen(content);
            if (blen > REACT_BREADCRUMB_BRIEF_LEN) blen = REACT_BREADCRUMB_BRIEF_LEN;
            memcpy(brief, content, (size_t)blen);
            brief[blen] = '\0';
            for (int b = 0; brief[b]; b++)
                if (brief[b] == '\n' || brief[b] == '\r') brief[b] = ' ';
            str_appendf(&breadcrumb, "- %s: %s (%s, %d chars)\n",
                chat->msgs[mi].store_alias, brief, role, (int)strlen(content));
            n_breadcrumbs++;
            continue;
        }

        if (strcmp(role, "tool") == 0 && (int)strlen(content) < REACT_SUMMARY_TOOL_MIN_LEN)
            continue;
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

/* Execute Pass 3 scored eviction. Marks messages for eviction based on
 * recoverability-aware scoring, respecting compaction floor.
 * Returns count of messages evicted. */
static int evict_pass3_scored(react_ctx_t *ctx, llm_chat_t *chat,
                              int keep_head, int keep_tail,
                              int target_pct, long context_budget,
                              long breadcrumb_cap, int step,
                              react_event_fn on_event, void *userdata) {
    (void)target_pct;  /* caller gates entry; scoring uses floor, not target */
    int evict_start = keep_head;
    int evict_end = chat->n_msgs - keep_tail;

    /* Adjust boundary to avoid splitting pairs (fixes bug #6) */
    evict_end = evict_adjust_boundary(chat, evict_start, evict_end, keep_tail);

    int n_evictable = evict_end - evict_start;
    if (n_evictable <= 0) return 0;

    /* Score all candidates */
    int n_scored;
    evict_scored_t *scored = evict_score_messages(chat, evict_start, evict_end, &n_scored);
    if (!scored) return 0;

    int *evict_mark = calloc((size_t)n_evictable, sizeof(int));
    if (!evict_mark) { free(scored); return 0; }

    /* Compaction floor: keep at least FLOOR_PCT% of non-head context */
    long head_chars = 0;
    for (int ki = 0; ki < evict_start; ki++)
        if (chat->msgs[ki].content)
            head_chars += (long)strlen(chat->msgs[ki].content);
    long floor_chars = (context_budget - head_chars)
                     * REACT_EVICT_FLOOR_PCT / 100;
    if (floor_chars < REACT_EVICT_FLOOR_MIN_CHARS)
        floor_chars = REACT_EVICT_FLOOR_MIN_CHARS;

    long tail_chars = 0;
    for (int ki = evict_end; ki < chat->n_msgs; ki++)
        if (chat->msgs[ki].content)
            tail_chars += (long)strlen(chat->msgs[ki].content);

    long evictable_chars = 0;
    for (int ki = evict_start; ki < evict_end; ki++)
        if (chat->msgs[ki].content)
            evictable_chars += (long)strlen(chat->msgs[ki].content);

    long remaining_chars = tail_chars + evictable_chars;
    int n_to_evict = 0;

    for (int si = 0; si < n_scored; si++) {
        int ri = scored[si].idx;
        int mi = evict_start + ri;
        long msg_chars = chat->msgs[mi].content
            ? (long)strlen(chat->msgs[mi].content) : 0;

        if (evict_mark[ri]) continue;
        if (chat->msgs[mi].importance >= LLM_MSG_IMPORTANCE_HIGH) continue;
        if (remaining_chars - msg_chars < floor_chars) break;

        /* Find partner using shared helper (fixes bug #3: type-based matching) */
        int partner_mi = react_find_tool_partner(chat, mi, evict_start, evict_end);
        int pair_ri = (partner_mi >= 0) ? partner_mi - evict_start : -1;
        long pair_chars = 0;
        if (pair_ri >= 0 && pair_ri < n_evictable) {
            pair_chars = chat->msgs[partner_mi].content
                ? (long)strlen(chat->msgs[partner_mi].content) : 0;
        } else {
            pair_ri = -1;
        }

        /* Check floor for total pair cost */
        if (remaining_chars - msg_chars - pair_chars < floor_chars) break;

        evict_mark[ri] = 1;
        remaining_chars -= msg_chars;
        n_to_evict++;

        if (pair_ri >= 0) {
            evict_mark[pair_ri] = 1;
            remaining_chars -= pair_chars;
            n_to_evict++;
        }
    }
    free(scored);

    if (n_to_evict <= 0) {
        free(evict_mark);
        return 0;
    }

    /* Build breadcrumbs before removing messages */
    char *bc_str = evict_build_breadcrumbs(ctx, chat, evict_start, n_evictable,
                                            evict_mark, n_to_evict,
                                            breadcrumb_cap, context_budget);

    /* Remove marked messages in REVERSE order to preserve indices */
    for (int ri = n_evictable - 1; ri >= 0; ri--) {
        if (evict_mark[ri])
            llm_chat_remove_range(chat, evict_start + ri, evict_start + ri + 1);
    }
    free(evict_mark);

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

    /* Re-inject scratchpad */
    int insert_pos = keep_head;
    react_reinject_scratchpad(ctx, chat, insert_pos);
    insert_pos = keep_head + 1;  /* after scratchpad */

    /* Inject breadcrumb index */
    if (bc_str) {
        llm_chat_insert_typed(chat, insert_pos,
            "user", bc_str, LLM_MSG_EVICTION_SUMMARY);
        free(bc_str);
        insert_pos++;
    }

    /* Post-compaction hint */
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

    return n_to_evict;
}

/* ── Post-eviction verification ──────────────────────── */

/* Verify context is within budget after eviction.
 * If still over, progressively shrink scratchpad, then emergency evict.
 * FIX #5: Remeasures actual_sp_size AFTER Pass 3 (which may have written
 * evicted_context to scratchpad), preventing stale size calculation. */
static void evict_post_verify(react_ctx_t *ctx, llm_chat_t *chat,
                              int keep_head, int eviction_pct,
                              long context_budget) {
    long total_chars = react_calc_total_chars(chat);
    int usage_pct = react_usage_pct(total_chars, context_budget);

    if (usage_pct <= eviction_pct) return;

    /* FIX #5: Measure actual scratchpad size NOW (after Pass 3 may have
     * written to it), not from a stale pre-eviction measurement. */
    long actual_sp_size = 0;
    {
        char *sp_preview = scratchpad_serialize_budget(&ctx->tools->scratch, SIZE_MAX);
        if (sp_preview) {
            actual_sp_size = (long)strlen(sp_preview);
            free(sp_preview);
        }
    }

    nash_log("[eviction] post-eviction usage %d%% > trigger %d%% — "
             "shrinking scratchpad", usage_pct, eviction_pct);
    llm_chat_remove_by_type(chat, LLM_MSG_SCRATCHPAD);

    /* Try proportional reduction first */
    total_chars = react_calc_total_chars(chat);
    long overshoot = total_chars - (context_budget * eviction_pct / 100);
    long sp_chars = actual_sp_size - overshoot;
    if (sp_chars > REACT_SP_SHRINK_MIN) {
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

    total_chars = react_calc_total_chars(chat);
    usage_pct = react_usage_pct(total_chars, context_budget);
    if (usage_pct > eviction_pct) {
        nash_log("[eviction] still %d%% after scratchpad shrink — "
                 "stripping entirely", usage_pct);
        llm_chat_remove_by_type(chat, LLM_MSG_SCRATCHPAD);
        total_chars = react_calc_total_chars(chat);
        usage_pct = react_usage_pct(total_chars, context_budget);
        if (usage_pct > eviction_pct) {
            nash_log("[eviction] still %d%% — emergency eviction", usage_pct);
            react_emergency_evict(chat, context_budget);
        }
    }
}

/* ── Main Entry Point ─────────────────────────────────── */

/* Check context usage and evict old messages if over threshold.
 * Orchestrates the 3-pass eviction pipeline with pre/post verification. */
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

    int keep_head = react_compute_keep_head(chat);
    int keep_tail = react_compute_keep_tail(chat);

    long total_chars = react_calc_total_chars(chat);
    int usage_pct = react_usage_pct(total_chars, context_budget);

    if (usage_pct <= eviction_pct || chat->n_msgs <= keep_head + keep_tail + 1)
        return;

    /* Capture pre-compaction state for journal logging */
    int before_msgs = chat->n_msgs;
    int before_pct = usage_pct;
    int did_evict = 0;

    /* Clean stale injected messages when eviction triggers */
    llm_chat_remove_by_type(chat, LLM_MSG_SCRATCHPAD);
    llm_chat_remove_by_type(chat, LLM_MSG_EVICTION_SUMMARY);
    llm_chat_remove_by_type(chat, LLM_MSG_MEMORY_HINT);

    total_chars = react_calc_total_chars(chat);
    usage_pct = react_usage_pct(total_chars, context_budget);

    /* If cleanup alone brought us under target, re-inject and return */
    if (usage_pct <= target_pct) {
        react_reinject_scratchpad(ctx, chat, keep_head);
        did_evict = 1;
        goto journal;
    }

    /* ── Pass 1: Strip LOW importance messages ── */
    if (evict_pass1_strip_low(chat, keep_head, keep_tail) > 0) {
        did_evict = 1;
        usage_pct = react_usage_pct(react_calc_total_chars(chat), context_budget);
        if (usage_pct <= target_pct) {
            react_reinject_scratchpad(ctx, chat, keep_head);
            goto journal;
        }
    }

    /* Compute re-injection estimate for effective target calculation */
    long actual_sp_size = 0;
    {
        char *sp_preview = scratchpad_serialize_budget(&ctx->tools->scratch, SIZE_MAX);
        if (sp_preview) {
            actual_sp_size = (long)strlen(sp_preview);
            free(sp_preview);
        }
    }
    long breadcrumb_cap = (long)(context_budget * REACT_SCRATCHPAD_BUDGET_PCT / REACT_BREADCRUMB_CAP_DIV);
    if (breadcrumb_cap < REACT_BREADCRUMB_CAP_MIN)
        breadcrumb_cap = REACT_BREADCRUMB_CAP_MIN;
    long reinject_est = actual_sp_size + breadcrumb_cap + REACT_REINJECT_PAD;

    int effective_target_pct = target_pct;
    if (context_budget > 0) {
        int reinject_pct = (int)(100L * reinject_est / context_budget);
        effective_target_pct = target_pct - reinject_pct;
        if (effective_target_pct < REACT_EFF_TARGET_MIN_PCT)
            effective_target_pct = REACT_EFF_TARGET_MIN_PCT;
    }

    /* Build shared BM25 query for Pass 2 */
    char *bm25_query = react_build_bm25_query(chat, user_query, &ctx->tools->scratch);

    /* ── Pass 2: BM25 compression ── */
    usage_pct = react_usage_pct(react_calc_total_chars(chat), context_budget);
    if (usage_pct > target_pct) {
        if (evict_pass2_compress(chat, keep_head, keep_tail,
                                 bm25_query, effective_target_pct, context_budget))
            did_evict = 1;
    }

    /* ── Pass 3: Scored eviction ── */
    usage_pct = react_usage_pct(react_calc_total_chars(chat), context_budget);
    if (usage_pct > target_pct) {
        int n = evict_pass3_scored(ctx, chat, keep_head, keep_tail,
                                   target_pct, context_budget, breadcrumb_cap,
                                   step, on_event, userdata);
        if (n > 0) did_evict = 1;
    }

    free(bm25_query);

    /* Post-eviction verification */
    if (did_evict)
        evict_post_verify(ctx, chat, keep_head, eviction_pct, context_budget);

journal:
    /* Log compaction event to journal if anything changed */
    if (did_evict && ctx->tools->journal) {
        long after_chars = react_calc_total_chars(chat);
        int after_pct = react_usage_pct(after_chars, context_budget);
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
