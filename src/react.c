#include "react.h"
#include "memory.h"
#include "journal.h"
#include "store.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── helpers ─────────────────────────────────────────── */

static int count_lines(const char *s) {
    int n = 0;
    for (; *s; s++) if (*s == '\n') n++;
    return n;
}

static const char *json_get_str(cJSON *obj, const char *key) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item && cJSON_IsString(item)) return item->valuestring;
    return NULL;
}

/* Streaming token callback context — bridges llm_token_fn to react_event_fn */
typedef struct {
    react_event_fn on_event;
    void          *userdata;
    int            step;
} stream_ctx_t;

static void stream_token_cb(const char *token, void *userdata) {
    stream_ctx_t *sctx = userdata;
    if (!sctx->on_event) return;
    react_event_t ev = {0};
    ev.type  = REACT_EVENT_LLM_TOKEN;
    ev.step  = sctx->step;
    ev.token = token;
    sctx->on_event(&ev, sctx->userdata);
}

static void emit(react_event_fn fn, void *ud, react_event_t *ev) {
    if (fn) fn(ev, ud);
}

/* Extract the key display parameter for a tool action */
static const char *get_action_desc(cJSON *action, const char *action_name,
                                   const char *thought) {
    if (strcmp(action_name, "shell_exec") == 0)
        return json_get_str(action, "command");
    if (strcmp(action_name, "file_read") == 0 ||
        strcmp(action_name, "file_write") == 0 ||
        strcmp(action_name, "file_edit") == 0)
        return json_get_str(action, "path");
    if (strcmp(action_name, "grep_search") == 0)
        return json_get_str(action, "pattern");
    if (strcmp(action_name, "notes") == 0)
        return "[saving notes]";
    if (strcmp(action_name, "done") == 0)
        return thought;
    return thought;
}

/* ── main react loop ─────────────────────────────────── */

char *react_run(react_ctx_t *ctx, const char *user_query,
                react_event_fn on_event, void *userdata) {
    llm_chat_t *chat = llm_chat_new();

    /* System message */
    llm_chat_add(chat, "system", tools_system_prompt());

    /* Inject journal manifest (shows what previous steps produced) */
    char *manifest = journal_manifest(ctx->tools->journal, 50);
    if (manifest) {
        llm_chat_add(chat, "user", manifest);
        free(manifest);
    }

    /* Inject memory index (list of available memories for the LLM to know about) */
    if (ctx->tools->memory) {
        int mem_max = ctx->tools->cfg ? ctx->tools->cfg->memory_index_max : 50;
        char *mem_index = memory_build_index(ctx->tools->memory, mem_max);
        if (mem_index && strlen(mem_index) > 0) {
            char *mem_msg = malloc(strlen(mem_index) + 64);
            if (mem_msg) {
                sprintf(mem_msg, "[MEMORY INDEX]\n%s", mem_index);
                llm_chat_add(chat, "user", mem_msg);
                free(mem_msg);
            }
        }
        free(mem_index);

        /* Inject pinned memories (always-active knowledge) */
        char *pinned = memory_load_pinned(ctx->tools->memory);
        if (pinned && strlen(pinned) > 0) {
            char *pin_msg = malloc(strlen(pinned) + 64);
            if (pin_msg) {
                sprintf(pin_msg, "[PINNED KNOWLEDGE]\n%s", pinned);
                llm_chat_add(chat, "user", pin_msg);
                free(pin_msg);
            }
        }
        free(pinned);

        /* Inject relevant skills (procedural memory — loaded on-demand based on query) */
        int max_skills = ctx->tools->cfg ? ctx->tools->cfg->max_skills_per_query : 3;
        memory_results_t skills = memory_recall(ctx->tools->memory, "skill:", max_skills);
        if (skills.count > 0) {
            str_t skill_msg = str_new(4096);
            str_append_cstr(&skill_msg, "[RELEVANT SKILLS]\n");
            for (int i = 0; i < skills.count; i++) {
                if (skills.entries[i].key &&
                    strncmp(skills.entries[i].key, "skill:", 6) == 0) {
                    str_appendf(&skill_msg, "\n--- %s ---\n%s\n",
                                skills.entries[i].key,
                                skills.entries[i].value ? skills.entries[i].value : "");
                }
            }
            if (skill_msg.len > 20) {  /* more than just the header */
                llm_chat_add(chat, "user", str_cstr(&skill_msg));
            }
            str_free(&skill_msg);
        }
        memory_results_free(&skills);
    }

    /* Compute scratchpad limit from context size (5% of context, min 2K, max 32K) */
    size_t max_scratchpad = 8192;  /* fallback if context_size unknown */
    if (ctx->llm->context_size > 0) {
        max_scratchpad = (size_t)ctx->llm->context_size * 4 / 20;  /* ~5% in chars (~4 chars/tok) */
        if (max_scratchpad < 2048)  max_scratchpad = 2048;
        if (max_scratchpad > 32768) max_scratchpad = 32768;
    }

    /* Inject scratchpad if exists (capped at computed limit) */
    if (ctx->tools->scratchpad && ctx->tools->scratchpad[0]) {
        size_t slen = strlen(ctx->tools->scratchpad);
        if (slen > max_scratchpad) slen = max_scratchpad;
        char *scratch_msg = malloc(slen + 32);
        if (scratch_msg) {
            snprintf(scratch_msg, slen + 32, "[SCRATCHPAD]\n%.*s",
                     (int)slen, ctx->tools->scratchpad);
            llm_chat_add(chat, "user", scratch_msg);
            free(scratch_msg);
        }
    }

    /* Inject last exchange summary for cross-query context (handles "do the same..." references) */
    if (ctx->last_query && ctx->last_result) {
        char last_ex[1024];
        snprintf(last_ex, sizeof(last_ex),
                 "[Previous query: \"%.200s\" → \"%.500s\"]",
                 ctx->last_query, ctx->last_result);
        llm_chat_add(chat, "user", last_ex);
    }

    /* User query */
    llm_chat_add(chat, "user", user_query);

    /* Reset per-loop step counter (react_loop is 0-based, incremented at END of loop) */
    ctx->tools->step = 0;

    /* Record system prompt and user query in journal (step 0) */
    {
        const char *sys_prompt = tools_system_prompt();
        char *sys_hash = store_save(ctx->tools->store, sys_prompt);
        /* Register alias for system prompt (function auto-generates S0, S1, ...) */
        const char *sys_alias = tool_register_alias(ctx->tools, sys_hash);
        cJSON *sys_p = cJSON_CreateObject();
        cJSON_AddStringToObject(sys_p, "type", "system_prompt");
        journal_append(ctx->tools->journal, ctx->tools->react_loop, 0, "system", sys_p, sys_alias,
                       strlen(sys_prompt), count_lines(sys_prompt), NULL);
        cJSON_Delete(sys_p);
        free(sys_hash);

        cJSON *q_p = cJSON_CreateObject();
        cJSON_AddStringToObject(q_p, "text", user_query);
        journal_append(ctx->tools->journal, ctx->tools->react_loop, 0, "query", q_p, NULL,
                       strlen(user_query), 0, NULL);
        cJSON_Delete(q_p);
    }

    char *final_result = NULL;
    struct timespec task_start;
    clock_gettime(CLOCK_MONOTONIC, &task_start);

    /* Action signature tracking for cycling detection */
    char last_sigs[8][256];
    int sig_count = 0;

    for (int step = 0; step < ctx->max_steps; step++) {
        ctx->tools->step = step + 1;

        /* Emit step start */
        {
            react_event_t ev = {0};
            ev.type = REACT_EVENT_STEP_START;
            ev.step = step + 1;
            ev.max_steps = ctx->max_steps;
            emit(on_event, userdata, &ev);
        }

        /* Call LLM */
        struct timespec step_start;
        clock_gettime(CLOCK_MONOTONIC, &step_start);

        llm_stats_t stats = {0};
        stream_ctx_t sctx = { on_event, userdata, step + 1 };
        int max_resp = ctx->tools->cfg ? ctx->tools->cfg->llm_max_response : 10*1024*1024;
        int rep_thresh = ctx->tools->cfg ? ctx->tools->cfg->llm_repeat_threshold : 100;
        char *response = llm_complete_stream(ctx->llm, chat, &stats,
            on_event ? stream_token_cb : NULL, &sctx,
            max_resp, rep_thresh);
        if (!response) {
            react_event_t ev = {0};
            ev.type = REACT_EVENT_ERROR;
            ev.step = step + 1;
            ev.message = "LLM call failed (returned NULL)";
            emit(on_event, userdata, &ev);
            break;
        }

        struct timespec step_end;
        clock_gettime(CLOCK_MONOTONIC, &step_end);
        double step_elapsed = (step_end.tv_sec - step_start.tv_sec) +
                              (step_end.tv_nsec - step_start.tv_nsec) / 1e9;

        /* Parse JSON response */
        cJSON *action = llm_parse_action(response);

        if (!action) {
            react_event_t ev = {0};
            ev.type = REACT_EVENT_ERROR;
            ev.step = step + 1;
            ev.message = "Failed to parse LLM response as JSON";
            emit(on_event, userdata, &ev);
            /* Retry */
            llm_chat_add(chat, "assistant", response);
            llm_chat_add(chat, "user",
                "Your response was not valid JSON. "
                "Reply with ONLY a JSON object: "
                "{\"thought\": \"...\", \"action\": \"tool_name\", ...}");
            free(response);
            continue;
        }

        const char *thought = json_get_str(action, "thought");
        const char *action_name = json_get_str(action, "action");

        if (!action_name) {
            react_event_t ev = {0};
            ev.type = REACT_EVENT_ERROR;
            ev.step = step + 1;
            ev.message = "No 'action' field in response";
            emit(on_event, userdata, &ev);
            /* Retry */
            llm_chat_add(chat, "assistant", response);
            llm_chat_add(chat, "user",
                "Your JSON response is missing the required 'action' field. "
                "Reply with ONLY a JSON object like: "
                "{\"thought\": \"...\", \"action\": \"tool_name\", \"param\": \"value\"}\n"
                "Available actions: shell_exec, file_read, file_write, file_edit, "
                "grep_search, notes, done");
            cJSON_Delete(action);
            free(response);
            continue;
        }

        const char *desc = get_action_desc(action, action_name, thought);

        /* Check for done */
        if (strcmp(action_name, "done") == 0) {
            const char *result = json_get_str(action, "result");
            final_result = result ? strdup(result) : strdup("(no result)");

            /* Store result for full audit trail (journal + store/) */
            tool_result_t tr = tool_execute(ctx->tools, action_name, action);
            tool_result_free(&tr);

            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double total = (now.tv_sec - task_start.tv_sec) +
                           (now.tv_nsec - task_start.tv_nsec) / 1e9;

            react_event_t ev = {0};
            ev.type = REACT_EVENT_DONE;
            ev.step = step + 1;
            ev.step_elapsed = step_elapsed;
            ev.total_elapsed = total;
            ev.action = action_name;
            ev.description = desc;
            ev.result = final_result;
            ev.stats = stats;
            ev.context_size = ctx->llm->context_size;
            emit(on_event, userdata, &ev);

            cJSON_Delete(action);
            free(response);
            break;
        }

        /* Cycling detection */
        char sig[256];
        const char *cmd = json_get_str(action, "command");
        const char *path = json_get_str(action, "path");
        const char *pattern = json_get_str(action, "pattern");
        snprintf(sig, sizeof(sig), "%s:%s:%s:%s",
                 action_name,
                 cmd ? cmd : "",
                 path ? path : "",
                 pattern ? pattern : "");

        int repeated = 0;
        for (int i = 0; i < sig_count && i < 8; i++) {
            if (strcmp(last_sigs[i], sig) == 0) repeated++;
        }
        if (sig_count < 8) {
            snprintf(last_sigs[sig_count], 256, "%s", sig);
            sig_count++;
        } else {
            memmove(last_sigs, last_sigs + 1, 7 * 256);
            snprintf(last_sigs[7], 256, "%s", sig);
        }

        if (repeated >= 2) {
            char warn_msg[256];
            snprintf(warn_msg, sizeof(warn_msg),
                     "Cycling detected — same action repeated %d times", repeated + 1);
            react_event_t ev = {0};
            ev.type = REACT_EVENT_WARNING;
            ev.step = step + 1;
            ev.message = warn_msg;
            emit(on_event, userdata, &ev);
        }

        /* Execute tool */
        tool_result_t tr = tool_execute(ctx->tools, action_name, action);

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double total_elapsed = (now.tv_sec - task_start.tv_sec) +
                               (now.tv_nsec - task_start.tv_nsec) / 1e9;

        /* Emit step complete */
        {
            react_event_t ev = {0};
            ev.type = REACT_EVENT_STEP_COMPLETE;
            ev.step = step + 1;
            ev.max_steps = ctx->max_steps;
            ev.step_elapsed = step_elapsed;
            ev.total_elapsed = total_elapsed;
            ev.action = action_name;
            ev.description = desc ? desc : "";
            ev.stats = stats;
            ev.context_size = ctx->llm->context_size;
            emit(on_event, userdata, &ev);
        }

        /* Emit tool output */
        {
            react_event_t ev = {0};
            ev.type = REACT_EVENT_TOOL_OUTPUT;
            ev.step = step + 1;
            ev.tool_meta = tr.meta;
            ev.store_ref = tr.store_ref;
            emit(on_event, userdata, &ev);
        }

        /* Build tool result string for context */
        char *meta_str = cJSON_PrintUnformatted(tr.meta);
        size_t result_len = strlen(meta_str) + 128;
        char *result_msg = malloc(result_len);
        snprintf(result_msg, result_len, "%s\n[step %d | %.1fs]",
                 meta_str, step + 1, total_elapsed);

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

        /* Within-loop context management: evict old messages when context gets full */
        if (ctx->llm->context_size > 0) {
            int total_chars = 0;
            for (int i = 0; i < chat->n_msgs; i++)
                total_chars += (int)strlen(chat->msgs[i].content);
            int usage_pct = (int)(100.0 * total_chars / (ctx->llm->context_size * 4));
            if (usage_pct > (ctx->tools->cfg ? ctx->tools->cfg->context_eviction_pct : 70) && chat->n_msgs > 6) {
                /* Priority eviction: remove error messages first (research: errors in context degrade performance) */
                for (int i = 3; i < chat->n_msgs - 4; i++) {
                    if (chat->msgs[i].content && strstr(chat->msgs[i].content, "ERROR:")) {
                        free(chat->msgs[i].role);
                        free(chat->msgs[i].content);
                        memmove(&chat->msgs[i], &chat->msgs[i + 1],
                                (chat->n_msgs - i - 1) * sizeof(llm_msg_t));
                        chat->n_msgs--;
                        i--;  /* re-check this position */
                    }
                }

                /* Recalculate after error eviction */
                total_chars = 0;
                for (int i = 0; i < chat->n_msgs; i++)
                    total_chars += (int)strlen(chat->msgs[i].content);
                usage_pct = (int)(100.0 * total_chars / (ctx->llm->context_size * 4));

                /* If still over 70%, do standard eviction */
                int keep_head = 3;
                int keep_tail = 4;
                int evict_start = keep_head;
                int evict_end = chat->n_msgs - keep_tail;
                if (usage_pct > (ctx->tools->cfg ? ctx->tools->cfg->context_eviction_pct : 70) && evict_end > evict_start) {
                    /* Free evicted messages */
                    for (int i = evict_start; i < evict_end; i++) {
                        free(chat->msgs[i].role);
                        free(chat->msgs[i].content);
                    }
                    /* Shift tail messages down */
                    int tail_count = chat->n_msgs - evict_end;
                    memmove(&chat->msgs[evict_start], &chat->msgs[evict_end],
                            tail_count * sizeof(llm_msg_t));
                    chat->n_msgs = evict_start + tail_count;

                    /* Re-inject fresh manifest at position keep_head */
                    char *fresh_manifest = journal_manifest(ctx->tools->journal, 50);
                    if (fresh_manifest) {
                        /* Insert manifest as a new message at keep_head */
                        if (chat->n_msgs >= chat->cap_msgs) {
                            chat->cap_msgs *= 2;
                            chat->msgs = realloc(chat->msgs, chat->cap_msgs * sizeof(llm_msg_t));
                        }
                        memmove(&chat->msgs[evict_start + 1], &chat->msgs[evict_start],
                                (chat->n_msgs - evict_start) * sizeof(llm_msg_t));
                        chat->msgs[evict_start].role = strdup("user");
                        chat->msgs[evict_start].content = fresh_manifest;
                        chat->n_msgs++;
                    }

                    /* Emit warning */
                    react_event_t ev = {0};
                    ev.type = REACT_EVENT_WARNING;
                    ev.step = step + 1;
                    ev.message = "Context compacted — old messages evicted, manifest refreshed";
                    emit(on_event, userdata, &ev);
                }
            }
        }

        /* Cleanup */
        free(meta_str);
        free(result_msg);
        tool_result_free(&tr);
        cJSON_Delete(action);
        free(response);
    }

    llm_chat_free(chat);

    /* Post-task reflection: ask LLM to extract reusable lessons/strategies.
     * Fires for BOTH successful and failed tasks — failures are often more
     * valuable for learning (what went wrong, what to avoid next time). */
    int task_succeeded = (final_result != NULL);
    if (ctx->tools->step > 2 && ctx->tools->memory) {
        llm_chat_t *reflect = llm_chat_new();
        if (task_succeeded) {
            llm_chat_add(reflect, "system",
                "You just completed a task successfully. Review what happened and extract "
                "0-3 reusable lessons, strategies, or reusable skills. For each, call "
                "memory_store with:\n"
                "- key: lesson:short-name, strategy:short-name, or skill:short-name\n"
                "- value: the reusable knowledge (for skills: include approach, pitfalls, "
                "verification)\n"
                "- tags: comma-separated relevant tags\n"
                "Skills are reusable multi-step procedures (e.g. skill:compile-and-test-c).\n"
                "If nothing worth storing, call done immediately.\n"
                "Respond with ONE JSON object per turn: "
                "{\"thought\":\"...\",\"action\":\"memory_store\"|\"done\",...}");
        } else {
            llm_chat_add(reflect, "system",
                "The task FAILED or was not completed (hit max steps, error, or timeout). "
                "Review the step history and extract 1-3 lessons about what went wrong. "
                "Focus on:\n"
                "- What caused the failure (wrong approach, missing tool, bad assumption)\n"
                "- What to do differently next time\n"
                "- Any pitfalls or gotchas to remember\n"
                "For each lesson, call memory_store with:\n"
                "- key: lesson:short-name (e.g. lesson:avoid-recursive-grep-on-large-dirs)\n"
                "- value: what went wrong and how to avoid it\n"
                "- tags: comma-separated relevant tags\n"
                "If nothing worth storing, call done immediately.\n"
                "Respond with ONE JSON object per turn: "
                "{\"thought\":\"...\",\"action\":\"memory_store\"|\"done\",...}");
        }

        /* Inject journal manifest as context for reflection */
        char *manifest = journal_manifest(ctx->tools->journal, 50);
        if (manifest) {
            llm_chat_add(reflect, "user", manifest);
            free(manifest);
        }
        llm_chat_add(reflect, "user",
            task_succeeded
                ? "What lessons or strategies should be stored from this task? "
                  "Call memory_store for each, or done if none."
                : "This task failed. What went wrong? What lessons should be stored "
                  "to avoid this failure next time? Call memory_store for each, or "
                  "done if none.");

        /* Mini react loop for reflection (max 4 steps) */
        for (int rstep = 0; rstep < (ctx->tools->cfg ? ctx->tools->cfg->max_reflection_steps : 4); rstep++) {
            llm_stats_t rstats = {0};
            int max_resp = ctx->tools->cfg ? ctx->tools->cfg->llm_max_response : 10*1024*1024;
            int rep_thresh = ctx->tools->cfg ? ctx->tools->cfg->llm_repeat_threshold : 100;
            char *rresp = llm_complete_stream(ctx->llm, reflect, &rstats,
                NULL, NULL, max_resp, rep_thresh);
            if (!rresp) break;

            cJSON *raction = llm_parse_action(rresp);
            if (!raction) { free(rresp); break; }

            const char *ract = NULL;
            cJSON *act_item = cJSON_GetObjectItemCaseSensitive(raction, "action");
            if (act_item && cJSON_IsString(act_item)) ract = act_item->valuestring;

            if (!ract || strcmp(ract, "done") == 0) {
                cJSON_Delete(raction);
                free(rresp);
                break;
            }

            if (strcmp(ract, "memory_store") == 0) {
                tool_result_t tr = tool_execute(ctx->tools, "memory_store", raction);
                /* Emit event so frontend can show it */
                react_event_t ev = {0};
                ev.type = REACT_EVENT_STEP_COMPLETE;
                ev.action = "memory_store";
                ev.description = "[reflection]";
                emit(on_event, userdata, &ev);
                tool_result_free(&tr);
            }

            llm_chat_add(reflect, "assistant", rresp);
            llm_chat_add(reflect, "user",
                "Stored. Any more lessons? Call memory_store or done.");
            cJSON_Delete(raction);
            free(rresp);
        }
        llm_chat_free(reflect);
    }

    /* Save last query+result for next react loop's context injection */
    if (ctx->last_query) free(ctx->last_query);
    if (ctx->last_result) free(ctx->last_result);
    ctx->last_query = strdup(user_query);
    ctx->last_result = final_result ? strdup(final_result) : NULL;

    /* Increment react loop counter for next query */
    ctx->tools->react_loop++;

    return final_result;
}
