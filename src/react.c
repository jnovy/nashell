#include "react_internal.h"
#include "compress.h"

/* ── helpers ─────────────────────────────────────────── */

/* Get chars-per-token ratio from provider config, defaulting to 3.5.
 * Used for context budget calculations instead of hardcoded 4. */
float react_get_chars_per_token(const react_ctx_t *ctx) {
    if (ctx->provider && ctx->provider->cfg.chars_per_token > 0)
        return ctx->provider->cfg.chars_per_token;
    return 3.5f;
}

const char *react_json_get_str(cJSON *obj, const char *key) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item && cJSON_IsString(item)) return item->valuestring;
    return NULL;
}

/* Build the full system prompt string (base + model-specific rules).
 * Returns malloc'd string — caller must free.
 * Used for both chat injection and journal logging (Fix #12). */
char *react_build_system_prompt(const config_t *cfg) {
    char *base = tools_system_prompt();
    const char *extra = cfg ? cfg->system_prompt_extra : NULL;
    if (extra && extra[0]) {
        size_t len = strlen(base) + strlen(extra) + 64;
        char *full = malloc(len);
        if (full) {
            snprintf(full, len, "%s\n\n[MODEL-SPECIFIC RULES]\n%s", base, extra);
            free(base);
            return full;
        }
    }
    return base;
}

/* Add system prompt to chat, appending model-specific rules if configured.
 * Uses react_build_system_prompt() to avoid duplication (Fix #12). */
void react_add_system_prompt(llm_chat_t *chat, const config_t *cfg) {
    char *prompt = react_build_system_prompt(cfg);
    llm_chat_add_typed(chat, "system", prompt, LLM_MSG_SYSTEM);
    free(prompt);
}


/* Log memory context injection to the journal for debugging.
 * Captures: index summary (total + type counts), pinned keys, skills recalled.
 * Called at both the checkpoint-restore path and the main react_run path. */
void react_log_memory_context(tool_ctx_t *tools, int react_loop, int step,
                               const char *mem_index,
                               const char *pinned,
                               memory_results_t *all_memories,
                               const char *query)
{
    if (!tools->memory || !tools->journal) return;

    cJSON *params = cJSON_CreateObject();

    /* Index summary — extract from the mem_index header line
     * Format: "Memory: N entries, X lessons, Y strategies, Z skills, ..." */
    if (mem_index && strlen(mem_index) > 0) {
        cJSON_AddStringToObject(params, "index_summary", mem_index);
    } else {
        cJSON_AddStringToObject(params, "index_summary", "no entries");
    }

    /* Pinned memory keys — extract from pinned output
     * Format: "[PINNED: key1]\nvalue1\n\n[PINNED: key2]\nvalue2" */
    if (pinned && strlen(pinned) > 0) {
        cJSON *pinned_keys = cJSON_CreateArray();
        const char *p = pinned;
        while (*p) {
            /* Find "[PINNED: key]" pattern */
            if (strncmp(p, "[PINNED: ", 9) == 0) {
                const char *end = strchr(p + 9, ']');
                if (end) {
                    char key[256];
                    int klen = (int)(end - (p + 9));
                    if (klen >= (int)sizeof(key)) klen = (int)sizeof(key) - 1;
                    memcpy(key, p + 9, (size_t)klen);
                    key[klen] = '\0';
                    cJSON_AddItemToArray(pinned_keys, cJSON_CreateString(key));
                    p = end + 1;
                    /* Skip to next line after value */
                    while (*p && *p != '\n') p++;
                    if (*p == '\n') p++;
                    /* Skip blank line separator */
                    if (*p == '\n') p++;
                } else {
                    break;
                }
            } else {
                /* Skip to next line */
                while (*p && *p != '\n') p++;
                if (*p == '\n') p++;
            }
        }
        cJSON_AddItemToObject(params, "pinned_keys", pinned_keys);
    } else {
        cJSON_AddItemToObject(params, "pinned_keys", cJSON_CreateArray());
    }

    /* Per-type memory recall logging — skills, lessons, strategies, anti-patterns */
    static const char *type_prefixes[] = {
        "skill:", "lesson:", "strategy:", "anti-pattern:", NULL
    };
    static const char *json_keys[] = {
        "skills_matched", "lessons_matched", "strategies_matched", "antipatterns_matched"
    };
    for (int t = 0; type_prefixes[t] != NULL; t++) {
        cJSON *arr = cJSON_CreateArray();
        if (all_memories) {
            for (int i = 0; i < all_memories->count; i++) {
                if (all_memories->entries[i].key &&
                    strncmp(all_memories->entries[i].key, type_prefixes[t], strlen(type_prefixes[t])) == 0) {
                    cJSON *e = cJSON_CreateObject();
                    cJSON_AddStringToObject(e, "key", all_memories->entries[i].key);
                    cJSON_AddNumberToObject(e, "score", all_memories->entries[i].relevance);
                    cJSON_AddNumberToObject(e, "raw_rel", all_memories->entries[i].raw_relevance);
                    cJSON_AddNumberToObject(e, "hits", all_memories->entries[i].recall_hits);
                    cJSON_AddNumberToObject(e, "misses", all_memories->entries[i].recall_misses);
                    cJSON_AddItemToArray(arr, e);
                }
            }
        }
        cJSON_AddItemToObject(params, json_keys[t], arr);
    }

    /* Query that triggered the recall */
    if (query) {
        /* Truncate query to 200 chars for compactness */
        char qtrunc[201];
        strncpy(qtrunc, query, 200);
        qtrunc[200] = '\0';
        /* Strip newlines for single-line log */
        for (int i = 0; qtrunc[i]; i++) {
            if (qtrunc[i] == '\n' || qtrunc[i] == '\r') qtrunc[i] = ' ';
        }
        cJSON_AddStringToObject(params, "query", qtrunc);
    }

    /* Store full memory context in store/ for audit trail */
    char *mc_alias = NULL;
    {
        char *content = cJSON_Print(params);
        if (content && tools->store && tools->aliases) {
            char *hash = store_save(tools->store, content);
            if (hash) {
                mc_alias = tool_register_alias(tools, hash);
                free(hash);
            }
        }
        free(content);
    }
    journal_append(tools->journal, react_loop, step, "memory_context",
                   params, mc_alias, mc_alias ? strlen(mc_alias) : 0, 0, NULL, NULL);
    free(mc_alias);
}

/* Recursively unwrap nested JSON in the "thought" field.
 * Delegates to the shared unwrap_thought() in journal.c. */
void react_sanitize_thought(cJSON *action) {
    cJSON *th = cJSON_GetObjectItemCaseSensitive(action, "thought");
    if (!th || !cJSON_IsString(th) || !th->valuestring || th->valuestring[0] != '{')
        return;
    char *clean = unwrap_thought(th->valuestring);
    if (clean) {
        free(th->valuestring);
        th->valuestring = clean;
    } else {
        /* unwrap_thought returned NULL — the thought was a JSON object
         * with no extractable thought text (e.g. the model echoed the full
         * action JSON with thought="").  Clear it to empty. */
        free(th->valuestring);
        th->valuestring = strdup("");
    }
}

/* Checkpoint restore, save, remove are in react_checkpoint.c */


void react_stream_token_cb(const char *token, void *userdata) {
    react_stream_ctx_t *sctx = userdata;
    if (!sctx->on_event) return;
    react_event_t ev = {0};
    ev.type  = REACT_EVENT_LLM_TOKEN;
    ev.step  = sctx->step;
    ev.react_loop = sctx->react_loop;
    ev.token = token;
    sctx->on_event(&ev, sctx->userdata);
}

void react_emit(react_event_fn fn, void *ud, react_event_t *ev) {
    if (fn) fn(ev, ud);
}

/* Extract the key display parameter for a tool action */
const char *react_get_action_desc(cJSON *action, const char *action_name,
                                   const char *thought) {
    if (strcmp(action_name, "shell_exec") == 0)
        return react_json_get_str(action, "command");
    if (strcmp(action_name, "file_read") == 0 ||
        strcmp(action_name, "file_write") == 0 ||
        strcmp(action_name, "file_edit") == 0)
        return react_json_get_str(action, "path");
    if (strcmp(action_name, "grep_search") == 0)
        return react_json_get_str(action, "pattern");
    if (strcmp(action_name, "notes") == 0)
        return "[saving notes]";
    if (strcmp(action_name, "done") == 0)
        return thought;
    return thought;
}

/* Read the original user_query from a checkpoint without restoring full state.
 * Used by /continue to resume with the original query instead of "continue".
 * Returns heap-allocated string or NULL if no checkpoint. Caller frees. */
char *checkpoint_read_query(const char *session_dir) {
    if (!session_dir) return NULL;
    char path[NASH_PATH_MAX];
    snprintf(path, sizeof(path), "%s/checkpoint.json", session_dir);
    cJSON *cp = slurp_json(path);
    if (!cp) return NULL;
    cJSON *q = cJSON_GetObjectItem(cp, "user_query");
    char *result = NULL;
    if (q && q->valuestring && q->valuestring[0])
        result = strdup(q->valuestring);
    cJSON_Delete(cp);
    return result;
}

/* Extract usable text from LLM output that may be plain markdown OR a tool-call
 * JSON object (e.g. {"thought":"...","action":"notes","content":"..."}).
 * Works WITH the model's training instead of fighting it:
 *   - If output is JSON with a "content" field → extract and return that
 *   - If output is wrapped in ```markdown code fences → strip them
 *   - Otherwise → return as-is
 * Returns a new allocation (caller must free). Returns NULL on empty/NULL input. */
char *react_extract_llm_text_output(const char *raw) {
    if (!raw || !raw[0]) return NULL;

    /* Skip leading whitespace */
    raw = skip_whitespace(raw);
    if (!*raw) return NULL;

    /* Case 1: JSON tool call — extract "content" field */
    if (raw[0] == '{') {
        cJSON *j = cJSON_Parse(raw);
        if (j) {
            cJSON *c = cJSON_GetObjectItem(j, "content");
            if (c && cJSON_IsString(c) && c->valuestring && c->valuestring[0]) {
                char *result = strdup(c->valuestring);
                cJSON_Delete(j);
                return result;
            }
            /* If JSON has an "action" field, it's a tool call without content
             * (e.g. {"action":"notes","op":"list"} — model trying to call a tool
             * instead of outputting text). Reject it — return NULL so the caller
             * knows the LLM didn't produce usable text. */
            cJSON *action = cJSON_GetObjectItem(j, "action");
            if (action && cJSON_IsString(action)) {
                cJSON_Delete(j);
                return NULL;  /* tool call without content — reject */
            }
            cJSON_Delete(j);
        }
        /* JSON but not a tool call — fall through to return as-is */
    }

    /* Case 2: Code-fenced output — strip ``` wrapper */
    if (strncmp(raw, "```", 3) == 0) {
        const char *start = raw + 3;
        /* Skip optional language tag (e.g. ```markdown) */
        while (*start && *start != '\n') start++;
        if (*start == '\n') start++;
        /* Find closing ``` */
        const char *end = strstr(start, "\n```");
        if (end) {
            return strndup(start, end - start);
        }
        /* No closing fence — return everything after opening */
        return strdup(start);
    }

    /* Case 3: Plain text — return as-is */
    return strdup(raw);
}

/* ── Harness-1 helpers (arXiv 2606.02373) ─────────────── */

/* Auto-assign importance to a tool result based on tool name and success.
 * Harness-1 §3.2: different tools produce outputs of different value. */
static int react_tool_importance(const char *tool_name, int success) {
    if (!success) return LLM_MSG_IMPORTANCE_LOW;
    if (!tool_name) return LLM_MSG_IMPORTANCE_NORMAL;
    /* Search/analysis tools produce higher-value results */
    if (strcmp(tool_name, "grep_search") == 0 ||
        strcmp(tool_name, "web_search") == 0 ||
        strcmp(tool_name, "web_fetch") == 0 ||
        strcmp(tool_name, "memory_recall") == 0)
        return LLM_MSG_IMPORTANCE_HIGH;
    /* Done is critical — never evict the final result */
    if (strcmp(tool_name, "done") == 0)
        return LLM_MSG_IMPORTANCE_CRITICAL;
    /* Notes/plan results are important (scratchpad state) */
    if (strcmp(tool_name, "notes") == 0 || strcmp(tool_name, "plan") == 0)
        return LLM_MSG_IMPORTANCE_HIGH;
    return LLM_MSG_IMPORTANCE_NORMAL;
}

/* Find tool index in TOOL_REGISTRY by name (for diversity tracking).
 * Returns -1 if not found. */
static int react_tool_index(const char *name) {
    if (!name) return -1;
    for (int i = 0; i < TOOL_REGISTRY_COUNT && i < 32; i++) {
        if (TOOL_REGISTRY[i].name && strcmp(TOOL_REGISTRY[i].name, name) == 0)
            return i;
    }
    return -1;
}

/* ── main react loop ─────────────────────────────────── */

char *react_run(react_ctx_t *ctx, const char *user_query,
                react_event_fn on_event, void *userdata) {
    llm_chat_t *chat = llm_chat_new();

    /* Reset alias sequence counter so new aliases start at R<N>S0.
     * Do NOT clear the hash map — old aliases (R0S0, R0S1, etc.) must
     * remain resolvable for cross-loop file_read("R0S5") references. */
    ctx->tools->aliases->next_seq = 0;

    /* Check for checkpoint — resume interrupted task */
    int resume_step = 0;
    resume_step = react_checkpoint_restore(ctx, chat, user_query, on_event, userdata);
    int restored = (resume_step >= 0);


    if (!restored) {
        react_build_context(ctx, chat, user_query, on_event, userdata);
    } /* end if (!restored) — context building */
    /* Layer 2: full spec snapshot on change (content-addressed, deduplicated).
     * Emitted once at session start and again whenever the spec hash changes
     * (e.g., model switch, hot-reload, --load-spec).  The full resolved spec
     * TOML is saved in the content-addressed store — identical specs across
     * sessions or loops cost zero extra storage.
     * Runs unconditionally (outside !restored) so even checkpoint-resumed
     * sessions record the spec for the current process invocation. */
    if (ctx->tools->cfg) {
        char *spec_str = config_dump_spec_to_string(ctx->tools->cfg,
                                                    ctx->tools->cfg->matched_profile_file);
        if (spec_str) {
            char *spec_hash = store_save(ctx->tools->store, spec_str);
            int changed = 0;
            if (spec_hash) {
                if (!ctx->tools->last_spec_hash ||
                    strcmp(spec_hash, ctx->tools->last_spec_hash) != 0) {
                    changed = 1;
                    free(ctx->tools->last_spec_hash);
                    ctx->tools->last_spec_hash = strdup(spec_hash);
                }
            }
            if (changed && spec_hash) {
                char *spec_alias = tool_register_alias(ctx->tools, spec_hash);
                cJSON *sp = cJSON_CreateObject();
                if (ctx->provider && ctx->provider->cfg.model_id)
                    cJSON_AddStringToObject(sp, "model", ctx->provider->cfg.model_id);
                if (ctx->tools->cfg->provider.type)
                    cJSON_AddStringToObject(sp, "provider", ctx->tools->cfg->provider.type);
                if (ctx->tools->cfg->matched_profile_file)
                    cJSON_AddStringToObject(sp, "profile", ctx->tools->cfg->matched_profile_file);
                cJSON_AddStringToObject(sp, "spec_hash", spec_hash);
                journal_append(ctx->tools->journal, ctx->tools->react_loop, 0,
                               "spec", sp, spec_alias,
                               strlen(spec_str), count_lines(spec_str), NULL, NULL);
                cJSON_Delete(sp);
                free(spec_alias);
            }
            free(spec_hash);
            free(spec_str);
        }
    }

    char *final_result = NULL;
    struct timespec task_start;
    clock_gettime(CLOCK_MONOTONIC, &task_start);

    /* Action signature tracking for cycling detection.
     * Window size and threshold are configurable via config.toml:
     *   cycling_window    = number of recent actions to track (default 4)
     *   cycling_threshold = identical actions in window to trigger warning (default 2) */
    int cw = (ctx->tools->cfg && ctx->tools->cfg->cycling_window > 0)
             ? ctx->tools->cfg->cycling_window : 4;
    int ct = (ctx->tools->cfg && ctx->tools->cfg->cycling_threshold > 0)
             ? ctx->tools->cfg->cycling_threshold : 2;
    if (cw > 64) cw = 64;  /* sanity cap */
    /* FIX #10: Increased cycling signature buffer from 1024 to 2048 to reduce
     * false positives/negatives for long arguments (file_edit with >120-char
     * old_text, shell_exec with >1024-char commands). */
    #define CYCLING_SIG_SIZE 2048
    char (*last_sigs)[CYCLING_SIG_SIZE] = calloc(cw, CYCLING_SIG_SIZE);
    if (!last_sigs) { cw = 4; last_sigs = calloc(cw, CYCLING_SIG_SIZE); }
    int sig_count = 0;
    int consecutive_null_responses = 0;  /* Track LLM failures (HTTP 500 etc.) */
    int total_null_responses = 0;        /* Total NULL responses (never reset — catches alternating patterns) */
    int total_400_errors = 0;            /* Track HTTP 400 errors (never reset) */

    for (int step = resume_step; ctx->max_steps == 0 || step < ctx->max_steps; step++) {
        ctx->tools->step = step + 1;
        nash_log_set_context(ctx->tools->react_loop, step + 1);

        /* Emit step start */
        {
            react_event_t ev = {0};
            ev.react_loop = ctx->tools->react_loop;
            ev.type = REACT_EVENT_STEP_START;
            ev.step = step + 1;
            ev.max_steps = ctx->max_steps;
            ev.context_size = ctx->provider->cfg.context_size;
            react_emit(on_event, userdata, &ev);
        }

        /* Call LLM */
        struct timespec step_start;
        clock_gettime(CLOCK_MONOTONIC, &step_start);

        /* EDRM routing: decide thinking mode before LLM call.
         * On the first step of this run (step 0, or resume_step on checkpoint
         * restore), probe entropy dynamics to determine if CoT is beneficial.
         * Subsequent steps inherit the decision.
         * See [arXiv:2605.22873] for the theory. */
        if (ctx->tools->cfg && step == resume_step) {
            int mode = ctx->tools->cfg->thinking.mode;

            /* EDRM only works with local llama.cpp servers (needs logprobs).
             * For API providers (Vertex, Anthropic, OpenAI), skip EDRM and
             * default to thinking OFF. */
            int is_api_provider = ctx->provider &&
                ctx->provider->type != PROVIDER_LOCAL;
            if (is_api_provider && mode == THINKING_EDRM) {
                mode = THINKING_OFF;
            }

            if (mode == THINKING_ON) {
                ctx->provider->cfg.enable_thinking = 1;
            } else if (mode == THINKING_OFF) {
                ctx->provider->cfg.enable_thinking = 0;
            } else if (mode == THINKING_EDRM) {
                /* Build probe prompt from user query.
                 * #7: Use /apply-template for correct template, fallback to ChatML. */
                str_t probe = str_new(8192);
                char *templated = llm_apply_template(ctx->provider->cfg.api_base, user_query);
                if (templated) {
                    str_append_cstr(&probe, templated);
                    free(templated);
                } else {
                    str_appendf(&probe,
                        "<|im_start|>user\n%s<|im_end|>\n<|im_start|>assistant\n",
                        user_query);
                }

                thinking_config_t *tc = &ctx->tools->cfg->thinking;
                edrm_result_t edrm = llm_edrm_probe(
                    ctx->provider->cfg.api_base, probe.data,
                    tc->probe_tokens, tc->probe_n_probs,
                    tc->probe_temperature,
                    tc->tau_rho, tc->tau_vnr, tc->tau_h);
                str_free(&probe);

                ctx->provider->cfg.enable_thinking = edrm.route;

                /* Log the routing decision */
                {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                        "EDRM: H̄=%.2f ρ=%.2f VNR=%.2f → %s",
                        edrm.h_mean, edrm.rho_s, edrm.vnr,
                        edrm.route ? "thinking ON" : "thinking OFF");
                    react_event_t ev = {0};
                    ev.react_loop = ctx->tools->react_loop;
                    ev.type = REACT_EVENT_WARNING;
                    ev.step = step + 1;
                    ev.message = msg;
                    react_emit(on_event, userdata, &ev);
                }
            }
            /* Propagate thinking budget from config */
            ctx->provider->cfg.thinking_budget = ctx->tools->cfg->thinking.budget;
        }

        llm_stats_t stats = {0};
        react_stream_ctx_t sctx = { on_event, userdata, step + 1,
                                     ctx->tools->react_loop };
        int max_resp = ctx->tools->cfg ? ctx->tools->cfg->llm_max_response : 10*1024*1024;
        int rep_thresh = ctx->tools->cfg ? ctx->tools->cfg->llm_repeat_threshold : 100;
        /* Propagate tool filter so provider builds schema with only allowed tools */
        ctx->provider->tool_filter = &ctx->tools->tool_filter;
        char *response = provider_complete_stream(ctx->provider, chat, &stats,
                on_event ? react_stream_token_cb : NULL, &sctx,
                max_resp, rep_thresh);
        if (!response) {
            consecutive_null_responses++;
            total_null_responses++;

            /* Write server error to journal so it's visible in TUI and
             * preserved for post-mortem analysis. The journal entry uses
             * tool="server_error" with failed=true, which the TUI renders
             * with an "x" marker instead of "+". */
            {
                cJSON *err_params = cJSON_CreateObject();
                /* Build error message from actual server error if available */
                {
                    const char *srv_err = NULL;
                    if (ctx->provider && ctx->provider->last_error)
                        srv_err = ctx->provider->last_error;

                    if (consecutive_null_responses >= 2) {
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
                cJSON_AddNumberToObject(err_params, "attempt", consecutive_null_responses);
                cJSON_AddNumberToObject(err_params, "total_null_responses", total_null_responses);
                cJSON_AddNumberToObject(err_params, "completion_tokens", stats.completion_tokens);

                /* Capture context size for diagnostics */
                int total_chars = 0;
                for (int ci = 0; ci < chat->n_msgs; ci++)
                    if (chat->msgs[ci].content)
                        total_chars += (int)strlen(chat->msgs[ci].content);
                cJSON_AddNumberToObject(err_params, "context_chars", total_chars);
                cJSON_AddNumberToObject(err_params, "context_msgs", chat->n_msgs);

                /* Include the actual server error message if available. */
                const char *err_msg = ctx->provider ? ctx->provider->last_error : NULL;
                const char *err_req = ctx->provider ? ctx->provider->last_error_request : NULL;
                const char *err_resp = ctx->provider ? ctx->provider->last_error_response : NULL;

                if (err_msg) {
                    cJSON_AddStringToObject(err_params, "server_message", err_msg);
                }

                /* Save raw request and response bodies to store/ for
                 * post-mortem analysis. These contain the exact JSON that
                 * caused the server error, including the offset information
                 * the server reports in its error message. */
                if (err_req && ctx->tools->store) {
                    char *req_ref = store_save(ctx->tools->store, err_req);
                    if (req_ref) {
                        cJSON_AddStringToObject(err_params, "request_ref", req_ref);
                        free(req_ref);
                    }
                }
                if (err_resp && ctx->tools->store) {
                    char *resp_ref = store_save(ctx->tools->store, err_resp);
                    if (resp_ref) {
                        cJSON_AddStringToObject(err_params, "response_ref", resp_ref);
                        free(resp_ref);
                    }
                }

                /* Store full error params in store/ for audit trail */
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
                    "LLM server error", NULL);
                free(se_alias);
                cJSON_Delete(err_params);
            }

            react_event_t ev = {0};
            ev.react_loop = ctx->tools->react_loop;
            ev.type = REACT_EVENT_ERROR;
            ev.step = step + 1;

            /* Check if this is an authentication error (HTTP 401/403).
             * Auth errors can't be fixed by scratchpad manipulation —
             * the provider already retried once with a fresh token.
             * If it still fails, the credentials are truly invalid. */
            {
                const char *perr = ctx->provider ? ctx->provider->last_error : NULL;
                if (perr && (strstr(perr, "HTTP 401") || strstr(perr, "HTTP 403"))) {
                    ev.message = "Authentication failed — token expired or invalid, "
                                 "please re-authenticate (e.g. gcloud auth login)";
                    react_emit(on_event, userdata, &ev);
                    break;
                }
            }

            /* BUG FIX: Detect HTTP 400 (client error = bad request).
             * Unlike HTTP 500 (transient server error), 400 means the request
             * itself is malformed — typically context too large. The old tier 1
             * retry (remove 2 messages + continue) would let the model respond
             * successfully, resetting consecutive_null_responses to 0, then the
             * next LLM call would fail again with 400 → infinite loop.
             * For 400 errors: do aggressive context eviction immediately. */
            {
                const char *perr = ctx->provider ? ctx->provider->last_error : NULL;
                if (perr && strstr(perr, "HTTP 400")) {
                    total_400_errors++;
                    if (total_400_errors >= 6) {
                        ev.message = "HTTP 400 — context still too large after "
                                     "repeated eviction, giving up";
                        react_emit(on_event, userdata, &ev);
                        break;
                    }
                    /* Aggressive eviction: remove half of middle messages */
                    int keep_head = REACT_EVICT_KEEP_HEAD;
                    int keep_tail = REACT_EVICT_KEEP_TAIL;
                    int evict_start = keep_head;
                    int evict_end = chat->n_msgs - keep_tail;
                    if (evict_end > evict_start + 2) {
                        /* Evict the older half of the evictable range
                         * Fix #9: use llm_chat_remove_range instead of manual free/memmove */
                        int mid = evict_start + (evict_end - evict_start) / 2;
                        int n_evict = mid - evict_start;
                        llm_chat_remove_range(chat, evict_start, mid);
                        char emsg[128];
                        snprintf(emsg, sizeof(emsg),
                            "HTTP 400 — evicted %d messages to reduce context "
                            "(attempt %d/6)", n_evict, total_400_errors);
                        ev.message = emsg;
                        react_emit(on_event, userdata, &ev);
                    }
                    /* HTTP 400 is a client error (context too large), not a
                     * transient server error. Don't let it poison the HTTP 500
                     * retry tier counter — otherwise N recoverable 400s would
                     * exhaust the 500-recovery budget. */
                    consecutive_null_responses = 0;
                    continue;
                }
            }

            /* FIX: Detect max-token exhaustion as a distinct error class.
             * When completion_tokens == max_tokens, the model hit the output
             * ceiling — this is deterministic, not transient. Typically happens
             * after aggressive compaction leaves too little context, causing
             * the model to generate a massive response trying to reconstruct
             * everything. Recovery: aggressive context eviction (same as 400). */
            if (stats.completion_tokens > 0 && ctx->provider &&
                stats.completion_tokens >= ctx->provider->cfg.max_tokens) {
                char mtmsg[256];
                snprintf(mtmsg, sizeof(mtmsg),
                    "Max-token exhaustion (%d/%d tokens) — "
                    "evicting context to recover",
                    stats.completion_tokens, ctx->provider->cfg.max_tokens);
                ev.message = mtmsg;
                react_emit(on_event, userdata, &ev);

                /* Aggressive eviction like HTTP 400 handler */
                int keep_head = REACT_EVICT_KEEP_HEAD;
                int keep_tail = REACT_EVICT_KEEP_TAIL;
                int evict_start = keep_head;
                int evict_end = chat->n_msgs - keep_tail;
                if (evict_end > evict_start + 2) {
                    int mid = evict_start + (evict_end - evict_start) / 2;
                    llm_chat_remove_range(chat, evict_start, mid);
                }
                /* Don't count as consecutive (recovery may work) */
                consecutive_null_responses = 0;
                continue;
            }

            /* Death spiral circuit breaker: catch alternating success/failure
             * patterns where consecutive_null_responses resets on success but
             * the model keeps failing on the next attempt. This burned 86 min
             * and 311K tokens in session 1781416620.46346. */
            if (total_null_responses >= 8) {
                ev.message = "LLM server error — total NULL response limit reached "
                             "(possible death spiral), giving up";
                react_emit(on_event, userdata, &ev);
                break;
            }

            /* 3-tier retry strategy for HTTP 500 / NULL responses.
             * Each tier addresses a different root cause:
             *   Tier 1: Remove last assistant+tool_result pair (model confusion)
             *   Tier 2: Reformulate scratchpad (context pollution)
             *   Tier 3: Strip scratchpad entirely (nuclear option)
             *   Tier 4+: Give up */
            if (consecutive_null_responses >= 4) {
                ev.message = "LLM server error — all recovery tiers exhausted, giving up";
                react_emit(on_event, userdata, &ev);
                break;
            }

            if (consecutive_null_responses == 1) {
                /* Tier 1: Remove the last assistant+tool_result pair.
                 * The model's previous output was likely malformed (e.g.,
                 * "shell_execshell_exec"). Removing it gives the model a
                 * clean slate to regenerate from the previous context. */
                ev.message = "LLM server error — removing last exchange and retrying (tier 1)";
                react_emit(on_event, userdata, &ev);

                /* Remove last 2 messages (assistant + tool_result) if they exist.
                 * Fix #9: use llm_chat_remove_range instead of manual free. */
                if (chat->n_msgs >= 2) {
                    int remove_from = chat->n_msgs - 2;
                    if (remove_from < 3) remove_from = 3;
                    if (remove_from < chat->n_msgs)
                        llm_chat_remove_range(chat, remove_from, chat->n_msgs);
                }
            } else if (consecutive_null_responses == 2) {
                /* Tier 2: Reformulate scratchpad into plain prose.
                 * Code blocks and JSON in the scratchpad can confuse
                 * the model's JSON generation.
                 * Fix #3: Use msg_type instead of content-prefix scanning. */
                int sp_idx = llm_chat_find_by_type(chat, LLM_MSG_SCRATCHPAD);

                if (sp_idx >= 0) {
                    ev.message = "LLM server error — stripping code blocks from scratchpad (tier 2)";
                    react_emit(on_event, userdata, &ev);

                    /* P6: Local code-block stripping instead of LLM call.
                     * The LLM server may be overloaded (common cause of 500s),
                     * so making another LLM call during recovery adds load.
                     * Strip ```...``` code blocks and inline `code` locally. */
                    const char *src = chat->msgs[sp_idx].content + (sizeof("[SCRATCHPAD]\n") - 1);
                    size_t src_len = strlen(src);
                    char *cleaned = malloc(src_len + 1);
                    if (cleaned) {
                        size_t di = 0;
                        for (size_t si = 0; si < src_len; ) {
                            /* Strip fenced code blocks: ```...``` */
                            if (si + 3 <= src_len && strncmp(src + si, "```", 3) == 0) {
                                /* Skip to closing ``` */
                                const char *end = strstr(src + si + 3, "```");
                                if (end) {
                                    si = (size_t)(end - src) + 3;
                                    /* Skip trailing newline */
                                    if (si < src_len && src[si] == '\n') si++;
                                } else {
                                    si += 3; /* no closing fence — skip opening */
                                }
                                continue;
                            }
                            /* Strip inline backtick code: `...` */
                            if (src[si] == '`') {
                                const char *end = strchr(src + si + 1, '`');
                                if (end && end - (src + si) < 200) {
                                    /* Copy content without backticks */
                                    si++;
                                    while (src + si < end) {
                                        cleaned[di++] = src[si++];
                                    }
                                    si++; /* skip closing backtick */
                                    continue;
                                }
                            }
                            cleaned[di++] = src[si++];
                        }
                        cleaned[di] = '\0';

                        size_t clen = strlen(cleaned);
                        char *new_sp = malloc(clen + 32);
                        if (new_sp) {
                            snprintf(new_sp, clen + 32, "[SCRATCHPAD]\n%s", cleaned);
                            free(chat->msgs[sp_idx].content);
                            chat->msgs[sp_idx].content = new_sp;
                        }
                        free(cleaned);
                    }
                } else {
                    ev.message = "LLM server error — no scratchpad, skipping tier 2";
                    react_emit(on_event, userdata, &ev);
                }
            } else if (consecutive_null_responses == 3) {
                /* Tier 3: Strip scratchpad entirely (nuclear option).
                 * If reformulation didn't help, the scratchpad itself
                 * may be the problem. Remove it completely.
                 * Fix #3: Use msg_type instead of content-prefix scanning. */
                ev.message = "LLM server error — stripping scratchpad entirely (tier 3)";
                react_emit(on_event, userdata, &ev);
                llm_chat_remove_by_type(chat, LLM_MSG_SCRATCHPAD);
            }
            continue;
        }
        consecutive_null_responses = 0;  /* Reset on successful LLM response */
        /* P4: Decay HTTP 400 error counter on success. Without this, 5
         * unrelated 400s across a long session would exhaust the budget
         * and the 6th would give up unconditionally. Decay instead of
         * hard reset retains some caution about recurring issues. */
        if (total_400_errors > 0) total_400_errors--;

        struct timespec step_end;
        clock_gettime(CLOCK_MONOTONIC, &step_end);
        double step_elapsed = (step_end.tv_sec - step_start.tv_sec) +
                              (step_end.tv_nsec - step_start.tv_nsec) / 1e9;

        /* Parse JSON response */
        cJSON *action = llm_parse_action(response);

        if (!action) {
            react_event_t ev = {0};
            ev.react_loop = ctx->tools->react_loop;
            ev.type = REACT_EVENT_ERROR;
            ev.step = step + 1;
            ev.message = "Failed to parse LLM response as JSON";
            react_emit(on_event, userdata, &ev);

            /* Log the invalid response to journal for analysis */
            char *err_hash = store_save(ctx->tools->store, response);
            char *err_alias = tool_register_alias(ctx->tools,
                                        err_hash ? err_hash : "");
            cJSON *err_p = cJSON_CreateObject();
            cJSON_AddStringToObject(err_p, "type", "parse_error");
            cJSON_AddStringToObject(err_p, "raw_preview",
                strlen(response) > 200 ? "(truncated)" : response);
            journal_append(ctx->tools->journal, ctx->tools->react_loop,
                           step + 1, "parse_error", err_p, err_alias,
                           strlen(response), 0,
                           "LLM response was not valid JSON", NULL);
            cJSON_Delete(err_p);
            free(err_alias);
            free(err_hash);

            /* Retry — tell model to use tool_calls */
            llm_chat_add(chat, "assistant", response);
            llm_chat_add(chat, "user",
                "Your response could not be parsed. "
                "You must call one of the available tools. "
                "Do not write free-form text.");
            free(response);
            continue;
        }

        /* Sanitize thought field: unwrap nested JSON if the LLM echoed
         * back a full action object as its content/thought. */
        react_sanitize_thought(action);

        const char *thought = react_json_get_str(action, "thought");
        const char *action_name = react_json_get_str(action, "action");

        if (!action_name) {
            if (thought && thought[0]) {
                /* Thought-only response — the model is reasoning without acting.
                 * This is valid (e.g., deep code analysis, planning).
                 * Log as "thinking" step (not an error), preserve in context. */
                char *think_hash = store_save(ctx->tools->store, response);
                char *think_alias = tool_register_alias(ctx->tools,
                                            think_hash ? think_hash : "");
                cJSON *think_p = cJSON_CreateObject();
                cJSON_AddStringToObject(think_p, "type", "thinking");
                cJSON_AddStringToObject(think_p, "thought", thought);
                journal_append(ctx->tools->journal, ctx->tools->react_loop,
                               step + 1, "thinking", think_p, think_alias,
                               strlen(thought), 0, NULL, NULL);
                cJSON_Delete(think_p);
                free(think_alias);
                free(think_hash);

                /* Emit as a step complete (not error) so TUI shows it */
                {
                    react_event_t ev = {0};
                    ev.react_loop = ctx->tools->react_loop;
                    ev.type = REACT_EVENT_STEP_COMPLETE;
                    ev.step = step + 1;
                    ev.max_steps = ctx->max_steps;
                    ev.step_elapsed = step_elapsed;
                    ev.action = "thinking";
                    ev.description = thought;
                    ev.stats = stats;
                    ev.context_size = ctx->provider->cfg.context_size;
                    react_emit(on_event, userdata, &ev);
                }

                /* Add thought to conversation as assistant message,
                 * followed by a brief user nudge to prevent consecutive
                 * assistant messages (which llama.cpp rejects with 400). */
                llm_chat_add(chat, "assistant", response);
                llm_chat_add(chat, "user",
                    "Good thinking. Now call a tool to act on it.");
            } else {
                /* No thought AND no action — genuine parse error */
                react_event_t ev = {0};
                ev.react_loop = ctx->tools->react_loop;
                ev.type = REACT_EVENT_ERROR;
                ev.step = step + 1;
                ev.message = "No 'action' field in response";
                react_emit(on_event, userdata, &ev);

                char *err_hash = store_save(ctx->tools->store, response);
                char *err_alias = tool_register_alias(ctx->tools,
                                            err_hash ? err_hash : "");
                cJSON *err_p = cJSON_CreateObject();
                cJSON_AddStringToObject(err_p, "type", "missing_action");
                cJSON_AddStringToObject(err_p, "raw_preview",
                    strlen(response) > 200 ? "(truncated)" : response);
                journal_append(ctx->tools->journal, ctx->tools->react_loop,
                               step + 1, "parse_error", err_p, err_alias,
                               strlen(response), 0,
                               "LLM response missing 'action' field", NULL);
                cJSON_Delete(err_p);
                free(err_alias);
                free(err_hash);

                /* Retry — tell model to use tool_calls */
                llm_chat_add(chat, "assistant", response);
                llm_chat_add(chat, "user",
                    "Your response could not be parsed. "
                    "You must call one of the available tools. "
                    "Do not write free-form text.");
            }
            cJSON_Delete(action);
            free(response);
            continue;
        }

        const char *desc = react_get_action_desc(action, action_name, thought);

        /* Check for user_ask — pause react loop and wait for user input */
        if (strcmp(action_name, "user_ask") == 0) {
            const char *question = react_json_get_str(action, "question");
            if (!question || !question[0]) {
                /* Model called user_ask without a question — nudge it to retry */
                const char *errmsg =
                    "ERROR: user_ask requires a 'question' parameter with a "
                    "non-empty string. Re-call user_ask with a specific question, "
                    "or use a different tool if no question is needed.";
                if (chat->last_tool_call_id) {
                    llm_chat_add_assistant_tool_call(chat, response,
                        chat->last_tool_calls_json);
                    llm_chat_add_tool_result(chat, chat->last_tool_call_id,
                                              errmsg);
                } else {
                    llm_chat_add(chat, "assistant", response);
                    llm_chat_add(chat, "user", errmsg);
                }
                cJSON_Delete(action);
                free(response);
                continue;
            }

            /* Store question in shared state for TUI to read */
            free(ctx->user_ask_question);
            ctx->user_ask_question = strdup(question);
            free(ctx->user_ask_answer);
            ctx->user_ask_answer = NULL;
            ctx->user_ask_pending = 1;

            /* Emit event so TUI shows the question */
            react_event_t ev = {0};
            ev.react_loop = ctx->tools->react_loop;
            ev.type = REACT_EVENT_USER_ASK;
            ev.step = step + 1;
            ev.message = question;
            react_emit(on_event, userdata, &ev);

            /* P7: Wait on condition variable instead of polling.
             * The TUI thread signals user_ask_cond after setting the answer. */
            pthread_mutex_lock(&ctx->user_ask_mutex);
            while (ctx->user_ask_pending) {
                pthread_cond_wait(&ctx->user_ask_cond, &ctx->user_ask_mutex);
            }
            pthread_mutex_unlock(&ctx->user_ask_mutex);

            /* Build tool result from user's answer */
            const char *answer = ctx->user_ask_answer;
            if (!answer) answer = "(no answer)";

            /* Log to journal — store as markdown so TUI renders it nicely */
            cJSON *ua_params = cJSON_CreateObject();
            cJSON_AddStringToObject(ua_params, "question", question);
            cJSON_AddStringToObject(ua_params, "answer", answer);
            size_t ua_md_len = strlen(question) + strlen(answer) + 64;
            char *ua_md = malloc(ua_md_len);
            snprintf(ua_md, ua_md_len,
                     "## Question\n\n%s\n\n## Answer\n\n%s\n",
                     question, answer);
            char *ua_hash = store_save(ctx->tools->store, ua_md);
            free(ua_md);
            char *ua_alias = ua_hash ? tool_register_alias(ctx->tools, ua_hash) : NULL;
            journal_append(ctx->tools->journal, ctx->tools->react_loop,
                           step + 1, "user_ask", ua_params, ua_alias,
                           strlen(answer), 0, NULL, NULL);
            free(ua_hash);

            /* Build result message for the model (JSON-escape the answer) */
            cJSON *ans_obj = cJSON_CreateObject();
            cJSON_AddStringToObject(ans_obj, "answer", answer);
            char *ans_json = cJSON_PrintUnformatted(ans_obj);
            cJSON_Delete(ans_obj);
            size_t ans_len = (ans_json ? strlen(ans_json) : 2) + 32;
            char *result_msg = malloc(ans_len);
            snprintf(result_msg, ans_len, "%s\n[step %d | user_ask]",
                     ans_json ? ans_json : "{}", step + 1);
            free(ans_json);

            /* Add to chat as tool result */
            if (chat->last_tool_call_id) {
                llm_chat_add_assistant_tool_call(chat, response,
                    chat->last_tool_calls_json);
                llm_chat_add_tool_result(chat, chat->last_tool_call_id, result_msg);
            } else {
                llm_chat_add(chat, "assistant", response);
                llm_chat_add(chat, "user", result_msg);
            }

            free(result_msg);
            free(ua_alias);
            cJSON_Delete(ua_params);
            cJSON_Delete(action);
            free(response);
            continue;
        }

        /* Check for done */
        if (strcmp(action_name, "done") == 0) {
            const char *result = react_json_get_str(action, "result");
            /* Fallback: if result is empty but thought has content, use thought.
             * Local models sometimes put the summary in "thought" and leave
             * "result" empty — the thought IS the answer for done calls. */
            if ((!result || !result[0]) && thought && thought[0]) {
                result = thought;
            }
            final_result = result ? strdup(result) : strdup("(no result)");
            react_checkpoint_remove(ctx);

            /* Auto-save done result for cross-loop inheritance.
             * Two mechanisms:
             * 1. Scratchpad section (R<N>_result) — may be pruned by post-reflection
             * 2. session_dir/result.txt — survives pruning, loaded as [PREVIOUS RESULT]
             *    in the next react loop. Provides reliable fallback for user follow-ups. */
            {
                char sec_name[32];
                snprintf(sec_name, sizeof(sec_name), "R%d_result",
                         ctx->tools->react_loop);
                scratchpad_write(&ctx->tools->scratch, sec_name,
                                 final_result, 1);  /* priority 1 = high */
                scratchpad_save(&ctx->tools->scratch, ctx->tools->session_dir);
            }
            if (ctx->tools->session_dir) {
                char rpath[NASH_PATH_MAX];
                snprintf(rpath, sizeof(rpath), "%s/result.txt",
                         ctx->tools->session_dir);
                write_file(rpath, final_result, strlen(final_result));
            }

            /* Store result for full audit trail (journal + store/) */
            ctx->tools->thought = thought;
            tool_result_t tr = tool_execute(ctx->tools, action_name, action);
            ctx->tools->thought = NULL;
            tool_result_free(&tr);

            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double total = (now.tv_sec - task_start.tv_sec) +
                           (now.tv_nsec - task_start.tv_nsec) / 1e9;

            react_event_t ev = {0};
            ev.react_loop = ctx->tools->react_loop;
            ev.type = REACT_EVENT_DONE;
            ev.step = step + 1;
            ev.step_elapsed = step_elapsed;
            ev.total_elapsed = total;
            ev.action = action_name;
            ev.description = desc;
            ev.result = final_result;
            ev.stats = stats;
            ev.context_size = ctx->provider->cfg.context_size;
            react_emit(on_event, userdata, &ev);

            cJSON_Delete(action);
            free(response);
            break;
        }

        /* Cycling detection — disabled by default, enable via config:
         *   [limits]
         *   cycling_detection = true
         * When disabled, the model can repeat the same action without
         * warnings or refusal. This is useful for tasks that legitimately
         * require repeated operations (e.g., reading multiple sections
         * of the same file, running similar commands). */
        int cycling_enabled = ctx->tools->cfg ? ctx->tools->cfg->cycling_detection : 0;
        char sig[CYCLING_SIG_SIZE];
        const char *cmd = react_json_get_str(action, "command");
        const char *path = react_json_get_str(action, "path");
        const char *pattern = react_json_get_str(action, "pattern");
        const char *content = react_json_get_str(action, "content");
        const char *old_text = react_json_get_str(action, "old_text");
        const char *new_text = react_json_get_str(action, "new_text");
        const char *query = react_json_get_str(action, "query");
        const char *question = react_json_get_str(action, "question");
        const char *url = react_json_get_str(action, "url");
        /* FIX BUG#11: Include key and value in signature so memory_store
         * calls with different keys aren't falsely detected as cycling. */
        const char *key = react_json_get_str(action, "key");
        const char *value = react_json_get_str(action, "value");
        /* Include start_line/end_line in signature so that reading different
         * line ranges of the same file is NOT detected as cycling.
         * file_read("react.c", 1, 50) and file_read("react.c", 50, 100)
         * are different actions, not repetitions. */
        cJSON *sl = cJSON_GetObjectItem(action, "start_line");
        cJSON *el = cJSON_GetObjectItem(action, "end_line");
        int start_line = sl ? (int)cJSON_GetNumberValue(sl) : 0;
        int end_line = el ? (int)cJSON_GetNumberValue(el) : 0;
        /* Build signature from all action-distinguishing parameters.
         * Truncate long fields (content, old_text, new_text) to keep sig bounded.
         * Use 512-char prefix — 120 was too short and caused false cycling
         * detection for file_edit calls that differ only after char 120
         * (common with large code blocks). The sig buffer is 2048 bytes,
         * so 5×512 + other fields still fits comfortably. */
        char content_prefix[520] = "", old_prefix[520] = "", new_prefix[520] = "";
        char key_prefix[520] = "", value_prefix[520] = "";
        if (content) snprintf(content_prefix, sizeof(content_prefix), "%.512s", content);
        if (old_text) snprintf(old_prefix, sizeof(old_prefix), "%.512s", old_text);
        if (new_text) snprintf(new_prefix, sizeof(new_prefix), "%.512s", new_text);
        if (key) snprintf(key_prefix, sizeof(key_prefix), "%.512s", key);
        if (value) snprintf(value_prefix, sizeof(value_prefix), "%.512s", value);
        snprintf(sig, sizeof(sig), "%s:%s:%s:%s:%d:%d:%s:%s:%s:%s:%s:%s:%s:%s",
                 action_name,
                 cmd ? cmd : "",
                 path ? path : "",
                 pattern ? pattern : "",
                 start_line, end_line,
                 content_prefix,
                 old_prefix,
                 new_prefix,
                 query ? query : "",
                 question ? question : "",
                 url ? url : "",
                 key_prefix,
                 value_prefix);

        int repeated = 0;
        for (int i = 0; i < sig_count && i < cw; i++) {
            if (strcmp(last_sigs[i], sig) == 0) repeated++;
        }
        if (sig_count < cw) {
            snprintf(last_sigs[sig_count], CYCLING_SIG_SIZE, "%s", sig);
            sig_count++;
        } else {
            memmove(last_sigs, last_sigs + 1, (size_t)(cw - 1) * CYCLING_SIG_SIZE);
            snprintf(last_sigs[cw - 1], CYCLING_SIG_SIZE, "%s", sig);
        }

        if (cycling_enabled && repeated >= ct) {
            char warn_msg[256];
            snprintf(warn_msg, sizeof(warn_msg),
                     "Cycling detected — same action repeated %d times", repeated + 1);
            react_event_t ev = {0};
            ev.react_loop = ctx->tools->react_loop;
            ev.type = REACT_EVENT_WARNING;
            ev.step = step + 1;
            ev.message = warn_msg;
            react_emit(on_event, userdata, &ev);

            /* Fix 1: Inject warning into chat so the model KNOWS it's cycling */
            llm_chat_add(chat, "user",
                "WARNING: You are repeating the same action. "
                "The output is already stored — use file_read(ref) to read it. "
                "Do NOT re-run the same command.");
        }

        /* Refuse execution after threshold+1 consecutive identical actions */
        tool_result_t tr;
        if (cycling_enabled && repeated >= ct + 1) {
            cJSON *err_meta = cJSON_CreateObject();
            cJSON_AddStringToObject(err_meta, "error",
                "Refused: same action repeated 4+ times. "
                "Read previous results with file_read(ref) instead.");
            tr = (tool_result_t){ .meta = err_meta, .store_ref = NULL, .success = 0 };

            /* Store cycling_refused details for audit trail */
            {
                cJSON *cr_params = cJSON_CreateObject();
                cJSON_AddStringToObject(cr_params, "error",
                    "same action repeated 4+ times");
                cJSON_AddStringToObject(cr_params, "action", action_name);
                char *cr_json = cJSON_PrintUnformatted(cr_params);
                char *cr_ref = (cr_json && ctx->tools->store)
                    ? store_save(ctx->tools->store, cr_json) : NULL;
                char *cr_alias = (cr_ref && ctx->tools->aliases)
                    ? tool_register_alias(ctx->tools, cr_ref) : NULL;
                journal_append(ctx->tools->journal, ctx->tools->react_loop,
                               step + 1, "cycling_refused", cr_params, cr_alias,
                               cr_json ? strlen(cr_json) : 0, 0,
                               "same action repeated 4+ times", NULL);
                free(cr_json);
                free(cr_ref);
                free(cr_alias);
                cJSON_Delete(cr_params);
            }
        } else {
            /* Normal execution — inject thought into tool_ctx for journal recording */
            ctx->tools->thought = thought;
            tr = tool_execute(ctx->tools, action_name, action);
            ctx->tools->thought = NULL;

            /* Unknown tool recovery: if the model generated a garbled tool name
             * (e.g., "shell_execshell_exec"), don't send the raw error back —
             * instead inject a corrective message and let the model retry.
             * This prevents the "garbled name → error → model confusion → 500" cascade. */
            if (!tr.success && tr.meta) {
                cJSON *err_j = cJSON_GetObjectItem(tr.meta, "error");
                if (err_j && err_j->valuestring &&
                    strstr(err_j->valuestring, "unknown tool")) {
                    /* Store garbled tool details in the store for debugging.
                     * This lets us investigate patterns: why did the model produce
                     * a garbled name? Which model? What context triggered it? */
                    cJSON *ut_data = cJSON_CreateObject();
                    cJSON_AddStringToObject(ut_data, "garbled_tool", action_name);
                    char *action_str = cJSON_PrintUnformatted(action);
                    cJSON_AddStringToObject(ut_data, "params", action_str ? action_str : "");
                    char *ut_json = cJSON_PrintUnformatted(ut_data);
                    char *ut_ref = (ut_json && ctx->tools->store)
                        ? store_save(ctx->tools->store, ut_json) : NULL;
                    char *ut_alias = (ut_ref && ctx->tools->aliases)
                        ? tool_register_alias(ctx->tools, ut_ref) : NULL;

                    /* Log the error for audit trail */
                    journal_append(ctx->tools->journal, ctx->tools->react_loop,
                                   step + 1, "unknown_tool", tr.meta, ut_alias,
                                   ut_json ? strlen(ut_json) : 0, 0,
                                   err_j->valuestring, NULL);

                    /* Inject corrective message into chat (with store ref for debugging) */
                    char *tool_names = tool_registry_names_csv();
                    char correction[1024];
                    if (ut_alias) {
                        snprintf(correction, sizeof(correction),
                            "ERROR: '%s' is not a valid tool. "
                            "Stored garbled call for debugging: file_read(\"%s\"). "
                            "Available tools: %s. "
                            "Please retry with the correct tool name.",
                            action_name, ut_alias,
                            tool_names ? tool_names : "(unknown)");
                    } else {
                        snprintf(correction, sizeof(correction),
                            "ERROR: '%s' is not a valid tool. "
                            "Available tools: %s. "
                            "Please retry with the correct tool name.",
                            action_name,
                            tool_names ? tool_names : "(unknown)");
                    }
                    free(tool_names);

                    if (chat->last_tool_call_id) {
                        /* Tool calls API: send error as tool result */
                        llm_chat_add_tool_result(chat, chat->last_tool_call_id,
                                                  correction);
                    } else {
                        llm_chat_add(chat, "user", correction);
                    }

                    tool_result_free(&tr);
                    cJSON_Delete(action);
                    free(response);
                    free(action_str);
                    free(ut_json);
                    free(ut_ref);
                    free(ut_alias);
                    cJSON_Delete(ut_data);
                    continue;  /* retry — model gets another chance */
                }
            }
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double total_elapsed = (now.tv_sec - task_start.tv_sec) +
                               (now.tv_nsec - task_start.tv_nsec) / 1e9;

        /* Emit step complete */
        {
            react_event_t ev = {0};
            ev.react_loop = ctx->tools->react_loop;
            ev.type = REACT_EVENT_STEP_COMPLETE;
            ev.step = step + 1;
            ev.max_steps = ctx->max_steps;
            ev.step_elapsed = step_elapsed;
            ev.total_elapsed = total_elapsed;
            ev.action = action_name;
            ev.description = desc ? desc : "";
            ev.stats = stats;
            ev.context_size = ctx->provider->cfg.context_size;
            react_emit(on_event, userdata, &ev);
        }

        /* Emit tool output */
        {
            react_event_t ev = {0};
            ev.react_loop = ctx->tools->react_loop;
            ev.type = REACT_EVENT_TOOL_OUTPUT;
            ev.step = step + 1;
            ev.tool_meta = tr.meta;
            ev.store_ref = tr.store_ref;
            react_emit(on_event, userdata, &ev);
        }

        /* Build tool result string for context */
        char *meta_str = cJSON_PrintUnformatted(tr.meta);
        size_t result_len = strlen(meta_str) + 128;
        char *result_msg = malloc(result_len);
        { char _dur[32]; fmt_duration(total_elapsed, _dur, sizeof(_dur));
        snprintf(result_msg, result_len, "%s\n[step %d | %s]",
                 meta_str, step + 1, _dur); }

        /* Harness-1 §3.2: Assign importance to tool result messages */
        int tool_imp = react_tool_importance(action_name, tr.success);

        /* Harness-1 §3.3: Context-level deduplication — detect and skip
         * near-duplicate tool results to avoid wasting context budget.
         * Uses CRC32 hash of the result content. */
        int is_dedup = 0;
        if (result_msg && result_msg[0] && tr.success) {
            uint32_t content_hash = compress_crc32(meta_str, strlen(meta_str));
            if (compress_is_duplicate(content_hash,
                    ctx->tools->dedup_hashes, ctx->tools->dedup_count)) {
                is_dedup = 1;
                /* Replace with a short reference */
                int dedup_step = -1;
                for (int di = 0; di < ctx->tools->dedup_count; di++) {
                    if (ctx->tools->dedup_hashes[di] == content_hash) {
                        dedup_step = ctx->tools->dedup_steps[di];
                        break;
                    }
                }
                free(result_msg);
                result_len = 128;
                result_msg = malloc(result_len);
                if (dedup_step >= 0)
                    snprintf(result_msg, result_len,
                        "{\"note\":\"Same content as step %d — see earlier result\"}\n[step %d | dedup]",
                        dedup_step, step + 1);
                else
                    snprintf(result_msg, result_len,
                        "{\"note\":\"Duplicate content — see earlier result\"}\n[step %d | dedup]",
                        step + 1);
                tool_imp = LLM_MSG_IMPORTANCE_LOW;  /* deduped results are low priority */
            } else {
                /* Record hash for future dedup checks */
                int idx = ctx->tools->dedup_count < 64
                    ? ctx->tools->dedup_count++ : (ctx->tools->dedup_count - 1);
                if (idx >= 63) {
                    /* Shift rolling buffer */
                    memmove(ctx->tools->dedup_hashes, ctx->tools->dedup_hashes + 1,
                            63 * sizeof(uint32_t));
                    memmove(ctx->tools->dedup_steps, ctx->tools->dedup_steps + 1,
                            63 * sizeof(int));
                    idx = 63;
                }
                ctx->tools->dedup_hashes[idx] = content_hash;
                ctx->tools->dedup_steps[idx] = step + 1;
            }
        }

        /* Add assistant + tool result to chat (with tool_calls threading if available) */
        if (chat->last_tool_call_id) {
            /* Tool calls API: add assistant with tool_calls, then tool result */
            llm_chat_add_assistant_tool_call(chat, response,
                                              chat->last_tool_calls_json);
            llm_chat_add_tool_result(chat, chat->last_tool_call_id, result_msg);
        } else {
            /* Fallback: legacy JSON-in-content format */
            llm_chat_add(chat, "assistant", response);
            llm_chat_add(chat, "user", result_msg);
        }

        /* Harness-1 §3.2: Tag the newly added messages with importance.
         * The assistant message gets NORMAL, the tool result gets tool-specific importance. */
        if (chat->n_msgs >= 2) {
            chat->msgs[chat->n_msgs - 2].importance = LLM_MSG_IMPORTANCE_NORMAL;
            chat->msgs[chat->n_msgs - 1].importance = (llm_msg_importance_t)tool_imp;
            chat->msgs[chat->n_msgs - 1].msg_type = tr.success ? LLM_MSG_TOOL_RESULT : LLM_MSG_ERROR;
        }

        /* Harness-1 §4.2: Track tool usage for diversity nudging */
        {
            int tidx = react_tool_index(action_name);
            if (tidx >= 0 && tidx < 32)
                ctx->tools->tool_use_counts[tidx]++;
            ctx->tools->n_tool_uses++;
        }

        /* Harness-1 §3.4: Auto-seed scratchpad from first successful tool result.
         * Ensures scratchpad is never empty when eviction kicks in.
         * Like Harness-1's auto-seeding of the curated set at "fair" importance. */
        if (step == 0 && tr.success && !is_dedup && meta_str) {
            int sp_exists = scratchpad_find(&ctx->tools->scratch, "auto_seed");
            if (sp_exists < 0) {
                /* Extract first 500 chars of tool output as seed */
                int seed_len = (int)strlen(meta_str);
                if (seed_len > 500) seed_len = 500;
                char *seed = malloc((size_t)(seed_len + 64));
                if (seed) {
                    snprintf(seed, (size_t)(seed_len + 64),
                        "First result (%s): %.*s%s",
                        action_name, seed_len, meta_str,
                        (int)strlen(meta_str) > 500 ? "..." : "");
                    scratchpad_write(&ctx->tools->scratch, "auto_seed", seed, 3);
                    free(seed);
                }
            }
        }

        /* P3: Error-triggered reactive retrieval — when a tool fails, query
         * memory with the error message to surface relevant lessons/skills.
         * Based on: arXiv 2605.30621 "Harness Updating Is Not Harness Benefit"
         * — activation failure occurs when stored knowledge exists but isn't
         * retrieved. Error scenarios are the highest-value retrieval opportunity
         * because the agent may have a lesson about exactly this error.
         *
         * All thresholds are configurable via [limits] in config.toml:
         *   error_recall_min_length    — min error text chars to trigger (default 10)
         *   error_recall_candidates    — candidates to retrieve (default 3)
         *   error_recall_max_inject    — max entries to inject (default 1)
         *   error_recall_min_relevance — relevance floor for injection (default 0.25) */
        if (!tr.success && ctx->tools->memory && ctx->flags.inject_memory) {
            cJSON *err_j = cJSON_GetObjectItem(tr.meta, "error");
            const char *err_text = err_j ? err_j->valuestring : NULL;
            int err_min_len = ctx->tools->cfg
                ? ctx->tools->cfg->error_recall_min_length : 10;
            if (err_text && (int)strlen(err_text) > err_min_len) {
                /* Build recall query from error text + action name */
                char err_query[512];
                snprintf(err_query, sizeof(err_query), "error: %.400s %s",
                         err_text, action_name);
                int err_candidates = ctx->tools->cfg
                    ? ctx->tools->cfg->error_recall_candidates : 3;
                memory_results_t err_mem = memory_recall(ctx->tools->memory,
                                                          err_query, err_candidates);
                int err_max_inject = ctx->tools->cfg
                    ? ctx->tools->cfg->error_recall_max_inject : 1;
                double err_min_rel = ctx->tools->cfg
                    ? ctx->tools->cfg->error_recall_min_relevance : 0.25;
                /* Inject top relevant matches not already recalled */
                int injected = 0;
                for (int j = 0; j < err_mem.count && injected < err_max_inject; j++) {
                    int dup = 0;
                    for (int k = 0; k < ctx->tools->n_recalled_keys; k++) {
                        if (strcmp(ctx->tools->recalled_keys[k],
                                   err_mem.entries[j].key) == 0) {
                            dup = 1;
                            break;
                        }
                    }
                    if (!dup && err_mem.entries[j].relevance > err_min_rel) {
                        char hint[2048];
                        snprintf(hint, sizeof(hint),
                            "[MEMORY HINT — relevant to this error]\n"
                            "--- %s ---\n%s",
                            err_mem.entries[j].key,
                            err_mem.entries[j].value);
                        llm_chat_add_typed(chat, "user", hint, LLM_MSG_MEMORY_HINT);
                        tool_track_recalled_key(ctx->tools,
                                                 err_mem.entries[j].key);
                        injected++;
                    }
                }
                memory_results_free(&err_mem);
            }
        }

        /* ── Harness-1 §3.5: Multi-pass progressive context rendering ──────
         * Instead of binary eviction (keep/delete), use 5-pass progressive
         * degradation: LOW evict → NORMAL compress → NORMAL summarize →
         * NORMAL evict + HIGH truncate → nuclear eviction.
         * See: arXiv 2606.02373 "Harness-1" §3.5 budget-safe context rendering.
         *
         * Preserves the existing compaction floor and pair-safe boundary logic,
         * but adds importance-aware decision-making at each pass. */
        if (ctx->flags.enable_compaction && ctx->provider->cfg.context_size > 0) {
            float cpt_ev = react_get_chars_per_token(ctx);
            int context_budget = (int)(ctx->provider->cfg.context_size * cpt_ev);
            int eviction_pct = ctx->tools->cfg ? ctx->tools->cfg->context_eviction_pct : 70;
            int keep_head = REACT_EVICT_KEEP_HEAD;
            int keep_tail = REACT_EVICT_KEEP_TAIL;

            /* Recalculate total chars */
            int total_chars = 0;
            for (int i = 0; i < chat->n_msgs; i++)
                if (chat->msgs[i].content)
                    total_chars += (int)strlen(chat->msgs[i].content);
            int usage_pct = (int)(100.0 * total_chars / context_budget);

            if (usage_pct > eviction_pct && chat->n_msgs > keep_head + keep_tail + 1) {
                int did_evict = 0;

                /* ── Pass 1: Strip LOW importance messages (errors, stale hints, deduped)
                 * These have the least value and may actively degrade performance. */
                for (int i = keep_head; i < chat->n_msgs - keep_tail; i++) {
                    if (chat->msgs[i].importance == LLM_MSG_IMPORTANCE_LOW) {
                        llm_chat_remove_range(chat, i, i + 1);
                        i--;
                        did_evict = 1;
                    }
                }
                if (did_evict) {
                    total_chars = 0;
                    for (int i = 0; i < chat->n_msgs; i++)
                        if (chat->msgs[i].content)
                            total_chars += (int)strlen(chat->msgs[i].content);
                    usage_pct = (int)(100.0 * total_chars / context_budget);
                }

                /* ── Pass 2: Compress NORMAL messages to top-4 sentences.
                 * Uses sentence-BM25 relevance scoring (Harness-1 §3.1).
                 * Only compresses messages longer than 500 chars. */
                if (usage_pct > eviction_pct) {
                    for (int i = keep_head; i < chat->n_msgs - keep_tail; i++) {
                        if (chat->msgs[i].importance <= LLM_MSG_IMPORTANCE_NORMAL &&
                            chat->msgs[i].content &&
                            (int)strlen(chat->msgs[i].content) > 500) {
                            char *compressed = compress_to_relevant(
                                chat->msgs[i].content, user_query, 4, 400);
                            if (compressed) {
                                free(chat->msgs[i].content);
                                chat->msgs[i].content = compressed;
                                did_evict = 1;
                            }
                        }
                    }
                    if (did_evict) {
                        total_chars = 0;
                        for (int i = 0; i < chat->n_msgs; i++)
                            if (chat->msgs[i].content)
                                total_chars += (int)strlen(chat->msgs[i].content);
                        usage_pct = (int)(100.0 * total_chars / context_budget);
                    }
                }

                /* ── Pass 3: Evict NORMAL middle messages entirely.
                 * Standard eviction of the evictable range, but now only NORMAL
                 * messages have survived (LOW already gone, HIGH preserved). */
                if (usage_pct > eviction_pct) {
                    int evict_start = keep_head;
                    int evict_end = chat->n_msgs - keep_tail;

                    /* FIX B5: Adjust eviction boundary to not split tool_call/tool_result pairs */
                    for (int adj_iter = 0; evict_end > evict_start && evict_end < chat->n_msgs && adj_iter < 20; adj_iter++) {
                        if (chat->msgs[evict_end].tool_call_id) {
                            evict_end++;
                            continue;
                        }
                        if (evict_end - 1 >= evict_start &&
                            chat->msgs[evict_end - 1].tool_calls_json) {
                            evict_end--;
                            continue;
                        }
                        break;
                    }

                    /* Compaction floor — never drop below 20% of context window */
                    {
                        int floor_chars = context_budget / 5;
                        if (floor_chars < 4000) floor_chars = 4000;
                        int kept_chars = 0;
                        for (int ki = 0; ki < evict_start; ki++)
                            if (chat->msgs[ki].content)
                                kept_chars += (int)strlen(chat->msgs[ki].content);
                        for (int ki = evict_end; ki < chat->n_msgs; ki++)
                            if (chat->msgs[ki].content)
                                kept_chars += (int)strlen(chat->msgs[ki].content);
                        while (kept_chars < floor_chars && evict_end > evict_start + 1) {
                            evict_end--;
                            if (chat->msgs[evict_end].content)
                                kept_chars += (int)strlen(chat->msgs[evict_end].content);
                        }
                    }

                    if (evict_end > evict_start) {
                        /* Heuristic extraction into scratchpad before eviction */
                        {
                            size_t sp_budget = (size_t)(context_budget * REACT_SCRATCHPAD_BUDGET_PCT / 100);
                            str_t summary = str_new(sp_budget > 4096 ? 4096 : sp_budget);
                            int max_per_msg = (int)(sp_budget / (unsigned)(evict_end - evict_start + 1));
                            if (max_per_msg < 200) max_per_msg = 200;
                            if (max_per_msg > 2000) max_per_msg = 2000;

                            for (int i = evict_start; i < evict_end; i++) {
                                const char *content = chat->msgs[i].content;
                                const char *role = chat->msgs[i].role;
                                if (!content || !content[0] || !role) continue;
                                if (strcmp(role, "system") == 0) continue;
                                if (strcmp(role, "tool") == 0 && strlen(content) < 50) continue;

                                int clen = (int)strlen(content);
                                if (clen > max_per_msg) clen = max_per_msg;
                                str_appendf(&summary, "[%s]: ", role);
                                str_append(&summary, content, (size_t)clen);
                                if ((int)strlen(content) > max_per_msg)
                                    str_append_cstr(&summary, "...[truncated]");
                                str_append_cstr(&summary, "\n");
                                if (summary.len >= sp_budget) break;
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
                        }

                        /* Remove evicted messages */
                        llm_chat_remove_range(chat, evict_start, evict_end);

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

                        /* Re-inject scratchpad at eviction point */
                        {
                            size_t sp_max = (ctx->provider->cfg.context_size > 0)
                                ? (size_t)(ctx->provider->cfg.context_size * react_get_chars_per_token(ctx) * 15 / 100) : 8192;
                            char *fresh_sp = scratchpad_serialize_budget(
                                &ctx->tools->scratch, sp_max);
                            if (fresh_sp && fresh_sp[0]) {
                                size_t slen = strlen(fresh_sp);
                                char *sp_msg = malloc(slen + 32);
                                if (sp_msg) {
                                    snprintf(sp_msg, slen + 32, "[SCRATCHPAD]\n%s", fresh_sp);
                                    /* Use LLM_MSG_SCRATCHPAD (not EVICTION_SUMMARY)
                                     * so Tier 2 recovery can find it via
                                     * llm_chat_find_by_type(LLM_MSG_SCRATCHPAD). */
                                    llm_chat_insert_typed(chat, evict_start,
                                        "user", sp_msg, LLM_MSG_SCRATCHPAD);
                                    free(sp_msg);
                                }
                            }
                            free(fresh_sp);
                        }

                        react_event_t ev = {0};
                        ev.react_loop = ctx->tools->react_loop;
                        ev.type = REACT_EVENT_WARNING;
                        ev.step = step + 1;
                        ev.message = "Context compacted (multi-pass) — evicted messages summarized into scratchpad";
                        react_emit(on_event, userdata, &ev);
                    }
                }

                /* Harness-1 §4.2: Tool diversity nudge — if the agent has used
                 * only 1-2 tools for 10+ steps, inject a soft reminder to use
                 * notes for saving findings. */
                if (ctx->tools->n_tool_uses >= 10) {
                    int notes_idx = react_tool_index("notes");
                    int notes_used = (notes_idx >= 0 && notes_idx < 32)
                        ? ctx->tools->tool_use_counts[notes_idx] : 0;
                    if (notes_used == 0) {
                        llm_chat_add_typed(chat, "user",
                            "[HINT] You have not used notes() to save key findings. "
                            "Consider saving important discoveries to scratchpad sections "
                            "to preserve them across context compaction.",
                            LLM_MSG_MEMORY_HINT);
                        /* Mark as having been nudged (set a fake count so we don't re-nudge) */
                        if (notes_idx >= 0 && notes_idx < 32)
                            ctx->tools->tool_use_counts[notes_idx] = -1;
                    }
                }
            }
        }

        /* Cleanup */
        free(meta_str);
        free(result_msg);
        tool_result_free(&tr);

        /* Save checkpoint after each tool execution (atomic write) */
        if (!final_result) react_checkpoint_save(ctx, step + 1, user_query,
                        chat->last_tool_call_id);

        /* Check for pause request (Space pressed in TUI — toggle pause/resume).
         * Save checkpoint and exit cleanly so the task can be resumed later. */
        if (!final_result && ctx->pause_requested) {
            react_event_t ev = {0};
            ev.react_loop = ctx->tools->react_loop;
            ev.type = REACT_EVENT_WARNING;
            ev.step = step + 1;
            ev.message = "Paused (Space to resume, type query to redirect)";
            react_emit(on_event, userdata, &ev);
            /* Checkpoint already saved above — just break out of the loop */
            cJSON_Delete(action);
            free(response);
            break;
        }

        cJSON_Delete(action);
        free(response);
    }

    llm_chat_free(chat);

    /* Remove checkpoint — task completed (successfully or not).
     * Only needed for non-done exits (max steps, errors);
     * the done handler already removes it on success. */
    if (!final_result)
        react_checkpoint_remove(ctx);

    /* Post-loop: validation scoring, reflection, promotion, pruning */
    {
        int task_succeeded = (final_result != NULL);
        react_post_loop(ctx, user_query, final_result, task_succeeded,
                        on_event, userdata);
    }

    /* Don't free last_query/last_result here — the caller (main.c) manages them.
     * They are set after each react_run() call and used to inject previous context. */

    /* Reset recalled keys for next query (each task is independent) */
    for (int i = 0; i < ctx->tools->n_recalled_keys; i++)
        free(ctx->tools->recalled_keys[i]);
    free(ctx->tools->recalled_keys);
    ctx->tools->recalled_keys = NULL;
    ctx->tools->n_recalled_keys = 0;
    ctx->tools->recalled_keys_cap = 0;

    free(last_sigs);
    return final_result;
}
