/* react_error.c — Error recovery for NULL LLM responses.
 * Includes emergency eviction and the 5-tier retry strategy. */

#include "react_internal.h"

/* ── Emergency Eviction ────────────────────────────────── */

/* Emergency scoring callback — aligned with progressive scoring.
 * Includes size awareness and normalized position, matching the progressive
 * scorer's preference order so eviction decisions are consistent when
 * Strategy 2 fires after progressive eviction. userdata is unused (NULL). */
static int evict_score_emergency(const llm_chat_t *chat, int mi, int ri,
                                 int n_evictable, void *userdata) {
  (void)userdata;
  int imp = (int)chat->msgs[mi].importance;
  int rec = (int)chat->msgs[mi].recoverability;
  int msg_len = (int)chat->msgs[mi].content_len;
  int pos_norm = (n_evictable > 1)
                   ? (ri * REACT_SCORE_POS_RANGE / (n_evictable - 1))
                   : 0;
  int size_bonus = 0;
  if (msg_len > REACT_SCORE_SIZE_THRESH) {
    size_bonus = (rec > 0) ? (msg_len / REACT_SCORE_SIZE_DIV) * rec
                           : msg_len / (REACT_SCORE_SIZE_DIV * 2);
    if (size_bonus > REACT_SCORE_SIZE_MAX) size_bonus = REACT_SCORE_SIZE_MAX;
  }
  return imp * REACT_SCORE_IMP_WEIGHT - rec * REACT_SCORE_REC_WEIGHT - size_bonus + pos_norm;
}

/* Emergency eviction — removes enough evictable messages to reach ~80% of
 * context budget, prioritizing recoverable content over irreplaceable.
 * Uses the same floor calculation as progressive eviction (pol.floor_pct).
 * Returns the number of messages evicted (0 if not enough to evict).
 * target_pct: target usage percentage (0 = use pol.emergency_target_pct).
 * Explicit target_pct prevents emergency eviction from leaving usage above
 * the trigger threshold. */
int react_emergency_evict(llm_chat_t *chat, long context_budget, int target_pct,
                          const config_t *cfg) {
  /* Use caller-supplied config to respect user-configured floor_pct. */
  eviction_policy_t pol = react_eviction_policy(cfg);
  int eff_target = (target_pct > 0) ? target_pct : pol.emergency_target_pct;
  int keep_head = react_compute_keep_head(chat);
  int keep_tail = react_compute_keep_tail(chat);
  int evict_start = keep_head;
  int evict_end = chat->n_msgs - keep_tail;

  /* Pair-safe boundary adjustment — never split tool_call/tool_result pairs. */
  evict_adjust_boundaries(chat, &evict_start, &evict_end);

  int n_evictable = evict_end - evict_start;
  if (n_evictable <= 2) return 0;

  long total_chars = react_calc_total_chars(chat);

  long target_chars;
  if (context_budget > 0)
    target_chars = context_budget * eff_target / 100;
  else
    target_chars = total_chars * eff_target / 100;
  if (total_chars <= target_chars) return 0;

  long head_chars = react_head_chars(chat, evict_start);
  long tail_chars = react_tail_chars(chat, evict_end);

  /* Floor based on evictable capacity only (excluding head+tail). */
  long floor_chars = react_calc_floor_chars_pol(chat, evict_start, evict_end,
                                                context_budget,
                                                head_chars, tail_chars, &pol);

  /* Build partner map once (O(n)) instead of per-candidate O(n²) scanning */
  evict_partner_map_t pmap = evict_build_partner_map(chat, evict_start, evict_end);

  int *evict_mark = xcalloc((size_t)n_evictable, sizeof(int));

  /* target_remaining = target_chars - head_chars = how much non-head content
     * we want to retain after eviction. */
  long remaining_nonhead = total_chars - head_chars;
  long target_remaining = target_chars - head_chars;
  if (target_remaining < floor_chars) target_remaining = floor_chars;

  int n_marked = evict_mark_candidates(chat, evict_start, evict_end,
                                       &pmap, floor_chars,
                                       remaining_nonhead, tail_chars,
                                       target_remaining,
                                       evict_score_emergency, NULL,
                                       evict_mark);

  /* Fallback — mark at least one message if nothing was marked.
     * Respects compaction floor and scores all eligible candidates to pick
     * the most evictable one (same scorer as main path). */
  if (n_marked == 0 && remaining_nonhead > floor_chars) {
    int best_i = -1, best_pair_ri = -1;
    int best_score = INT_MAX;
    for (int i = 0; i < n_evictable; i++) {
      int mi = evict_start + i;
      if (chat->msgs[mi].importance >= LLM_MSG_IMPORTANCE_HIGH) continue;
      long msg_chars = (long)chat->msgs[mi].content_len;
      long pair_chars = 0;
      int partner_mi = (mi < pmap.n_msgs) ? pmap.partner[mi] : -1;
      int pair_ri = -1;
      if (partner_mi >= evict_start && partner_mi < evict_end) {
        pair_ri = partner_mi - evict_start;
        if (pair_ri >= 0 && pair_ri < n_evictable)
          pair_chars = (long)chat->msgs[partner_mi].content_len;
        else
          pair_ri = -1;
      }
      /* Floor guard: ensure remaining content stays above floor */
      if (remaining_nonhead - tail_chars - msg_chars - pair_chars < floor_chars)
        continue;
      int score = evict_score_emergency(chat, mi, i, n_evictable, NULL);
      if (score < best_score) {
        best_score = score;
        best_i = i;
        best_pair_ri = pair_ri;
      }
    }
    if (best_i >= 0) {
      evict_mark[best_i] = 1;
      n_marked++;
      if (best_pair_ri >= 0 && !evict_mark[best_pair_ri]) {
        evict_mark[best_pair_ri] = 1;
        n_marked++;
      }
    }
  }

  evict_free_partner_map(&pmap);

  /* Shared sweep helper */
  int removed = evict_sweep_marked(chat, evict_start, evict_mark, n_evictable);
  free(evict_mark);
  return removed;
}

/* Combined emergency evict + breadcrumb/hint/scratchpad re-injection.
 * target_pct comes from config to avoid re-trigger loops. */
int react_emergency_evict_and_reinject(react_ctx_t *ctx, llm_chat_t *chat) {
  long cb = react_context_budget(ctx);
  int target_pct = react_eviction_target_pct(ctx->tools->cfg);
  int n_evict = react_emergency_evict(chat, cb, target_pct, ctx->tools->cfg);
  /* Budget-guarded breadcrumb + hint + scratchpad injection. */
  react_inject_emergency_breadcrumbs(ctx, chat, n_evict, cb, target_pct,
                                     /*skip_sp=*/0);
  return n_evict;
}

/* ── NULL Response Handling ────────────────────────────── */

/* Handle NULL response from LLM (HTTP 400/500/auth errors).
 * Returns: 0 = continue (retry), 1 = break (give up).
 * Modifies chat in-place for recovery. */
int react_handle_null_response(react_ctx_t *ctx, llm_chat_t *chat,
                               int *consecutive_null, int *total_400,
                               llm_stats_t *stats, int step,
                               react_event_fn on_event, void *userdata) {
  /* Extract provider error strings once at function entry. */
  const char *srv_err = ctx->provider ? ctx->provider->last_error : NULL;
  const char *srv_err_req = ctx->provider ? ctx->provider->last_error_request : NULL;
  const char *srv_err_resp = ctx->provider ? ctx->provider->last_error_response : NULL;

  /* Write server error to journal */
  {
    cJSON *err_params = cJSON_CreateObject();
    {

      if (*consecutive_null >= 3) {
        if (srv_err) {
          char emsg[512];
          snprintf(emsg, sizeof(emsg),
                   "LLM server error after recovery attempt — %s", srv_err);
          cJSON_AddStringToObject(err_params, "error", emsg);
        } else {
          cJSON_AddStringToObject(err_params, "error",
                                  "LLM server error after recovery attempt — giving up");
        }
      } else {
        if (srv_err) {
          char emsg[512];
          snprintf(emsg, sizeof(emsg),
                   "LLM server returned NULL response (%s)", srv_err);
          cJSON_AddStringToObject(err_params, "error", emsg);
        } else {
          cJSON_AddStringToObject(err_params, "error",
                                  "LLM server returned NULL response (unknown error)");
        }
      }
    }
    cJSON_AddNumberToObject(err_params, "attempt", *consecutive_null);
    cJSON_AddNumberToObject(err_params, "completion_tokens", stats->completion_tokens);

    long total_chars = react_calc_total_chars(chat);
    cJSON_AddNumberToObject(err_params, "context_chars", (double)total_chars);
    cJSON_AddNumberToObject(err_params, "context_msgs", chat->n_msgs);

    if (srv_err)
      cJSON_AddStringToObject(err_params, "server_message", srv_err);

    if (srv_err_req && ctx->tools->store) {
      char *req_ref = store_save(ctx->tools->store, srv_err_req);
      if (req_ref) {
        cJSON_AddStringToObject(err_params, "request_ref", req_ref);
        free(req_ref);
      }
    }
    if (srv_err_resp && ctx->tools->store) {
      char *resp_ref = store_save(ctx->tools->store, srv_err_resp);
      if (resp_ref) {
        cJSON_AddStringToObject(err_params, "response_ref", resp_ref);
        free(resp_ref);
      }
    }

    char *se_alias = NULL;
    {
      char *content = cJSON_Print(err_params);
      if (content && ctx->tools->store && ctx->tools->aliases) {
        char *hash = store_save(ctx->tools->store, content);
        if (hash) {
          se_alias = tool_register_alias(ctx->tools, hash);
          free(hash);
        }
      }
      free(content);
    }

    journal_append(ctx->tools->journal,
                   ctx->tools->react_loop, step + 1, "server_error",
                   err_params, se_alias, se_alias ? strlen(se_alias) : 0, 0,
                   "LLM server error", NULL, 0);
    free(se_alias);
    cJSON_Delete(err_params);
  }

  react_event_t ev = {0};
  ev.react_loop = ctx->tools->react_loop;
  ev.type = REACT_EVENT_ERROR;
  ev.step = step + 1;

  /* Authentication error (HTTP 401/403) — can't be fixed by eviction */
  if (srv_err && (strstr(srv_err, "HTTP 401") || strstr(srv_err, "HTTP 403"))) {
    ev.message = "Authentication failed — token expired or invalid, "
                 "please re-authenticate (e.g. gcloud auth login)";
    react_emit(on_event, userdata, &ev);
    return 1;
  }

  /* HTTP 400 (client error = bad request) — aggressive context eviction */
  if (srv_err && strstr(srv_err, "HTTP 400")) {
    (*total_400)++;
    if (*total_400 >= 6) {
      ev.message = "HTTP 400 — context still too large after "
                   "repeated eviction, giving up";
      react_emit(on_event, userdata, &ev);
      return 1;
    }
    int n_evict = react_emergency_evict_and_reinject(ctx, chat);
    if (n_evict > 0) {
      char emsg[128];
      snprintf(emsg, sizeof(emsg),
               "HTTP 400 — evicted %d messages to reduce context "
               "(attempt %d/6)",
               n_evict, *total_400);
      ev.message = emsg;
      react_emit(on_event, userdata, &ev);
    } else {
      ev.message = "HTTP 400 — no evictable messages remain, giving up";
      react_emit(on_event, userdata, &ev);
      return 1;
    }
    *consecutive_null = 0;
    return 0;
  }

  /* Max-token exhaustion — deterministic, not transient */
  if (stats->completion_tokens > 0 && ctx->provider &&
      stats->completion_tokens >= ctx->provider->cfg.max_tokens) {
    long cb = react_context_budget(ctx);
    int usage = react_chat_usage_pct(chat, cb);

    if (usage < 50) {
      /* Thinking-only exhaustion: the model spent all output tokens
             * on extended thinking without producing any text or tool call.
             * Context eviction won't help — the context isn't too large
             * (e.g. 5% of 1M).  Inject a hint and let the 5-tier retry
             * handle backoff.  Thinking length is non-deterministic, so
             * a plain retry usually succeeds. */
      char mtmsg[256];
      snprintf(mtmsg, sizeof(mtmsg),
               "Thinking-only max-token exhaustion (%d/%d tokens, "
               "context %d%%) — injecting hint and retrying",
               stats->completion_tokens, ctx->provider->cfg.max_tokens,
               usage);
      ev.message = mtmsg;
      react_emit(on_event, userdata, &ev);

      llm_chat_add(chat, "user",
                   "Your previous attempt used all output tokens on internal "
                   "reasoning without producing any visible response (no tool "
                   "call or text). You MUST produce a tool call or text "
                   "response. Think more concisely — focus on the immediate "
                   "next action rather than planning the entire solution.");
      if (chat->n_msgs > 0)
        chat->msgs[chat->n_msgs - 1].importance =
          LLM_MSG_IMPORTANCE_NORMAL;
      /* Fall through to 5-tier retry for backoff/escalation */
    } else {
      /* Context-overflow exhaustion — evict to make room */
      char mtmsg[256];
      snprintf(mtmsg, sizeof(mtmsg),
               "Max-token exhaustion (%d/%d tokens, context %d%%) — "
               "evicting context to recover",
               stats->completion_tokens, ctx->provider->cfg.max_tokens,
               usage);
      ev.message = mtmsg;
      react_emit(on_event, userdata, &ev);

      int n_evict = react_emergency_evict_and_reinject(ctx, chat);
      if (n_evict > 0) {
        (*consecutive_null)++;
        return 0;
      } else {
        ev.message = "Max-token exhaustion — no evictable messages "
                     "remain, giving up";
        react_emit(on_event, userdata, &ev);
        return 1;
      }
    }
  }

  /* 5-tier retry strategy for HTTP 500 / NULL responses */
  if (*consecutive_null >= 6) {
    ev.message = "LLM server error — all recovery tiers exhausted, giving up";
    react_emit(on_event, userdata, &ev);
    return 1;
  }

  if (*consecutive_null <= 2) {
    /* Tier 0: Plain retry with backoff */
    if (ctx->pause_requested) return 1;
    int backoff_ms = *consecutive_null * 2000;
    char rmsg[128];
    snprintf(rmsg, sizeof(rmsg),
             "LLM server error — plain retry %d/2 (backoff %dms)",
             *consecutive_null, backoff_ms);
    ev.message = rmsg;
    react_emit(on_event, userdata, &ev);
    for (int ms = 0; ms < backoff_ms && !ctx->pause_requested; ms += 100)
      usleep(100000);
  } else if (*consecutive_null == 3) {
    /* Tier 1: Remove the last assistant+tool_result pair.
         * Walk backward to find the actual last tool_result, skipping
         * injected hint/summary messages. Then find its partner tool_call. */
    ev.message = "LLM server error — removing last exchange and retrying (tier 1)";
    react_emit(on_event, userdata, &ev);
    int kh = react_compute_keep_head(chat);
    int tr_idx = -1; /* last tool_result index */
    for (int i = chat->n_msgs - 1; i >= kh; i--) {
      if (chat->msgs[i].tool_call_id) {
        tr_idx = i;
        break;
      }
    }
    if (tr_idx >= 0) {
      /* Find the partner tool_call for this result */
      int tc_idx = react_find_tool_partner(chat, tr_idx, kh, chat->n_msgs);
      int remove_from = (tc_idx >= kh) ? tc_idx : tr_idx;
      if (remove_from < chat->n_msgs)
        llm_chat_remove_range(chat, remove_from, chat->n_msgs);
    } else if (chat->n_msgs > kh) {
      /* No tool_result found — fall back to removing last message */
      llm_chat_remove_range(chat, chat->n_msgs - 1, chat->n_msgs);
    }
  } else if (*consecutive_null == 4) {
    /* Tier 2: Reformulate scratchpad (strip code blocks) */
    int sp_idx = llm_chat_find_by_type(chat, LLM_MSG_SCRATCHPAD);
    if (sp_idx >= 0) {
      ev.message = "LLM server error — stripping code blocks from scratchpad (tier 2)";
      react_emit(on_event, userdata, &ev);
      const char *src = chat->msgs[sp_idx].content;
      {
        const char *nl = strchr(src, '\n');
        if (nl) src = nl + 1;
      }
      size_t src_len = strlen(src);
      char *cleaned = xmalloc(src_len + 1);
      if (cleaned) {
        size_t di = 0;
        for (size_t si = 0; si < src_len;) {
          if (si + 3 <= src_len && strncmp(src + si, "```", 3) == 0) {
            const char *end = strstr(src + si + 3, "```");
            if (end) {
              si = (size_t)(end - src) + 3;
              if (si < src_len && src[si] == '\n') si++;
            } else {
              si += 3;
            }
            continue;
          }
          if (src[si] == '`') {
            const char *end = strchr(src + si + 1, '`');
            if (end && end - (src + si) < REACT_THOUGHT_TRUNC_LEN) {
              si++;
              while (src + si < end)
                cleaned[di++] = src[si++];
              si++;
              continue;
            }
          }
          cleaned[di++] = src[si++];
        }
        cleaned[di] = '\0';
        char *new_sp = react_format_scratchpad_msg(cleaned);
        if (new_sp)
          llm_chat_replace_content(chat, sp_idx, new_sp);
        free(cleaned);
      }
    } else {
      ev.message = "LLM server error — no scratchpad, skipping tier 2";
      react_emit(on_event, userdata, &ev);
    }
  } else if (*consecutive_null == 5) {
    /* Tier 3: Strip scratchpad entirely (nuclear option) */
    ev.message = "LLM server error — stripping scratchpad entirely (tier 3)";
    react_emit(on_event, userdata, &ev);
    llm_chat_remove_by_type(chat, LLM_MSG_SCRATCHPAD);
    /* Do NOT re-inject — this is a true strip, not a refresh. */
  }
  return 0;
}
