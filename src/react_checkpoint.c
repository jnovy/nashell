/* react_checkpoint.c — Checkpoint save/restore/remove for crash recovery.
 * Extracted from react.c (P1 decomposition). */
#include "react_internal.h"

/* ── Checkpoint Restore ─────────────────────────────── */

/* Restore conversation state from checkpoint.json + journal.jsonl + .store/
 * Returns the step number to resume from, or -1 if no checkpoint exists.
 * Populates chat with reconstructed messages. */
int react_checkpoint_restore(react_ctx_t *ctx, llm_chat_t *chat,
                             const char *user_query,
                             react_event_fn on_event, void *userdata) {
    char path[NASH_PATH_MAX];
    snprintf(path, sizeof(path), "%s/checkpoint.json",
             ctx->tools->session_dir);

    cJSON *cp = slurp_json(path);
    if (!cp) return -1;  /* no checkpoint — start fresh */

    int saved_step = (int)cJSON_GetNumberValue(
        cJSON_GetObjectItem(cp, "step"));
    int saved_loop = (int)cJSON_GetNumberValue(
        cJSON_GetObjectItem(cp, "react_loop"));

    /* Restore scratchpad (section-based; handles legacy plain-text format too) */
    scratchpad_load(&ctx->tools->scratch, ctx->tools->session_dir);
    if (ctx->tools->scratch.count == 0) {
        /* Try checkpoint JSON as last resort (very old sessions) */
        cJSON *sp = cJSON_GetObjectItem(cp, "scratchpad");
        if (sp && sp->valuestring && sp->valuestring[0]) {
            scratchpad_parse(&ctx->tools->scratch, sp->valuestring,
                             "default", 5);
        }
    }

    /* Restore last_tc_id for tool_calls threading */
    char *restored_tc_id = NULL;
    cJSON *tc_id_j = cJSON_GetObjectItem(cp, "last_tc_id");
    if (tc_id_j && tc_id_j->valuestring)
        restored_tc_id = strdup(tc_id_j->valuestring);

    cJSON_Delete(cp);

    /* Step 1: Add system prompt (fresh — may have changed) */
    react_add_system_prompt(chat, ctx->tools->cfg);

    /* v5: No manifest injection — scratchpad carries all cross-loop state. */

    /* Step 2: Add memory context (fresh) */
    if (ctx->tools->memory) {
        char *mem_summary = memory_build_index(ctx->tools->memory);
        if (mem_summary && strlen(mem_summary) > 0) {
            size_t mem_msg_sz = strlen(mem_summary) + 512;
            char *mem_msg = malloc(mem_msg_sz);
            if (mem_msg) {
                snprintf(mem_msg, mem_msg_sz, "[MEMORY INDEX]\n%s\n\n"
                        "Call memory_recall when the answer may depend on user preferences, "
                        "prior decisions, ongoing projects, or historical context not visible "
                        "in the current conversation.\n"
                        "Use memory_list to browse all keys (optionally filtered by type).", mem_summary);
                llm_chat_add(chat, "user", mem_msg);
                free(mem_msg);
            }
        }

        char *pinned = memory_load_pinned(ctx->tools->memory);
        if (pinned && strlen(pinned) > 0) {
            size_t pin_msg_sz = strlen(pinned) + 64;
            char *pin_msg = malloc(pin_msg_sz);
            if (pin_msg) {
                snprintf(pin_msg, pin_msg_sz, "[PINNED KNOWLEDGE]\n%s", pinned);
                llm_chat_add(chat, "user", pin_msg);
                free(pin_msg);
            }
        }

        /* Log memory context for debugging (checkpoint restore path) */
        react_log_memory_context(ctx->tools, ctx->tools->react_loop,
                           ctx->tools->step, mem_summary, pinned,
                           NULL, user_query);

        free(mem_summary);
        free(pinned);
    }

    /* Step 4: Add scratchpad if exists (section-based or legacy) */
    {
        char *sp_text = NULL;
        if (ctx->tools->scratch.count > 0) {
            sp_text = scratchpad_serialize(&ctx->tools->scratch);
        }
        if (sp_text && sp_text[0]) {
            size_t slen = strlen(sp_text);
            char *scratch_msg = malloc(slen + 32);
            if (scratch_msg) {
                snprintf(scratch_msg, slen + 32, "[SCRATCHPAD]\n%s", sp_text);
                llm_chat_add(chat, "user", scratch_msg);
                free(scratch_msg);
            }
        }
        free(sp_text);
    }

    /* Step 5: Add user query */
    llm_chat_add(chat, "user", user_query);

    /* Step 6: Replay tool calls from journal to rebuild conversation history */
    char jpath[NASH_PATH_MAX];
    snprintf(jpath, sizeof(jpath), "%s/journal.jsonl",
             ctx->tools->session_dir);
    FILE *f = fopen(jpath, "r");
    if (!f) {
        /* Restore last_tc_id even without journal replay */
        if (restored_tc_id) {
            free(chat->last_tool_call_id);
            chat->last_tool_call_id = restored_tc_id;
        }
        return saved_step;
    }

    /* Track highest alias sequence number seen during replay so we can
     * set next_seq after the loop to avoid collisions with restored aliases.
     * Without this, next_seq stays at 0 (reset in react_run) and new
     * tool_register_alias() calls would create R<N>S0, R<N>S1, etc.
     * that overwrite the in-memory map entries for restored aliases. */
    int max_restored_seq = -1;

    char line[NASH_LINE_MAX];
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
            char ref_path[NASH_PATH_MAX];
            snprintf(ref_path, sizeof(ref_path), "%s/%s",
                     ctx->tools->session_dir, ref);
            char link_target[NASH_PATH_MAX];
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

                    /* Track highest seq number to set next_seq after replay.
                     * Alias format: R<loop>S<seq> */
                    int ref_seq = -1;
                    if (sscanf(ref, "R%*dS%d", &ref_seq) == 1 &&
                        ref_seq > max_restored_seq) {
                        max_restored_seq = ref_seq;
                    }
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
                        char *content = slurp_file(store_path, NULL);
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

    /* Advance next_seq past all restored aliases so new tool_register_alias()
     * calls don't collide with existing R<N>S0..R<N>S<max> aliases.
     * The +1 is because next_seq is the NEXT sequence to use. */
    if (max_restored_seq >= 0) {
        ctx->tools->aliases->next_seq = max_restored_seq + 1;
    }

    /* Set step counter to resume position */
    ctx->tools->step = saved_step;
    ctx->tools->react_loop = saved_loop;

    /* Log checkpoint restore to journal */
    {
        cJSON *cp_params = cJSON_CreateObject();
        cJSON_AddNumberToObject(cp_params, "restored_step", saved_step);
        cJSON_AddNumberToObject(cp_params, "react_loop", saved_loop);
        cJSON_AddStringToObject(cp_params, "status", "checkpoint restored");
        char *cp_json = cJSON_PrintUnformatted(cp_params);
        char *cp_hash = store_save(ctx->tools->store, cp_json ? cp_json : "{}");
        char *cp_alias = cp_hash ? tool_register_alias(ctx->tools, cp_hash) : NULL;
        journal_append(ctx->tools->journal, saved_loop, saved_step,
                       "checkpoint_restore", cp_params, cp_alias,
                       cp_json ? strlen(cp_json) : 0, 0, NULL, NULL);
        free(cp_json);
        free(cp_hash);
        free(cp_alias);
        cJSON_Delete(cp_params);
    }

    /* Restore last_tc_id for tool_calls threading after crash */
    if (restored_tc_id) {
        free(chat->last_tool_call_id);
        chat->last_tool_call_id = restored_tc_id;
    }

    /* Emit restore event — include react_loop so UI tracks the correct loop */
    {
        react_event_t ev = {0};
        ev.react_loop = saved_loop;
        ev.type = REACT_EVENT_WARNING;
        ev.step = saved_step;
        ev.message = "Resuming from checkpoint";
        react_emit(on_event, userdata, &ev);
    }

    return saved_step;
}

/* ── Checkpoint Save ────────────────────────────────── */

void react_checkpoint_save(react_ctx_t *ctx, int step, const char *user_query,
                           const char *last_tc_id) {
    char path[NASH_PATH_MAX], tmp_path[NASH_PATH_MAX];
    snprintf(path, sizeof(path), "%s/checkpoint.json", ctx->tools->session_dir);
    snprintf(tmp_path, sizeof(tmp_path), "%s/checkpoint.tmp", ctx->tools->session_dir);

    /* Persist scratchpad to disk alongside checkpoint */
    scratchpad_save(&ctx->tools->scratch, ctx->tools->session_dir);

    cJSON *cp = cJSON_CreateObject();
    cJSON_AddNumberToObject(cp, "version", 1);
    cJSON_AddNumberToObject(cp, "step", step);
    cJSON_AddNumberToObject(cp, "react_loop", ctx->tools->react_loop);
    if (user_query) cJSON_AddStringToObject(cp, "user_query", user_query);
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

/* ── Checkpoint Remove ──────────────────────────────── */

void react_checkpoint_remove(react_ctx_t *ctx) {
    char path[NASH_PATH_MAX];
    snprintf(path, sizeof(path), "%s/checkpoint.json", ctx->tools->session_dir);
    unlink(path);
}
