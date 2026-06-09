#include "react.h"
#include "nash_limits.h"
#include "config.h"
#include "memory.h"
#include "journal.h"
#include "store.h"
#include "nash_log.h"
#include "cJSON.h"
#include "str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <stdint.h>

/* ── helpers ─────────────────────────────────────────── */

static const char *json_get_str(cJSON *obj, const char *key) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item && cJSON_IsString(item)) return item->valuestring;
    return NULL;
}

/* ── reflection deduplication callback ───────────────── */

typedef struct {
    react_ctx_t *ctx;
    cJSON *rkey_j;
    embed_multi_vec_t *new_emb;  /* multi-vec for consistent similarity metric */
    int *should_store;
    int task_succeeded;
} reflection_scan_t;

static int reflection_dedup_cb(const char *dirpath, const char *filename,
                               const char *fullpath, void *user_data) {
    reflection_scan_t *s = (reflection_scan_t *)user_data;
    (void)dirpath; (void)filename;

    /* FIX BUG2: Use multi-vec × multi-vec (MaxSim) similarity, consistent
     * with consolidation_cb in tools.c. Previously used single-vec × multi-vec
     * which computes a different metric, making the 0.90 threshold here and
     * the 0.82 threshold in consolidation incomparable. */
    embed_multi_vec_t exist_emb = embed_multi_vec_load(fullpath);
    if (!exist_emb.data) return 0;
    if (exist_emb.dim != s->new_emb->dim) {
        embed_multi_vec_free(&exist_emb);
        return 0;
    }
    float sim = embed_cosine_sim_multi_multi(s->new_emb, &exist_emb);
    embed_multi_vec_free(&exist_emb);
    if (sim > 0.90f) {
        /* When a task FAILED, the reflection may produce a corrective insight
         * that contradicts an existing entry. Since contradictions have high
         * embedding similarity (same topic, opposite conclusion), we must
         * allow the store — memory_try_consolidate will classify it as
         * SUPERSEDES and delete the old entry. Only block for successes. */
        if (!s->task_succeeded) {
            /* Log but allow — let consolidation handle contradiction */
            cJSON *dup_p = cJSON_CreateObject();
            cJSON_AddStringToObject(dup_p, "key", s->rkey_j->valuestring);
            cJSON_AddNumberToObject(dup_p, "similarity", (double)sim);
            cJSON_AddStringToObject(dup_p, "action", "allowed_failure_correction");
            journal_append(s->ctx->tools->journal,
                s->ctx->tools->react_loop, s->ctx->tools->step,
                "reflection_dedup", dup_p, NULL, 0, 0, NULL, NULL);
            cJSON_Delete(dup_p);
            return 1;  /* stop iterating, but should_store stays 1 */
        }
        *s->should_store = 0;
        /* Log to journal instead of stderr (TUI mode) */
        {
            cJSON *dup_p = cJSON_CreateObject();
            cJSON_AddStringToObject(dup_p, "key", s->rkey_j->valuestring);
            cJSON_AddNumberToObject(dup_p, "similarity", (double)sim);
            cJSON_AddStringToObject(dup_p, "action", "skipped");
            journal_append(s->ctx->tools->journal,
                s->ctx->tools->react_loop, s->ctx->tools->step,
                "reflection_dedup", dup_p, NULL, 0, 0, NULL, NULL);
            cJSON_Delete(dup_p);
        }
        return 1;  /* stop iterating */
    }
    return 0;
}

/* Log memory context injection to the journal for debugging.
 * Captures: index summary (total + type counts), pinned keys, skills recalled.
 * Called at both the checkpoint-restore path and the main react_run path. */
static void log_memory_context(tool_ctx_t *tools, int react_loop, int step,
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
static void sanitize_thought(cJSON *action) {
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
    char path[NASH_PATH_MAX];
    snprintf(path, sizeof(path), "%s/checkpoint.json",
             ctx->tools->session_dir);

    char *buf = slurp_file(path, NULL);
    if (!buf) return -1;  /* no checkpoint — start fresh */

    cJSON *cp = cJSON_Parse(buf);
    free(buf);
    if (!cp) return -1;

    int saved_step = (int)cJSON_GetNumberValue(
        cJSON_GetObjectItem(cp, "step"));
    int saved_loop = (int)cJSON_GetNumberValue(
        cJSON_GetObjectItem(cp, "react_loop"));

    /* Restore scratchpad — try section-based first, fall back to legacy string */
    scratchpad_load(&ctx->tools->scratch, ctx->tools->session_dir);
    if (ctx->tools->scratch.count > 0) {
        /* Sections loaded from disk — regenerate legacy string */
        free(ctx->tools->scratchpad);
        ctx->tools->scratchpad = scratchpad_serialize(&ctx->tools->scratch);
    } else {
        /* Fall back to checkpoint string (legacy format) */
        cJSON *sp = cJSON_GetObjectItem(cp, "scratchpad");
        if (sp && sp->valuestring && sp->valuestring[0]) {
            free(ctx->tools->scratchpad);
            ctx->tools->scratchpad = strdup(sp->valuestring);
        }
    }

    /* Restore last_tc_id for tool_calls threading */
    char *restored_tc_id = NULL;
    cJSON *tc_id_j = cJSON_GetObjectItem(cp, "last_tc_id");
    if (tc_id_j && tc_id_j->valuestring)
        restored_tc_id = strdup(tc_id_j->valuestring);

    /* Restore aliases from journal symlinks */
    /* (aliases are re-derived from journal refs below) */

    cJSON_Delete(cp);

    /* Step 1: Add system prompt (fresh — may have changed) */
    llm_chat_add(chat, "system", tools_system_prompt());

    /* v5: No manifest injection — scratchpad carries all cross-loop state. */

    /* Step 2: Add memory context (fresh) */
    if (ctx->tools->memory) {
        char *mem_summary = memory_build_index(ctx->tools->memory);
        if (mem_summary && strlen(mem_summary) > 0) {
            char *mem_msg = malloc(strlen(mem_summary) + 256);
            if (mem_msg) {
                sprintf(mem_msg, "[MEMORY INDEX]\n%s\n\nUse memory_recall with a query to search your memory store.\n"
                        "Use memory_list to browse all keys (optionally filtered by type).", mem_summary);
                llm_chat_add(chat, "user", mem_msg);
                free(mem_msg);
            }
        }

        char *pinned = memory_load_pinned(ctx->tools->memory);
        if (pinned && strlen(pinned) > 0) {
            char *pin_msg = malloc(strlen(pinned) + 64);
            if (pin_msg) {
                sprintf(pin_msg, "[PINNED KNOWLEDGE]\n%s", pinned);
                llm_chat_add(chat, "user", pin_msg);
                free(pin_msg);
            }
        }

        /* Log memory context for debugging (checkpoint restore path) */
        log_memory_context(ctx->tools, ctx->tools->react_loop,
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
        } else if (ctx->tools->scratchpad && ctx->tools->scratchpad[0]) {
            sp_text = strdup(ctx->tools->scratchpad);
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

    /* Log checkpoint restore to journal (not stderr — corrupts TUI) */
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
    char path[NASH_PATH_MAX], tmp_path[NASH_PATH_MAX];
    snprintf(path, sizeof(path), "%s/checkpoint.json", ctx->tools->session_dir);
    snprintf(tmp_path, sizeof(tmp_path), "%s/checkpoint.tmp", ctx->tools->session_dir);

    /* FIX #7: Persist section-based scratchpad alongside checkpoint */
    if (ctx->tools->scratch.count > 0) {
        scratchpad_save(&ctx->tools->scratch, ctx->tools->session_dir);
    }

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
    char path[NASH_PATH_MAX];
    snprintf(path, sizeof(path), "%s/checkpoint.json", ctx->tools->session_dir);
    unlink(path);
}

/* Read the original user_query from a checkpoint without restoring full state.
 * Used by /continue to resume with the original query instead of "continue".
 * Returns heap-allocated string or NULL if no checkpoint. Caller frees. */
char *checkpoint_read_query(const char *session_dir) {
    if (!session_dir) return NULL;
    char path[NASH_PATH_MAX];
    snprintf(path, sizeof(path), "%s/checkpoint.json", session_dir);
    char *buf = slurp_file(path, NULL);
    if (!buf) return NULL;
    cJSON *cp = cJSON_Parse(buf);
    free(buf);
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
static char *extract_llm_text_output(const char *raw) {
    if (!raw || !raw[0]) return NULL;

    /* Skip leading whitespace */
    while (*raw == ' ' || *raw == '\n' || *raw == '\r' || *raw == '\t') raw++;
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
    resume_step = checkpoint_restore(ctx, chat, user_query);
    int restored = (resume_step >= 0);
    if (restored) {
        /* Emit event instead of fprintf — TUI will display it */
        react_event_t ev = {0};
        ev.type = REACT_EVENT_WARNING;
        ev.step = resume_step;
        ev.message = "Resuming from checkpoint";
        emit(on_event, userdata, &ev);
    }


    if (!restored) {
    /* Reset per-loop counters FIRST — before any journal logging that uses step.
     * Previously this was done after memory injection, causing memory_context
     * journal entries to inherit the step value from the previous react loop. */
    ctx->tools->step = 0;

    /* System message */
    llm_chat_add(chat, "system", tools_system_prompt());

    /* v5: No manifest injection — scratchpad is the sole persistence mechanism.
     * Cross-loop state is carried via scratchpad (auto-saved done results +
     * LLM-pruned summaries). Within-loop recovery uses LLM summarization
     * instead of manifest re-injection. */

    /* Inject memory summary (counts only — no alphabetical listing) */
    if (ctx->flags.inject_memory && ctx->tools->memory) {
        char *mem_summary = memory_build_index(ctx->tools->memory);
        if (mem_summary && strlen(mem_summary) > 0) {
            char *mem_msg = malloc(strlen(mem_summary) + 256);
            if (mem_msg) {
                sprintf(mem_msg, "[MEMORY INDEX]\n%s\n\nUse memory_recall with a query to search your memory store.\n"
                        "Use memory_list to browse all keys (optionally filtered by type).", mem_summary);
                llm_chat_add(chat, "user", mem_msg);
                free(mem_msg);
            }
        }

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

        /* Inject relevant memories by type — semantic recall filtered by prefix.
         * FIX B1/D1: Use user_query for semantic recall, then filter by type prefix.
         * Previously used "skill:" as the query, which matched ALL skills by type
         * prefix rather than finding skills semantically relevant to the task.
         * Now recalls skills, lessons, strategies, and anti-patterns separately.
         * Each type has its own limit to control context budget. */
        int max_skills = ctx->tools->cfg ? ctx->tools->cfg->max_skills_per_query : 3;
        int max_lessons = ctx->tools->cfg ? ctx->tools->cfg->max_lessons_per_query : 2;
        int max_strategies = ctx->tools->cfg ? ctx->tools->cfg->max_strategies_per_query : 2;
        int max_antipatterns = ctx->tools->cfg ? ctx->tools->cfg->max_antipatterns_per_query : 1;
        /* Request enough candidates to cover all types after filtering */
        int max_candidates = (max_skills + max_lessons + max_strategies + max_antipatterns) * 3;

        /* Build enriched recall query: user_query + scratchpad content.
         * The scratchpad carries accumulated working memory across loops,
         * so including it helps retrieve memories relevant to the current
         * task context, not just the raw user query. memory_recall handles
         * capacity overflow via multi-vec chunking internally. */
        str_t recall_query = str_new(1024);
        str_append_cstr(&recall_query, user_query);
        if (ctx->tools->scratch.count > 0) {
            char *sp_text = scratchpad_serialize_budget(&ctx->tools->scratch, SIZE_MAX);
            if (sp_text && sp_text[0]) {
                str_append_cstr(&recall_query, "\n");
                str_append_cstr(&recall_query, sp_text);
            }
            free(sp_text);
        }
        memory_results_t all_memories = memory_recall(ctx->tools->memory, str_cstr(&recall_query), max_candidates);
        str_free(&recall_query);

        /* Helper macro: inject entries of a given type prefix */
        // NOLINTNEXTLINE(bugprone-macro-parentheses)
        #define INJECT_TYPE(label, prefix, plen, max_count, type_count) \
            do { \
                if (type_count > 0) { \
                    str_t msg = str_new(4096); \
                    str_appendf(&msg, "%s\n", label); \
                    for (int j = 0; j < all_memories.count; j++) { \
                        if (all_memories.entries[j].key && \
                            strncmp(all_memories.entries[j].key, prefix, plen) == 0) { \
                            str_appendf(&msg, "\n--- %s ---\n%s\n", \
                                all_memories.entries[j].key, \
                                all_memories.entries[j].value ? all_memories.entries[j].value : ""); \
                            tool_track_recalled_key(ctx->tools, all_memories.entries[j].key); \
                            type_count--; \
                        } \
                        if (type_count <= 0) break; \
                    } \
                    if (msg.len > strlen(label) + 5) { \
                        llm_chat_add(chat, "user", str_cstr(&msg)); \
                    } \
                    str_free(&msg); \
                } \
            } while(0)

        INJECT_TYPE("[RELEVANT SKILLS]", "skill:", 6, max_skills, max_skills);
        INJECT_TYPE("[RELEVANT LESSONS]", "lesson:", 7, max_lessons, max_lessons);
        INJECT_TYPE("[RELEVANT STRATEGIES]", "strategy:", 10, max_strategies, max_strategies);
        INJECT_TYPE("[RELEVANT ANTI-PATTERNS]", "anti-pattern:", 13, max_antipatterns, max_antipatterns);

        #undef INJECT_TYPE

        /* Log memory context for debugging — before freeing mem_summary/pinned */
        log_memory_context(ctx->tools, ctx->tools->react_loop,
                           ctx->tools->step, mem_summary, pinned,
                           &all_memories, user_query);

        free(mem_summary);
        free(pinned);
        memory_results_free(&all_memories);
    }

    /* v5: Scratchpad budget = 15% of context size, no min/max caps.
     * The scratchpad is the SOLE cross-loop persistence mechanism, so it
     * gets a generous budget. All limits scale linearly with context_size. */
    size_t max_scratchpad = 8192;  /* fallback if context_size unknown */
    if (ctx->llm->context_size > 0) {
        max_scratchpad = (size_t)ctx->llm->context_size * 4 * 15 / 100;  /* 15% in chars (~4 chars/tok) */
    }

    /* Inject scratchpad if exists (budget-aware, priority-ordered) */
    {
        char *serialized = NULL;
        if (ctx->tools->scratch.count > 0) {
            /* Use budget-aware serialization: high-priority sections first */
            serialized = scratchpad_serialize_budget(&ctx->tools->scratch, max_scratchpad);
        } else if (ctx->tools->scratchpad && ctx->tools->scratchpad[0]) {
            /* Legacy fallback */
            size_t slen = strlen(ctx->tools->scratchpad);
            if (slen > max_scratchpad) slen = max_scratchpad;
            serialized = malloc(slen + 1);
            if (serialized) { memcpy(serialized, ctx->tools->scratchpad, slen); serialized[slen] = '\0'; }
        }
        if (serialized && serialized[0]) {
            size_t slen = strlen(serialized);
            char *scratch_msg = malloc(slen + 32);
            if (scratch_msg) {
                snprintf(scratch_msg, slen + 32, "[SCRATCHPAD]\n%s", serialized);
                llm_chat_add(chat, "user", scratch_msg);
                free(scratch_msg);
            }
            free(serialized);
        } else {
            free(serialized);
        }
    }

    /* Inject previous result — loaded from session_dir/result.txt.
     * This survives post-reflection scratchpad pruning (which removes
     * resolved info including the R<N>_result section). Provides a
     * reliable cross-loop fallback so user follow-ups can reference
     * the previous task's output. */
    if (ctx->flags.inject_prev_result) {
        char rpath[NASH_PATH_MAX];
        snprintf(rpath, sizeof(rpath), "%s/result.txt", ctx->tools->session_dir);
        char *prev_result = slurp_file(rpath, NULL);
        if (prev_result && strlen(prev_result) > 0) {
            /* Include the full previous result — no truncation.
             * The LLM needs the complete output to make informed decisions
             * about follow-up queries. */
            size_t rlen = strlen(prev_result);
            char *prev_msg = malloc(rlen + 128);
            if (prev_msg) {
                snprintf(prev_msg, rlen + 128,
                    "[PREVIOUS RESULT]\n%s\n"
                    "The above is the result of the previous task. "
                    "You can reference it for follow-up queries.",
                    prev_result);
                llm_chat_add(chat, "user", prev_msg);
                free(prev_msg);
            }
            free(prev_result);
        }
    }

    /* User query */
    llm_chat_add(chat, "user", user_query);

    /* Record system prompt and user query in journal (step 0) */
    {
        const char *sys_prompt = tools_system_prompt();
        char *sys_hash = store_save(ctx->tools->store, sys_prompt);
        /* Register alias for system prompt (function auto-generates S0, S1, ...) */
        char *sys_alias = tool_register_alias(ctx->tools, sys_hash);
        cJSON *sys_p = cJSON_CreateObject();
        cJSON_AddStringToObject(sys_p, "type", "system_prompt");
        journal_append(ctx->tools->journal, ctx->tools->react_loop, 0, "system", sys_p, sys_alias,
                       strlen(sys_prompt), count_lines(sys_prompt), NULL, NULL);
        cJSON_Delete(sys_p);
        free(sys_alias);
        free(sys_hash);

        cJSON *q_p = cJSON_CreateObject();
        cJSON_AddStringToObject(q_p, "text", user_query);
        char *q_hash = store_save(ctx->tools->store, user_query);
        char *q_alias = q_hash ? tool_register_alias(ctx->tools, q_hash) : NULL;
        journal_append(ctx->tools->journal, ctx->tools->react_loop, 0, "query", q_p, q_alias,
                       strlen(user_query), 0, NULL, NULL);
        cJSON_Delete(q_p);
        free(q_alias);
        free(q_hash);
    }

    /* Log the full initial LLM context (all messages) as a single
     * journal entry so the TUI can show exactly what the LLM received. */
    {
        char *ctx_text = llm_chat_serialize(chat);
        if (ctx_text && ctx_text[0]) {
            char *ctx_hash = store_save(ctx->tools->store, ctx_text);
            char *ctx_alias = ctx_hash ? tool_register_alias(ctx->tools, ctx_hash) : NULL;
            cJSON *ctx_p = cJSON_CreateObject();
            cJSON_AddNumberToObject(ctx_p, "n_messages", chat->n_msgs);
            journal_append(ctx->tools->journal, ctx->tools->react_loop, 0,
                           "context", ctx_p, ctx_alias,
                           strlen(ctx_text), count_lines(ctx_text), NULL, NULL);
            cJSON_Delete(ctx_p);
            free(ctx_alias);
            free(ctx_hash);
        }
        free(ctx_text);
    }

    } /* end if (!restored) */
    char *final_result = NULL;
    struct timespec task_start;
    clock_gettime(CLOCK_MONOTONIC, &task_start);

    /* Action signature tracking for cycling detection */
    char last_sigs[8][512];
    int sig_count = 0;
    int consecutive_null_responses = 0;  /* Track LLM failures (HTTP 500 etc.) */
    int total_400_errors = 0;            /* Track HTTP 400 errors (never reset) */

    for (int step = resume_step; ctx->max_steps == 0 || step < ctx->max_steps; step++) {
        ctx->tools->step = step + 1;
        nash_log_set_context(ctx->tools->react_loop, step + 1);

        /* Emit step start */
        {
            react_event_t ev = {0};
            ev.type = REACT_EVENT_STEP_START;
            ev.step = step + 1;
            ev.max_steps = ctx->max_steps;
            ev.context_size = ctx->llm->context_size;
            emit(on_event, userdata, &ev);
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
                ctx->llm->enable_thinking = 1;
                ctx->provider->cfg.enable_thinking = 1;
            } else if (mode == THINKING_OFF) {
                ctx->llm->enable_thinking = 0;
                ctx->provider->cfg.enable_thinking = 0;
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
                ctx->provider->cfg.enable_thinking = edrm.route;

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
            ctx->provider->cfg.thinking_budget = ctx->tools->cfg->thinking.budget;
        }

        llm_stats_t stats = {0};
        stream_ctx_t sctx = { on_event, userdata, step + 1 };
        int max_resp = ctx->tools->cfg ? ctx->tools->cfg->llm_max_response : 10*1024*1024;
        int rep_thresh = ctx->tools->cfg ? ctx->tools->cfg->llm_repeat_threshold : 100;
        char *response = provider_complete_stream(ctx->provider, chat, &stats,
                on_event ? stream_token_cb : NULL, &sctx,
                max_resp, rep_thresh);
        if (!response) {
            consecutive_null_responses++;

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
                    else if (ctx->llm->last_error)
                        srv_err = ctx->llm->last_error;

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

                /* Capture context size for diagnostics */
                int total_chars = 0;
                for (int ci = 0; ci < chat->n_msgs; ci++)
                    total_chars += (int)strlen(chat->msgs[ci].content);
                cJSON_AddNumberToObject(err_params, "context_chars", total_chars);
                cJSON_AddNumberToObject(err_params, "context_msgs", chat->n_msgs);

                /* Include the actual server error message if available.
                 * Check both llm_config_t (direct llm path) and provider_t
                 * (provider path) for error details. */
                const char *err_msg = ctx->llm->last_error;
                const char *err_req = ctx->llm->last_error_request;
                const char *err_resp = ctx->llm->last_error_response;

                /* Provider path: if provider was used, check its error fields */
                if (ctx->provider) {
                    if (!err_msg && ctx->provider->last_error)
                        err_msg = ctx->provider->last_error;
                    if (!err_req && ctx->provider->last_error_request)
                        err_req = ctx->provider->last_error_request;
                    if (!err_resp && ctx->provider->last_error_response)
                        err_resp = ctx->provider->last_error_response;
                }

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
                    emit(on_event, userdata, &ev);
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
                        emit(on_event, userdata, &ev);
                        break;
                    }
                    /* Aggressive eviction: remove half of middle messages */
                    int keep_head = 3;
                    int keep_tail = 4;
                    int evict_start = keep_head;
                    int evict_end = chat->n_msgs - keep_tail;
                    if (evict_end > evict_start + 2) {
                        /* Evict the older half of the evictable range */
                        int mid = evict_start + (evict_end - evict_start) / 2;
                        int n_evict = mid - evict_start;
                        for (int i = evict_start; i < mid; i++) {
                            free(chat->msgs[i].role);
                            free(chat->msgs[i].content);
                            free(chat->msgs[i].tool_call_id);
                            free(chat->msgs[i].tool_calls_json);
                        }
                        memmove(&chat->msgs[evict_start],
                                &chat->msgs[mid],
                                (chat->n_msgs - mid) * sizeof(llm_msg_t));
                        chat->n_msgs -= n_evict;
                        char emsg[128];
                        snprintf(emsg, sizeof(emsg),
                            "HTTP 400 — evicted %d messages to reduce context "
                            "(attempt %d/6)", n_evict, total_400_errors);
                        ev.message = emsg;
                        emit(on_event, userdata, &ev);
                    }
                    /* HTTP 400 is a client error (context too large), not a
                     * transient server error. Don't let it poison the HTTP 500
                     * retry tier counter — otherwise N recoverable 400s would
                     * exhaust the 500-recovery budget. */
                    consecutive_null_responses = 0;
                    continue;
                }
            }

            /* 3-tier retry strategy for HTTP 500 / NULL responses.
             * Each tier addresses a different root cause:
             *   Tier 1: Remove last assistant+tool_result pair (model confusion)
             *   Tier 2: Reformulate scratchpad (context pollution)
             *   Tier 3: Strip scratchpad entirely (nuclear option)
             *   Tier 4+: Give up */
            if (consecutive_null_responses >= 4) {
                ev.message = "LLM server error — all recovery tiers exhausted, giving up";
                emit(on_event, userdata, &ev);
                break;
            }

            if (consecutive_null_responses == 1) {
                /* Tier 1: Remove the last assistant+tool_result pair.
                 * The model's previous output was likely malformed (e.g.,
                 * "shell_execshell_exec"). Removing it gives the model a
                 * clean slate to regenerate from the previous context. */
                ev.message = "LLM server error — removing last exchange and retrying (tier 1)";
                emit(on_event, userdata, &ev);

                /* Remove last 2 messages (assistant + tool_result) if they exist */
                if (chat->n_msgs >= 2) {
                    for (int r = 0; r < 2 && chat->n_msgs > 3; r++) {
                        int last = chat->n_msgs - 1;
                        free(chat->msgs[last].role);
                        free(chat->msgs[last].content);
                        free(chat->msgs[last].tool_call_id);
                        free(chat->msgs[last].tool_calls_json);
                        chat->n_msgs--;
                    }
                }
            } else if (consecutive_null_responses == 2) {
                /* Tier 2: Reformulate scratchpad into plain prose.
                 * Code blocks and JSON in the scratchpad can confuse
                 * the model's JSON generation. */
                int sp_idx = -1;
                for (int i = 0; i < chat->n_msgs; i++) {
                    if (chat->msgs[i].content &&
                        strncmp(chat->msgs[i].content, "[SCRATCHPAD]", 12) == 0) {
                        sp_idx = i;
                        break;
                    }
                }

                if (sp_idx >= 0) {
                    ev.message = "LLM server error — reformulating scratchpad (tier 2)";
                    emit(on_event, userdata, &ev);

                    llm_chat_t *rewrite = llm_chat_new();
                    llm_chat_add(rewrite, "system",
                        "You are a text sanitiser. Rewrite the user's notes "
                        "into plain prose. Remove ALL code blocks, JSON "
                        "snippets, backtick-fenced sections, and deeply "
                        "escaped strings. Keep the semantic meaning and key "
                        "facts but express everything in simple sentences. "
                        "Output ONLY the rewritten text, nothing else.");
                    llm_chat_add(rewrite, "user", chat->msgs[sp_idx].content + 13);

                    char *reformulated = provider_complete(ctx->provider, rewrite, NULL);
                    llm_chat_free(rewrite);

                    if (reformulated && strlen(reformulated) > 0) {
                        size_t rlen = strlen(reformulated);
                        char *new_sp = malloc(rlen + 32);
                        if (new_sp) {
                            snprintf(new_sp, rlen + 32, "[SCRATCHPAD]\n%s",
                                     reformulated);
                            free(chat->msgs[sp_idx].content);
                            chat->msgs[sp_idx].content = new_sp;
                        }
                        free(reformulated);
                    } else {
                        free(reformulated);
                    }
                } else {
                    ev.message = "LLM server error — no scratchpad, skipping tier 2";
                    emit(on_event, userdata, &ev);
                }
            } else if (consecutive_null_responses == 3) {
                /* Tier 3: Strip scratchpad entirely (nuclear option).
                 * If reformulation didn't help, the scratchpad itself
                 * may be the problem. Remove it completely. */
                ev.message = "LLM server error — stripping scratchpad entirely (tier 3)";
                emit(on_event, userdata, &ev);

                for (int i = 0; i < chat->n_msgs; i++) {
                    if (chat->msgs[i].content &&
                        strncmp(chat->msgs[i].content, "[SCRATCHPAD]", 12) == 0) {
                        free(chat->msgs[i].role);
                        free(chat->msgs[i].content);
                        free(chat->msgs[i].tool_call_id);
                        free(chat->msgs[i].tool_calls_json);
                        memmove(&chat->msgs[i], &chat->msgs[i + 1],
                                (chat->n_msgs - i - 1) * sizeof(llm_msg_t));
                        chat->n_msgs--;
                        break;
                    }
                }
            }
            continue;
        }
        consecutive_null_responses = 0;  /* Reset on successful LLM response */

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
        sanitize_thought(action);

        const char *thought = json_get_str(action, "thought");
        const char *action_name = json_get_str(action, "action");

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

        const char *desc = get_action_desc(action, action_name, thought);

        /* Check for user_ask — pause react loop and wait for user input */
        if (strcmp(action_name, "user_ask") == 0) {
            const char *question = json_get_str(action, "question");
            if (!question || !question[0]) question = "(no question specified)";

            /* Store question in shared state for TUI to read */
            free(ctx->user_ask_question);
            ctx->user_ask_question = strdup(question);
            free(ctx->user_ask_answer);
            ctx->user_ask_answer = NULL;
            ctx->user_ask_pending = 1;

            /* Emit event so TUI shows the question */
            react_event_t ev = {0};
            ev.type = REACT_EVENT_USER_ASK;
            ev.step = step + 1;
            ev.message = question;
            emit(on_event, userdata, &ev);

            /* Poll until TUI provides the answer (set by main.c) */
            while (ctx->user_ask_pending) {
                { struct timespec ts = {0, 100000000}; nanosleep(&ts, NULL); }  /* 100ms */
            }

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
            const char *result = json_get_str(action, "result");
            /* Fallback: if result is empty but thought has content, use thought.
             * Local models sometimes put the summary in "thought" and leave
             * "result" empty — the thought IS the answer for done calls. */
            if ((!result || !result[0]) && thought && thought[0]) {
                result = thought;
            }
            final_result = result ? strdup(result) : strdup("(no result)");
            checkpoint_remove(ctx);

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
            {
                char rpath[NASH_PATH_MAX];
                snprintf(rpath, sizeof(rpath), "%s/result.txt",
                         ctx->tools->session_dir);
                FILE *rf = fopen(rpath, "w");
                if (rf) {
                    fputs(final_result, rf);
                    fclose(rf);
                }
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

        /* Cycling detection — disabled by default, enable via config:
         *   [limits]
         *   cycling_detection = true
         * When disabled, the model can repeat the same action without
         * warnings or refusal. This is useful for tasks that legitimately
         * require repeated operations (e.g., reading multiple sections
         * of the same file, running similar commands). */
        int cycling_enabled = ctx->tools->cfg ? ctx->tools->cfg->cycling_detection : 0;
        char sig[512];
        const char *cmd = json_get_str(action, "command");
        const char *path = json_get_str(action, "path");
        const char *pattern = json_get_str(action, "pattern");
        const char *content = json_get_str(action, "content");
        const char *old_text = json_get_str(action, "old_text");
        const char *new_text = json_get_str(action, "new_text");
        const char *query = json_get_str(action, "query");
        const char *question = json_get_str(action, "question");
        const char *url = json_get_str(action, "url");
        /* Include start_line/end_line in signature so that reading different
         * line ranges of the same file is NOT detected as cycling.
         * file_read("react.c", 1, 50) and file_read("react.c", 50, 100)
         * are different actions, not repetitions. */
        cJSON *sl = cJSON_GetObjectItem(action, "start_line");
        cJSON *el = cJSON_GetObjectItem(action, "end_line");
        int start_line = sl ? (int)cJSON_GetNumberValue(sl) : 0;
        int end_line = el ? (int)cJSON_GetNumberValue(el) : 0;
        /* Build signature from all action-distinguishing parameters.
         * Truncate long fields (content, old_text, new_text) to keep sig bounded. */
        char content_prefix[32] = "", old_prefix[32] = "", new_prefix[32] = "";
        if (content) snprintf(content_prefix, sizeof(content_prefix), "%.30s", content);
        if (old_text) snprintf(old_prefix, sizeof(old_prefix), "%.30s", old_text);
        if (new_text) snprintf(new_prefix, sizeof(new_prefix), "%.30s", new_text);
        snprintf(sig, sizeof(sig), "%s:%s:%s:%s:%d:%d:%s:%s:%s:%s:%s:%s",
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
                 url ? url : "");

        int repeated = 0;
        for (int i = 0; i < sig_count && i < 8; i++) {
            if (strcmp(last_sigs[i], sig) == 0) repeated++;
        }
        if (sig_count < 8) {
            snprintf(last_sigs[sig_count], 512, "%s", sig);
            sig_count++;
        } else {
            memmove(last_sigs, last_sigs + 1, 7 * 512);
            snprintf(last_sigs[7], 512, "%s", sig);
        }

        if (cycling_enabled && repeated >= 2) {
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
        if (cycling_enabled && repeated >= 3) {
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
                    char correction[1024];
                    if (ut_alias) {
                        snprintf(correction, sizeof(correction),
                            "ERROR: '%s' is not a valid tool. "
                            "Stored garbled call for debugging: file_read(\"%s\"). "
                            "Available tools: shell_exec, file_read, file_write, "
                            "file_edit, grep_search, web_fetch, web_search, notes, "
                            "done, memory_store, memory_recall, memory_pin, "
                            "memory_unpin, memory_delete. "
                            "Please retry with the correct tool name.",
                            action_name, ut_alias);
                    } else {
                        snprintf(correction, sizeof(correction),
                            "ERROR: '%s' is not a valid tool. "
                            "Available tools: shell_exec, file_read, file_write, "
                            "file_edit, grep_search, web_fetch, web_search, notes, "
                            "done, memory_store, memory_recall, memory_pin, "
                            "memory_unpin, memory_delete. "
                            "Please retry with the correct tool name.",
                            action_name);
                    }

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
        if (ctx->flags.enable_compaction && ctx->llm->context_size > 0) {
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

                /* If still over threshold, do standard eviction */
                int keep_head = 3;
                int keep_tail = 4;
                int evict_start = keep_head;
                int evict_end = chat->n_msgs - keep_tail;

                /* FIX B5: Adjust eviction boundary to not split tool_call/tool_result pairs.
                 * A pair is: assistant(tool_calls_json) immediately followed by
                 * tool_result(tool_call_id). Never evict one without the other. */
                if (evict_end > evict_start && evict_end < chat->n_msgs) {
                    /* Case 1: Boundary lands between assistant and its tool_result.
                     * assistant(tool_calls) at evict_end-1, tool_result at evict_end.
                     * Keep both by moving boundary back one. */
                    if (evict_end - 1 >= evict_start &&
                        chat->msgs[evict_end - 1].tool_calls_json &&
                        evict_end < chat->n_msgs &&
                        chat->msgs[evict_end].tool_call_id) {
                        evict_end--;  /* keep the pair in tail */
                    }
                    /* Case 2: Boundary lands on assistant with tool_calls_json, and
                     * the next message is its tool_result. Move boundary forward to
                     * evict both, keeping the pair together. */
                    if (evict_end < chat->n_msgs &&
                        chat->msgs[evict_end].tool_calls_json &&
                        evict_end + 1 < chat->n_msgs &&
                        chat->msgs[evict_end + 1].tool_call_id) {
                        evict_end++;  /* evict both assistant and its tool_result */
                    }
                    /* Case 3: Boundary lands on a tool_result whose assistant was
                     * already evicted (or is before evict_start). Include this
                     * orphaned tool_result in the eviction. */
                    if (evict_end < chat->n_msgs && chat->msgs[evict_end].tool_call_id &&
                        (evict_end <= evict_start || !chat->msgs[evict_end - 1].tool_calls_json)) {
                        evict_end++;  /* evict the orphaned tool_result */
                    }
                }

                if (usage_pct > (ctx->tools->cfg ? ctx->tools->cfg->context_eviction_pct : 70) && evict_end > evict_start) {
                    /* v5: LLM-based semantic summarization replaces destructive eviction.
                     * Instead of deleting messages and re-injecting a navigation manifest,
                     * we ask the LLM to summarize the evicted messages and merge the
                     * summary into the scratchpad — preserving semantic knowledge. */

                    /* Step 1: Collect evicted messages into a single string for summarization */
                    str_t evicted_text = str_new(4096);
                    for (int i = evict_start; i < evict_end; i++) {
                        if (chat->msgs[i].content && chat->msgs[i].content[0]) {
                            str_appendf(&evicted_text, "[%s]: %s\n\n",
                                        chat->msgs[i].role ? chat->msgs[i].role : "?",
                                        chat->msgs[i].content);
                        }
                    }

                    /* Step 2: LLM summarization call */
                    char *summary = NULL;
                    if (evicted_text.len > 0) {
                        size_t sp_budget = (size_t)ctx->llm->context_size * 4 * 15 / 100;
                        char *current_sp = (ctx->tools->scratch.count > 0)
                            ? scratchpad_serialize(&ctx->tools->scratch) : strdup("");

                        str_t summ_prompt = str_new(evicted_text.len + 2048);
                        str_appendf(&summ_prompt,
                            "Summarize a portion of an agentic work session being "
                            "evicted from context to free space.\n"
                            "Extract ALL key findings, decisions, file paths, code changes, "
                            "errors, and conclusions.\n"
                            "Preserve specific details (line numbers, variable names, exact "
                            "error messages).\n"
                            "Omit tool call mechanics and navigation steps.\n\n"
                            "Messages being evicted:\n---\n%s\n---\n\n"
                            "Current scratchpad:\n---\n%s\n---\n\n"
                            "Write a MERGED scratchpad combining existing content with key "
                            "findings from evicted messages.\n"
                            "Use structured sections with ## headers.\n"
                            "Keep under %d characters.\n",
                            str_cstr(&evicted_text), current_sp, (int)sp_budget);
                        free(current_sp);

                        llm_chat_t *summ_chat = llm_chat_new();
                        llm_chat_add(summ_chat, "user", str_cstr(&summ_prompt));
                        char *raw_summary = provider_complete(ctx->provider, summ_chat, NULL);
                        llm_chat_free(summ_chat);
                        str_free(&summ_prompt);

                        /* Extract text from LLM output — accepts both plain markdown
                         * and JSON tool-call format (extracts "content" field). */
                        summary = extract_llm_text_output(raw_summary);
                        free(raw_summary);
                        if (summary) {
                            scratchpad_write(&ctx->tools->scratch, "context_summary",
                                             summary, 0);  /* priority 0 = highest */
                            scratchpad_save(&ctx->tools->scratch, ctx->tools->session_dir);
                        }
                        free(summary);
                    }
                    str_free(&evicted_text);

                    /* Step 3: Free evicted messages */
                    for (int i = evict_start; i < evict_end; i++) {
                        free(chat->msgs[i].role);
                        free(chat->msgs[i].content);
                        free(chat->msgs[i].tool_call_id);
                        free(chat->msgs[i].tool_calls_json);
                    }
                    /* Shift tail messages down */
                    int tail_count = chat->n_msgs - evict_end;
                    memmove(&chat->msgs[evict_start], &chat->msgs[evict_end],
                            tail_count * sizeof(llm_msg_t));
                    chat->n_msgs = evict_start + tail_count;

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

                    /* Step 4: Re-inject updated scratchpad at evict_start */
                    {
                        size_t sp_max = (ctx->llm->context_size > 0)
                            ? (size_t)ctx->llm->context_size * 4 * 15 / 100 : 8192;
                        char *fresh_sp = scratchpad_serialize_budget(
                            &ctx->tools->scratch, sp_max);
                        if (fresh_sp && fresh_sp[0]) {
                            size_t slen = strlen(fresh_sp);
                            char *sp_msg = malloc(slen + 32);
                            if (sp_msg) {
                                snprintf(sp_msg, slen + 32, "[SCRATCHPAD]\n%s", fresh_sp);
                                /* Insert scratchpad as a new message at evict_start */
                                if (chat->n_msgs >= chat->cap_msgs) {
                                    chat->cap_msgs *= 2;
                                    chat->msgs = realloc(chat->msgs,
                                        chat->cap_msgs * sizeof(llm_msg_t));
                                }
                                memmove(&chat->msgs[evict_start + 1],
                                        &chat->msgs[evict_start],
                                        (chat->n_msgs - evict_start) * sizeof(llm_msg_t));
                                chat->msgs[evict_start].role = strdup("user");
                                chat->msgs[evict_start].content = sp_msg;
                                chat->msgs[evict_start].tool_call_id = NULL;
                                chat->msgs[evict_start].tool_calls_json = NULL;
                                chat->n_msgs++;
                            }
                        }
                        free(fresh_sp);
                    }

                    /* Emit warning */
                    react_event_t ev = {0};
                    ev.type = REACT_EVENT_WARNING;
                    ev.step = step + 1;
                    ev.message = "Context compacted — evicted messages summarized into scratchpad";
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

        /* Check for pause request (Space pressed in TUI — toggle pause/resume).
         * Save checkpoint and exit cleanly so the task can be resumed later. */
        if (!final_result && ctx->pause_requested) {
            react_event_t ev = {0};
            ev.type = REACT_EVENT_WARNING;
            ev.step = step + 1;
            ev.message = "Paused (Space to resume, type query to redirect)";
            emit(on_event, userdata, &ev);
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
        checkpoint_remove(ctx);

    /* Post-task reflection: ask LLM to extract reusable lessons/strategies.
     * Fires for BOTH successful and failed tasks — failures are often more
     * valuable for learning (what went wrong, what to avoid next time). */
    int task_succeeded = (final_result != NULL);

    /* Validation scoring: update recall_hits for all recalled memories.
     * Counter bumps are written to JSON files but NOT git-committed —
     * these are high-frequency, low-value changes that pollute the git log
     * (access_count, recall_hits, recall_misses). Git history is reserved
     * for meaningful content changes (store, delete, prune, consolidate).
     *
     * FIX DESIGN1: Only increment hits on success, NOT blanket misses on
     * failure.  Previously, ALL recalled memories got a miss on failure,
     * but failure is rarely caused by the recalled memories — it's usually
     * task difficulty or model error.  Blanket miss attribution creates
     * noise that degrades vscore of high-recall, high-value memories
     * (their vscore converges to the background success rate rather than
     * the memory's actual contribution).  Corrective insights for truly
     * harmful memories are handled by reflection → SUPERSEDES.
     *
     * Future: the reflection phase could identify specific harmful memories
     * and increment misses only for those (targeted attribution). */
    if (ctx->flags.enable_scoring && ctx->tools->memory &&
        ctx->tools->n_recalled_keys > 0 && task_succeeded) {
        for (int i = 0; i < ctx->tools->n_recalled_keys; i++) {
            memory_increment_hits(ctx->tools->memory,
                                  ctx->tools->recalled_keys[i]);
        }
    }

    /* FIX D2: Skip reflection when max_reflection_steps == 0 */
    int max_refl = ctx->tools->cfg ? ctx->tools->cfg->max_reflection_steps : 4;
    if (ctx->flags.enable_reflection && ctx->tools->step > 2 && ctx->tools->memory && max_refl > 0) {
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
            } else if (ctx->tools->scratchpad && ctx->tools->scratchpad[0]) {
                sp_text = strdup(ctx->tools->scratchpad);
            }
            if (sp_text && sp_text[0]) {
                size_t slen = strlen(sp_text);
                char *sp_msg = malloc(slen + 32);
                if (sp_msg) {
                    snprintf(sp_msg, slen + 32, "[SCRATCHPAD]\n%s", sp_text);
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

        /* Mini react loop for reflection (max 4 steps) */
        for (int rstep = 0; rstep < (ctx->tools->cfg ? ctx->tools->cfg->max_reflection_steps : 4); rstep++) {
            llm_stats_t rstats = {0};
            int max_resp = ctx->tools->cfg ? ctx->tools->cfg->llm_max_response : 10*1024*1024;
            int rep_thresh = ctx->tools->cfg ? ctx->tools->cfg->llm_repeat_threshold : 100;
            char *rresp = provider_complete_stream(ctx->provider, reflect, &rstats,
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

            int should_store = 1;  /* declared outside if-block for use in feedback message */
            if (strcmp(ract, "memory_store") == 0) {
                /* FIX #4+B4: Deduplication guard — check if a very similar memory
                 * already exists before storing. This prevents reflection from
                 * creating near-duplicate entries on every task.
                 * B4 fix: Load existing entry's cached .emb file directly instead
                 * of calling memory_recall() (which generates a query embedding)
                 * and then re-embedding the existing entry. Saves 2 API calls. */
                cJSON *rkey_j = cJSON_GetObjectItem(raction, "key");
                cJSON *rval_j = cJSON_GetObjectItem(raction, "value");
                if (rkey_j && rkey_j->valuestring && rval_j && rval_j->valuestring &&
                    ctx->tools->memory && ctx->tools->memory->embed &&
                    ctx->tools->memory->embed->available) {
                    /* FIX BUG2: Generate a multi-vec embedding (1 chunk) so the
                     * dedup guard uses embed_cosine_sim_multi_multi — the same
                     * similarity function as consolidation_cb in tools.c.
                     * Previously used embed_text (single vec) which produces a
                     * different similarity metric than consolidation. */
                    int mic = embed_max_input_chars(
                                  ctx->tools->memory->embed);
                    char *prep = embed_prepare_text(rkey_j->valuestring,
                                                     rval_j->valuestring,
                                                     mic);
                    if (prep) {
                        embed_vec_t single = embed_text(
                            ctx->tools->memory->embed, prep);
                        free(prep);
                        if (single.data) {
                            /* Wrap single vec into 1-chunk multi-vec */
                            embed_multi_vec_t new_emb = {
                                .data = single.data,
                                .dim = single.dim,
                                .n_chunks = 1,
                            };
                            /* Scan existing .emb files for high similarity
                             * instead of calling memory_recall + re-embedding */
                            reflection_scan_t rscan = {
                                .ctx = ctx,
                                .rkey_j = rkey_j,
                                .new_emb = &new_emb,
                                .should_store = &should_store,
                                .task_succeeded = task_succeeded,
                            };
                            for_each_dir_entry(ctx->tools->memory->dir, ".emb",
                                              reflection_dedup_cb, &rscan);
                            embed_vec_free(&single);
                        }
                    }
                }

                if (should_store) {
                    tool_result_t tr = tool_execute(ctx->tools, "memory_store", raction);
                    /* Emit event so frontend can show it */
                    react_event_t ev = {0};
                    ev.type = REACT_EVENT_STEP_COMPLETE;
                    ev.action = "memory_store";
                    ev.description = "[reflection]";
                    emit(on_event, userdata, &ev);
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
    }

    /* P4: Scratchpad-to-memory promotion — auto-promote high-priority
     * scratchpad sections to long-term memory before they're lost.
     *
     * Research basis:
     *   DCPM [arXiv:2606.09483, Jun 2026] — cognitive capability hierarchy
     *     ascending from raw inputs through belief trajectories to schemas.
     *     Promotion moves working knowledge UP the hierarchy.
     *   Letta Skill Learning [May 2026] — agents that learn from past
     *     experience improve +36.8%. Promotion captures experience that
     *     the LLM didn't explicitly memory_store.
     *   MemoPilot [arXiv:2606.08656, ICML 2026] — RL-trained memory
     *     copilot that optimizes WHAT to store. Promotion is the heuristic
     *     equivalent: high-priority sections that survived compaction are
     *     worth persisting.
     *   AutoMEM [arXiv:2606.04315, Jun 2026] — self-managed memory with
     *     active control. Promotion gives the system active control over
     *     what crosses from working memory to long-term memory.
     *
     * Heuristic: sections with priority <= 1 and content > 200 chars
     * are promoted to fact:<section-name> memories. Deduplication via
     * embedding similarity prevents redundant storage. */
    if (ctx->tools->scratch.count > 0 && ctx->tools->memory && task_succeeded) {
        for (int si = 0; si < ctx->tools->scratch.count; si++) {
            scratchpad_section_t *sec = &ctx->tools->scratch.sections[si];
            if (!sec->name || !sec->content) continue;
            if (sec->priority > 1) continue;  /* only high-priority sections */
            if (strlen(sec->content) < 200) continue;  /* skip trivial content */

            /* Skip result sections (R<N>_result) — those are handled separately */
            if (sec->name[0] == 'R' && strstr(sec->name, "_result")) continue;

            /* Check if a similar memory already exists (dedup via recall) */
            char pkey[256];
            snprintf(pkey, sizeof(pkey), "fact:%s", sec->name);
            memory_results_t check = memory_recall(ctx->tools->memory, pkey, 1);
            int already_exists = 0;
            if (check.count > 0 && check.entries[0].relevance > 0.8)
                already_exists = 1;
            memory_results_free(&check);
            if (already_exists) continue;

            /* FIX BUG4: Promote via tool_execute (not direct memory_store)
             * so the entry flows through memory_try_consolidate. Previously,
             * promoted fact: entries could be near-duplicates of existing
             * lesson:/strategy: entries but would never be merged. */
            {
                cJSON *pp = cJSON_CreateObject();
                cJSON_AddStringToObject(pp, "key", pkey);
                cJSON_AddStringToObject(pp, "value", sec->content);
                tool_result_t tr = tool_execute(ctx->tools,
                                               "memory_store", pp);
                tool_result_free(&tr);
                cJSON_Delete(pp);
            }
        }
    }

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
        sec_pri_t orig_priorities[SCRATCHPAD_MAX_SECTIONS];
        int n_orig = ctx->tools->scratch.count;
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
            char *cleaned = extract_llm_text_output(raw_cleaned);
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
        free(preserved_result);
        free(full_sp);
    }

    /* Don't free last_query/last_result here — the caller (main.c) manages them.
     * They are set after each react_run() call and used to inject previous context. */

    /* Reset recalled keys for next query (each task is independent) */
    for (int i = 0; i < ctx->tools->n_recalled_keys; i++)
        free(ctx->tools->recalled_keys[i]);
    ctx->tools->n_recalled_keys = 0;

    return final_result;
}
