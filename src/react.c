#include "react.h"
#include "config.h"
#include "memory.h"
#include "journal.h"
#include "store.h"
#include "cJSON.h"
#include "str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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
/* Checkpoint restore function — to be inserted into react.c after checkpoint_remove() */

/* Restore conversation state from checkpoint.json + journal.jsonl + .store/
 * Returns the step number to resume from, or -1 if no checkpoint exists.
 * Populates chat with reconstructed messages. */
static int checkpoint_restore(react_ctx_t *ctx, llm_chat_t *chat,
                               const char *user_query) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/checkpoint.json",
             ctx->tools->session_dir);

    FILE *f = fopen(path, "r");
    if (!f) return -1;  /* no checkpoint — start fresh */

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return -1; }  /* #3: ftell failure */
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -1; }
    size_t nread = fread(buf, 1, (size_t)sz, f);  /* #2: check fread */
    buf[nread] = '\0';
    fclose(f);

    cJSON *cp = cJSON_Parse(buf);
    free(buf);
    if (!cp) return -1;

    int saved_step = (int)cJSON_GetNumberValue(
        cJSON_GetObjectItem(cp, "step"));
    int saved_loop = (int)cJSON_GetNumberValue(
        cJSON_GetObjectItem(cp, "react_loop"));

    /* Restore scratchpad */
    cJSON *sp = cJSON_GetObjectItem(cp, "scratchpad");
    if (sp && sp->valuestring && sp->valuestring[0]) {
        free(ctx->tools->scratchpad);
        ctx->tools->scratchpad = strdup(sp->valuestring);
    }

    /* Restore aliases from journal symlinks */
    /* (aliases are re-derived from journal refs below) */

    cJSON_Delete(cp);

    /* Step 1: Add system prompt (fresh — may have changed) */
    llm_chat_add(chat, "system", tools_system_prompt());

    /* Step 2: Add fresh manifest (shows full history including pre-crash steps) */
    char *manifest = journal_manifest(ctx->tools->journal, 50);
    if (manifest) {
        llm_chat_add(chat, "user", manifest);
        free(manifest);
    }

    /* Step 3: Add memory context (fresh) */
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
    }

    /* Step 4: Add scratchpad if exists */
    if (ctx->tools->scratchpad && ctx->tools->scratchpad[0]) {
        size_t slen = strlen(ctx->tools->scratchpad);
        char *scratch_msg = malloc(slen + 32);
        if (scratch_msg) {
            snprintf(scratch_msg, slen + 32, "[SCRATCHPAD]\n%.*s",
                     (int)slen, ctx->tools->scratchpad);
            llm_chat_add(chat, "user", scratch_msg);
            free(scratch_msg);
        }
    }

    /* Step 5: Add user query */
    llm_chat_add(chat, "user", user_query);

    /* Step 6: Replay tool calls from journal to rebuild conversation history */
    char jpath[4096];
    snprintf(jpath, sizeof(jpath), "%s/journal.jsonl",
             ctx->tools->session_dir);
    f = fopen(jpath, "r");
    if (!f) return saved_step;

    char line[65536];
    while (fgets(line, sizeof(line), f)) {
        cJSON *entry = cJSON_Parse(line);
        if (!entry) continue;

        int loop = (int)cJSON_GetNumberValue(
            cJSON_GetObjectItem(entry, "react_loop"));
        int step = (int)cJSON_GetNumberValue(
            cJSON_GetObjectItem(entry, "step"));
        const char *tool = cJSON_GetStringValue(
            cJSON_GetObjectItem(entry, "tool"));
        const char *ref = cJSON_GetStringValue(
            cJSON_GetObjectItem(entry, "ref"));
        const char *tc_id = cJSON_GetStringValue(
            cJSON_GetObjectItem(entry, "tc_id"));
        cJSON *params = cJSON_GetObjectItem(entry, "params");

        /* Only replay entries from the current react loop */
        if (loop != saved_loop) { cJSON_Delete(entry); continue; }

        /* Skip system, query, parse_error entries */
        if (!tool || strcmp(tool, "system") == 0 ||
            strcmp(tool, "query") == 0 ||
            strcmp(tool, "parse_error") == 0) {
            cJSON_Delete(entry);
            continue;
        }

        /* Re-register alias and track the hash for this entry */
        const char *entry_hash = NULL;
        char entry_hash_buf[128] = "";
        if (ref) {
            /* Extract hash from ref by resolving the symlink */
            char ref_path[4096];
            snprintf(ref_path, sizeof(ref_path), "%s/%s",
                     ctx->tools->session_dir, ref);
            char link_target[4096];
            ssize_t llen = readlink(ref_path, link_target, sizeof(link_target) - 1);
            if (llen > 0) {
                link_target[llen] = '\0';
                /* Extract hash from "../../store/<hash>" */
                const char *slash = strrchr(link_target, '/');
                if (slash) {
                    slash++;  /* skip the '/' */
                    snprintf(entry_hash_buf, sizeof(entry_hash_buf), "%s", slash);
                    entry_hash = entry_hash_buf;
                    /* Register in alias hash map */
                    alias_map_insert(ctx->tools->aliases, ref, slash);
                }
            }
        }

        /* Handle thinking steps */
        if (strcmp(tool, "thinking") == 0) {
            const char *thought = NULL;
            if (params) {
                cJSON *t = cJSON_GetObjectItem(params, "thought");
                if (t && t->valuestring) thought = t->valuestring;
            }
            if (thought) {
                /* Read the stored response for the full thinking content */
                if (ref && entry_hash) {
                    char *store_path = store_resolve(ctx->tools->store,
                        entry_hash);
                    if (store_path) {
                        size_t content_len = 0; (void)content_len;
                        char *content = NULL;
                        FILE *sf = fopen(store_path, "r");
                        if (sf) {
                            fseek(sf, 0, SEEK_END);
                            long csz = ftell(sf);
                            fseek(sf, 0, SEEK_SET);
                            content = malloc((size_t)csz + 1);
                            if (content) {
                                fread(content, 1, (size_t)csz, sf);
                                content[csz] = '\0';
                                content_len = (size_t)csz;
                            }
                            fclose(sf);
                        }
                        if (content) {
                            llm_chat_add(chat, "assistant", content);
                            free(content);
                        }
                        free(store_path);
                    }
                }
            }
            cJSON_Delete(entry);
            continue;
        }

        /* Regular tool call — reconstruct assistant + tool result messages */
        const char *thought = "";
        const char *action_name = tool;
        if (params) {
            cJSON *t = cJSON_GetObjectItem(params, "thought");
            if (t && t->valuestring) thought = t->valuestring;
        }

        if (tc_id) {
            /* Native tool_calls API format */
            /* Build tool_calls JSON */
            cJSON *tc_arr = cJSON_CreateArray();
            cJSON *tc = cJSON_CreateObject();
            cJSON_AddStringToObject(tc, "id", tc_id);
            cJSON_AddStringToObject(tc, "type", "function");
            cJSON *fn = cJSON_CreateObject();
            cJSON_AddStringToObject(fn, "name", action_name);

            /* Build arguments from params (exclude thought and action) */
            cJSON *args = cJSON_CreateObject();
            if (params) {
                cJSON *child = params->child;
                while (child) {
                    if (strcmp(child->string, "thought") != 0 &&
                        strcmp(child->string, "action") != 0) {
                        cJSON_AddItemToObject(args, child->string,
                            cJSON_Duplicate(child, 1));
                    }
                    child = child->next;
                }
            }
            char *args_str = cJSON_PrintUnformatted(args);
            cJSON_AddStringToObject(fn, "arguments", args_str ? args_str : "{}");
            free(args_str);
            cJSON_Delete(args);

            cJSON_AddItemToObject(tc, "function", fn);
            cJSON_AddItemToArray(tc_arr, tc);

            char *tc_json = cJSON_PrintUnformatted(tc_arr);
            cJSON_Delete(tc_arr);

            /* Add assistant message with tool_calls */
            llm_chat_add_assistant_tool_call(chat,
                (thought && thought[0]) ? thought : NULL,
                tc_json);

            /* Build tool result content */
            char result_content[1024];
            int rsize = (int)cJSON_GetNumberValue(
                cJSON_GetObjectItem(entry, "size"));
            snprintf(result_content, sizeof(result_content),
                     "{\"ref\":\"%s\",\"chars\":%d}\n[step %d | restored]",
                     ref ? ref : "?", rsize, step);

            llm_chat_add_tool_result(chat, tc_id, result_content);
            free(tc_json);
        } else {
            /* Legacy JSON-in-content format (no tc_id) */
            /* Build a unified JSON response */
            cJSON *unified = cJSON_CreateObject();
            cJSON_AddStringToObject(unified, "thought", thought);
            cJSON_AddStringToObject(unified, "action", action_name);
            if (params) {
                cJSON *child = params->child;
                while (child) {
                    if (strcmp(child->string, "thought") != 0 &&
                        strcmp(child->string, "action") != 0) {
                        cJSON_AddItemToObject(unified, child->string,
                            cJSON_Duplicate(child, 1));
                    }
                    child = child->next;
                }
            }
            char *resp = cJSON_PrintUnformatted(unified);
            cJSON_Delete(unified);

            llm_chat_add(chat, "assistant", resp ? resp : "{}");

            char result_content[1024];
            int rsize = (int)cJSON_GetNumberValue(
                cJSON_GetObjectItem(entry, "size"));
            snprintf(result_content, sizeof(result_content),
                     "{\"ref\":\"%s\",\"chars\":%d}\n[step %d | restored]",
                     ref ? ref : "?", rsize, step);
            llm_chat_add(chat, "user", result_content);
            free(resp);
        }

        cJSON_Delete(entry);
    }
    fclose(f);

    /* Set step counter to resume position */
    ctx->tools->step = saved_step;
    ctx->tools->react_loop = saved_loop;

    fprintf(stderr, "[checkpoint] Restored from step %d (react loop %d)\n",
            saved_step, saved_loop);

    return saved_step;
}

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


/* ── checkpoint ──────────────────────────────────────────── */

/* Save minimal checkpoint after each tool execution.
 * The journal + store contain the actual data; this just records
 * the ephemeral state needed to resume: step, scratchpad, evicted steps. */
static void checkpoint_save(react_ctx_t *ctx, int step, const char *user_query,
                            const char *last_tc_id) {
    char path[4096], tmp_path[4096];
    snprintf(path, sizeof(path), "%s/checkpoint.json", ctx->tools->session_dir);
    snprintf(tmp_path, sizeof(tmp_path), "%s/checkpoint.tmp", ctx->tools->session_dir);

    cJSON *cp = cJSON_CreateObject();
    cJSON_AddNumberToObject(cp, "version", 1);
    cJSON_AddNumberToObject(cp, "step", step);
    cJSON_AddNumberToObject(cp, "react_loop", ctx->tools->react_loop);
    if (user_query) cJSON_AddStringToObject(cp, "user_query", user_query);
    if (ctx->tools->scratchpad)
        cJSON_AddStringToObject(cp, "scratchpad", ctx->tools->scratchpad);
    if (last_tc_id)
        cJSON_AddStringToObject(cp, "last_tc_id", last_tc_id);

    char *json = cJSON_Print(cp);
    FILE *f = fopen(tmp_path, "w");
    if (f) {
        fputs(json, f);
        fclose(f);
        rename(tmp_path, path);  /* atomic write */
    }
    free(json);
    cJSON_Delete(cp);
}

/* Remove checkpoint on successful completion (task done, no resume needed) */
static void checkpoint_remove(react_ctx_t *ctx) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/checkpoint.json", ctx->tools->session_dir);
    unlink(path);
}

/* ── main react loop ─────────────────────────────────── */

char *react_run(react_ctx_t *ctx, const char *user_query,
                react_event_fn on_event, void *userdata) {
    llm_chat_t *chat = llm_chat_new();

    /* Clear alias hash map at the start of every react loop.
     * Aliases start at R<N>S0 for each query. */
    alias_map_clear(ctx->tools->aliases);

    /* Check for checkpoint — resume interrupted task */
    int resume_step = 0;
    resume_step = checkpoint_restore(ctx, chat, user_query);
    int restored = (resume_step >= 0);
    if (restored) {
        fprintf(stderr, "[checkpoint] resuming from step %d\n", resume_step);
    }


    if (!restored) {
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
        char lq[201], lr[501];
        utf8_truncate(lq, ctx->last_query, 200);
        utf8_truncate(lr, ctx->last_result, 500);
        char last_ex[1024];
        snprintf(last_ex, sizeof(last_ex),
                 "[Previous query: \"%s\" → \"%s\"]",
                 lq, lr);
        llm_chat_add(chat, "user", last_ex);
    }

    /* User query */
    llm_chat_add(chat, "user", user_query);

    /* Reset per-loop counters (react_loop is 0-based, incremented at END of loop) */
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
                       strlen(sys_prompt), count_lines(sys_prompt), NULL, NULL);
        cJSON_Delete(sys_p);
        free(sys_hash);

        cJSON *q_p = cJSON_CreateObject();
        cJSON_AddStringToObject(q_p, "text", user_query);
        journal_append(ctx->tools->journal, ctx->tools->react_loop, 0, "query", q_p, NULL,
                       strlen(user_query), 0, NULL, NULL);
        cJSON_Delete(q_p);
    }

    } /* end if (!restored) */
    char *final_result = NULL;
    struct timespec task_start;
    clock_gettime(CLOCK_MONOTONIC, &task_start);

    /* Action signature tracking for cycling detection */
    char last_sigs[8][256];
    int sig_count = 0;

    for (int step = resume_step; ctx->max_steps == 0 || step < ctx->max_steps; step++) {
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

        /* EDRM routing: decide thinking mode before LLM call.
         * On step 0, probe entropy dynamics to determine if CoT is beneficial.
         * Subsequent steps inherit the decision from step 0.
         * See [arXiv:2605.22873] for the theory. */
        if (ctx->tools->cfg && step == resume_step) {
            int mode = ctx->tools->cfg->thinking.mode;
            if (mode == THINKING_ON) {
                ctx->llm->enable_thinking = 1;
            } else if (mode == THINKING_OFF) {
                ctx->llm->enable_thinking = 0;
            } else if (mode == THINKING_EDRM) {
                /* Build probe prompt from user query.
                 * #7: Use /apply-template for correct template, fallback to ChatML. */
                str_t probe = str_new(8192);
                char *templated = llm_apply_template(ctx->llm->api_base, user_query);
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
                    ctx->llm->api_base, probe.data,
                    tc->probe_tokens, tc->probe_n_probs,
                    tc->probe_temperature,
                    tc->tau_rho, tc->tau_vnr, tc->tau_h);
                str_free(&probe);

                ctx->llm->enable_thinking = edrm.route;

                /* Log the routing decision */
                {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                        "EDRM: H̄=%.2f ρ=%.2f VNR=%.2f → %s",
                        edrm.h_mean, edrm.rho_s, edrm.vnr,
                        edrm.route ? "thinking ON" : "thinking OFF");
                    react_event_t ev = {0};
                    ev.type = REACT_EVENT_WARNING;
                    ev.step = step + 1;
                    ev.message = msg;
                    emit(on_event, userdata, &ev);
                }
            }
            /* Propagate thinking budget from config */
            ctx->llm->thinking_budget = ctx->tools->cfg->thinking.budget;
        }

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

            /* Log the invalid response to journal for analysis */
            char *err_hash = store_save(ctx->tools->store, response);
            const char *err_alias = tool_register_alias(ctx->tools,
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

        const char *thought = json_get_str(action, "thought");
        const char *action_name = json_get_str(action, "action");

        if (!action_name) {
            if (thought && thought[0]) {
                /* Thought-only response — the model is reasoning without acting.
                 * This is valid (e.g., deep code analysis, planning).
                 * Log as "thinking" step (not an error), preserve in context. */
                char *think_hash = store_save(ctx->tools->store, response);
                const char *think_alias = tool_register_alias(ctx->tools,
                                            think_hash ? think_hash : "");
                cJSON *think_p = cJSON_CreateObject();
                cJSON_AddStringToObject(think_p, "type", "thinking");
                journal_append(ctx->tools->journal, ctx->tools->react_loop,
                               step + 1, "thinking", think_p, think_alias,
                               strlen(thought), 0, NULL, NULL);
                cJSON_Delete(think_p);
                free(think_hash);

                /* Emit as a step complete (not error) so TUI shows it */
                {
                    react_event_t ev = {0};
                    ev.type = REACT_EVENT_STEP_COMPLETE;
                    ev.step = step + 1;
                    ev.max_steps = ctx->max_steps;
                    ev.step_elapsed = step_elapsed;
                    ev.action = "thinking";
                    ev.description = thought;
                    ev.stats = stats;
                    ev.context_size = ctx->llm->context_size;
                    emit(on_event, userdata, &ev);
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
                ev.type = REACT_EVENT_ERROR;
                ev.step = step + 1;
                ev.message = "No 'action' field in response";
                emit(on_event, userdata, &ev);

                char *err_hash = store_save(ctx->tools->store, response);
                const char *err_alias = tool_register_alias(ctx->tools,
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

        const char *desc = get_action_desc(action, action_name, thought);

        /* Check for done */
        if (strcmp(action_name, "done") == 0) {
            const char *result = json_get_str(action, "result");
            final_result = result ? strdup(result) : strdup("(no result)");
            checkpoint_remove(ctx);

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

            /* Fix 1: Inject warning into chat so the model KNOWS it's cycling */
            llm_chat_add(chat, "user",
                "WARNING: You are repeating the same action. "
                "The output is already stored — use file_read(ref) to read it. "
                "Do NOT re-run the same command.");
        }

        /* Fix 2: Refuse execution after 3+ consecutive identical actions */
        tool_result_t tr;
        if (repeated >= 3) {
            cJSON *err_meta = cJSON_CreateObject();
            cJSON_AddStringToObject(err_meta, "error",
                "Refused: same action repeated 4+ times. "
                "Read previous results with file_read(ref) instead.");
            tr = (tool_result_t){ .meta = err_meta, .store_ref = NULL, .success = 0 };

            journal_append(ctx->tools->journal, ctx->tools->react_loop,
                           step + 1, "cycling_refused", NULL, NULL,
                           0, 0, "same action repeated 4+ times", NULL);
        } else {
            /* Normal execution */
            tr = tool_execute(ctx->tools, action_name, action);
        }

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
        { char _dur[32]; fmt_duration(total_elapsed, _dur, sizeof(_dur));
        snprintf(result_msg, result_len, "%s\n[step %d | %s]",
                 meta_str, step + 1, _dur); }

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
                        free(chat->msgs[i].tool_call_id);    /* #8 */
                        free(chat->msgs[i].tool_calls_json); /* #8 */
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
                        free(chat->msgs[i].tool_call_id);    /* #8 */
                        free(chat->msgs[i].tool_calls_json); /* #8 */
                    }
                    /* Shift tail messages down */
                    int tail_count = chat->n_msgs - evict_end;
                    memmove(&chat->msgs[evict_start], &chat->msgs[evict_end],
                            tail_count * sizeof(llm_msg_t));
                    chat->n_msgs = evict_start + tail_count;

                    /* #9: NULL dangling pointers after eviction */
                    free(chat->last_tool_call_id);
                    chat->last_tool_call_id = NULL;
                    free(chat->last_tool_calls_json);
                    chat->last_tool_calls_json = NULL;

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

        /* Save checkpoint after each tool execution (atomic write) */
        if (!final_result) checkpoint_save(ctx, step + 1, user_query,
                        chat->last_tool_call_id);

        cJSON_Delete(action);
        free(response);
    }

    llm_chat_free(chat);

    /* Remove checkpoint — task completed (successfully or not) */
    checkpoint_remove(ctx);

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

    return final_result;
}
