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
#include <dirent.h>

/* ── helpers ─────────────────────────────────────────── */

static const char *json_get_str(cJSON *obj, const char *key) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item && cJSON_IsString(item)) return item->valuestring;
    return NULL;
}

/* Log memory context injection to the journal for debugging.
 * Captures: index summary (total + type counts), pinned keys, skills recalled.
 * Called at both the checkpoint-restore path and the main react_run path. */
static void log_memory_context(tool_ctx_t *tools, int react_loop, int step,
                               const char *mem_index,
                               const char *pinned,
                               memory_results_t *skills,
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
            if (strncmp(p, "[PINNED: ", 10) == 0) {
                const char *end = strchr(p + 10, ']');
                if (end) {
                    char key[256];
                    int klen = (int)(end - (p + 10));
                    if (klen >= (int)sizeof(key)) klen = (int)sizeof(key) - 1;
                    memcpy(key, p + 10, (size_t)klen);
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

    /* Skills recalled — keys + brief info */
    if (skills && skills->count > 0) {
        cJSON *skill_arr = cJSON_CreateArray();
        int skill_injected = 0;
        for (int i = 0; i < skills->count; i++) {
            if (skills->entries[i].key &&
                strncmp(skills->entries[i].key, "skill:", 6) == 0) {
                cJSON *s = cJSON_CreateObject();
                cJSON_AddStringToObject(s, "key", skills->entries[i].key);
                cJSON_AddNumberToObject(s, "hits", skills->entries[i].recall_hits);
                cJSON_AddNumberToObject(s, "misses", skills->entries[i].recall_misses);
                cJSON_AddItemToArray(skill_arr, s);
                skill_injected++;
            }
        }
        cJSON_AddItemToObject(params, "skills_matched", skill_arr);
    } else {
        cJSON_AddItemToObject(params, "skills_matched", cJSON_CreateArray());
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

/* Unwrap nested JSON in the "thought" field.
 * Sometimes the LLM returns content that is itself a serialized JSON object
 * (e.g. {"thought":"...","action":"..."}), causing the thought display to show
 * raw JSON instead of clean text. This function detects and unwraps it. */
static void sanitize_thought(cJSON *action) {
    cJSON *th = cJSON_GetObjectItemCaseSensitive(action, "thought");
    if (!th || !cJSON_IsString(th) || !th->valuestring || th->valuestring[0] != '{')
        return;
    cJSON *nested = cJSON_Parse(th->valuestring);
    if (!nested) return;
    cJSON *inner = cJSON_GetObjectItemCaseSensitive(nested, "thought");
    if (inner && cJSON_IsString(inner) && inner->valuestring && inner->valuestring[0]) {
        free(th->valuestring);
        th->valuestring = strdup(inner->valuestring);
    }
    cJSON_Delete(nested);
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
                           ctx->tools->step, mem_index, pinned,
                           NULL, user_query);

        free(mem_index);
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
    char jpath[4096];
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

    fprintf(stderr, "[checkpoint] Restored from step %d (react loop %d)\n",
            saved_step, saved_loop);

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
    char path[4096], tmp_path[4096];
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
    char path[4096];
    snprintf(path, sizeof(path), "%s/checkpoint.json", ctx->tools->session_dir);
    unlink(path);
}

/* Read the original user_query from a checkpoint without restoring full state.
 * Used by /continue to resume with the original query instead of "continue".
 * Returns heap-allocated string or NULL if no checkpoint. Caller frees. */
char *checkpoint_read_query(const char *session_dir) {
    if (!session_dir) return NULL;
    char path[4096];
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
            cJSON_Delete(j);
        }
        /* JSON but no content field — fall through to return as-is */
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
        fprintf(stderr, "[checkpoint] resuming from step %d\n", resume_step);
    }


    if (!restored) {
    /* System message */
    llm_chat_add(chat, "system", tools_system_prompt());

    /* v5: No manifest injection — scratchpad is the sole persistence mechanism.
     * Cross-loop state is carried via scratchpad (auto-saved done results +
     * LLM-pruned summaries). Within-loop recovery uses LLM summarization
     * instead of manifest re-injection. */

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

        /* Inject relevant skills (procedural memory — loaded on-demand based on query).
         * FIX B1/D1: Use user_query for semantic recall, then filter by skill: prefix.
         * Previously used "skill:" as the query, which matched ALL skills by type
         * prefix rather than finding skills semantically relevant to the task. */
        int max_skills = ctx->tools->cfg ? ctx->tools->cfg->max_skills_per_query : 3;
        memory_results_t skills = memory_recall(ctx->tools->memory, user_query, max_skills * 3);
        if (skills.count > 0) {
            str_t skill_msg = str_new(4096);
            str_append_cstr(&skill_msg, "[RELEVANT SKILLS]\n");
            int skill_count = 0;
            for (int i = 0; i < skills.count && skill_count < max_skills; i++) {
                if (skills.entries[i].key &&
                    strncmp(skills.entries[i].key, "skill:", 6) == 0) {
                    str_appendf(&skill_msg, "\n--- %s ---\n%s\n",
                                skills.entries[i].key,
                                skills.entries[i].value ? skills.entries[i].value : "");
                    /* Track for validation scoring */
                    tool_track_recalled_key(ctx->tools, skills.entries[i].key);
                    skill_count++;
                }
            }
            if (skill_msg.len > 20) {  /* more than just the header */
                llm_chat_add(chat, "user", str_cstr(&skill_msg));
            }
            str_free(&skill_msg);
        }

        /* Log memory context for debugging — before freeing mem_index/pinned */
        log_memory_context(ctx->tools, ctx->tools->react_loop,
                           ctx->tools->step, mem_index, pinned,
                           &skills, user_query);

        free(mem_index);
        free(pinned);
        memory_results_free(&skills);
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

    /* v5: No last_exchange injection — cross-loop state is carried via scratchpad.
     * The scratchpad auto-saves done results and LLM-prunes stale data,
     * providing full semantic context instead of truncated 500-char snippets. */

    /* User query */
    llm_chat_add(chat, "user", user_query);

    /* Reset per-loop counters (react_loop is 0-based, incremented at END of loop) */
    ctx->tools->step = 0;

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

    } /* end if (!restored) */
    char *final_result = NULL;
    struct timespec task_start;
    clock_gettime(CLOCK_MONOTONIC, &task_start);

    /* Action signature tracking for cycling detection */
    char last_sigs[8][256];
    int sig_count = 0;
    int consecutive_null_responses = 0;  /* Track LLM failures (HTTP 500 etc.) */

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
        char *response = ctx->provider ?
            provider_complete_stream(ctx->provider, chat, &stats,
                on_event ? stream_token_cb : NULL, &sctx,
                max_resp, rep_thresh) :
            llm_complete_stream(ctx->llm, chat, &stats,
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
                cJSON_AddStringToObject(err_params, "error",
                    consecutive_null_responses >= 2
                        ? "LLM server error after scratchpad reformulation — giving up"
                        : "LLM server returned NULL response (HTTP 500 or malformed output)");
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
                    ctx->tools->react_loop, step, "server_error",
                    err_params, se_alias, se_alias ? strlen(se_alias) : 0, 0,
                    "LLM server error", NULL);
                free(se_alias);
                cJSON_Delete(err_params);
            }

            react_event_t ev = {0};
            ev.type = REACT_EVENT_ERROR;
            ev.step = step + 1;

            if (consecutive_null_responses >= 2) {
                ev.message = "LLM server error after scratchpad reformulation — giving up";
                emit(on_event, userdata, &ev);
                break;
            }

            /* HTTP 500 from the server typically means the model produced
             * malformed tool-call JSON.  The scratchpad often contains
             * code blocks and deeply-nested JSON escaping that confuse
             * the model's JSON generation.  Instead of blindly retrying
             * (same prompt → same malformed output), we ask the LLM to
             * reformulate the scratchpad into plain prose, then retry
             * once with the sanitised version. */

            /* Find the [SCRATCHPAD] message in the chat */
            int sp_idx = -1;
            for (int i = 0; i < chat->n_msgs; i++) {
                if (chat->msgs[i].content &&
                    strncmp(chat->msgs[i].content, "[SCRATCHPAD]", 12) == 0) {
                    sp_idx = i;
                    break;
                }
            }

            if (sp_idx >= 0) {
                ev.message = "LLM server error — reformulating scratchpad and retrying";
                emit(on_event, userdata, &ev);

                /* Build a one-shot reformulation request */
                llm_chat_t *rewrite = llm_chat_new();
                llm_chat_add(rewrite, "system",
                    "You are a text sanitiser. Rewrite the user's notes "
                    "into plain prose. Remove ALL code blocks, JSON "
                    "snippets, backtick-fenced sections, and deeply "
                    "escaped strings. Keep the semantic meaning and key "
                    "facts but express everything in simple sentences. "
                    "Output ONLY the rewritten text, nothing else.");
                llm_chat_add(rewrite, "user", chat->msgs[sp_idx].content + 13);

                char *reformulated = ctx->provider ?
                    provider_complete(ctx->provider, rewrite, NULL) :
                    llm_complete(ctx->llm, rewrite, NULL);
                llm_chat_free(rewrite);

                if (reformulated && strlen(reformulated) > 0) {
                    /* Replace the scratchpad message in-place */
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
                    /* Reformulation failed — strip scratchpad entirely */
                    free(reformulated);
                    llm_chat_remove_by_prefix(chat, "[SCRATCHPAD]");
                }
            } else {
                /* No scratchpad to reformulate — nothing we can do */
                ev.message = "LLM server error — no scratchpad to reformulate, giving up";
                emit(on_event, userdata, &ev);
                break;
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

        /* Check for done */
        if (strcmp(action_name, "done") == 0) {
            const char *result = json_get_str(action, "result");
            final_result = result ? strdup(result) : strdup("(no result)");
            checkpoint_remove(ctx);

            /* v5: Auto-save done result to scratchpad for cross-loop inheritance.
             * The scratchpad is the SOLE mechanism for passing results between
             * react loops. Section name includes loop number for traceability. */
            {
                char sec_name[32];
                snprintf(sec_name, sizeof(sec_name), "R%d_result",
                         ctx->tools->react_loop);
                scratchpad_write(&ctx->tools->scratch, sec_name,
                                 final_result, 1);  /* priority 1 = high */
                scratchpad_save(&ctx->tools->scratch, ctx->tools->session_dir);
            }

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
                        char *raw_summary = llm_complete(ctx->llm, summ_chat, NULL);
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

        /* P1: Per-turn memory re-evaluation — refresh injected memories as
         * the task context evolves. A session that starts as "fix a Python
         * bug" might evolve into "redesign the database schema" by step 15.
         * Without re-evaluation, stale memories from step 0 persist.
         *
         * Research basis:
         *   CALMem [arXiv:2605.20724, May 2026] — token-budget-adaptive
         *     injection mechanism (MOIM) that re-evaluates per turn.
         *   MemForest [arXiv:2605.23986, May 2026] — temporal indexing
         *     shows that memory relevance changes over time.
         *
         * Implementation: Every 3 steps, re-run memory_recall with the
         * current task context (user_query + last tool result summary).
         * If the memory set has changed significantly (>50% different keys),
         * inject an updated [MEMORY GUIDANCE] message.
         *
         * P2: Query-time synthesis — if memory_synthesis is enabled, pass
         * retrieved memories through an LLM synthesis call to generate
         * context-adapted guidance instead of injecting verbatim entries.
         *
         * Research basis:
         *   Mem-π [arXiv:2605.21463, May 2026] — generative memory policy
         *     that generates context-specific guidance, +59% on WebArena.
         *   DeferMem [arXiv:2605.22411, May 2026] — query-time evidence
         *     distillation produces faithful, self-contained evidence.
         *
         * The synthesis prompt can respond "NONE" for semantic abstention,
         * which is more nuanced than P0's score thresholding — the LLM
         * understands semantic relevance better than cosine similarity. */
        if (ctx->tools->memory && step > 0 && (step % 3) == 0) {
            /* Build context string from user query + recent tool result */
            str_t context_str = str_new(512);
            str_append_cstr(&context_str, user_query);
            if (tr.meta) {
                char *meta_s = cJSON_PrintUnformatted(tr.meta);
                if (meta_s) {
                    str_append_cstr(&context_str, " ");
                    str_append_cstr(&context_str, meta_s);
                    free(meta_s);
                }
            }

            memory_results_t refreshed = memory_recall(
                ctx->tools->memory, str_cstr(&context_str), 5);

            if (refreshed.count > 0) {
                int do_synthesis = ctx->tools->cfg &&
                                   ctx->tools->cfg->memory_synthesis;

                if (do_synthesis) {
                    /* P2: Query-time synthesis — generate adapted guidance.
                     * Uses a single LLM call with a synthesis prompt to fuse
                     * retrieved fragments into context-specific guidance.
                     * This is the frozen-model equivalent of Mem-π's
                     * generative memory [arXiv:2605.21463]. */
                    str_t synth_prompt = str_new(4096);
                    str_append_cstr(&synth_prompt,
                        "You are a memory synthesis module. Given the current "
                        "task context and retrieved memories, generate a concise, "
                        "actionable guidance paragraph that synthesizes the most "
                        "relevant insights. Adapt the guidance to the specific "
                        "current situation. If none of the memories are relevant "
                        "to the current task, respond with exactly \"NONE\".\n\n"
                        "Current task: ");
                    str_append_cstr(&synth_prompt, user_query);
                    str_append_cstr(&synth_prompt,
                        "\nCurrent step context: ");
                    if (tr.meta) {
                        char *ms = cJSON_PrintUnformatted(tr.meta);
                        if (ms) {
                            str_append_cstr(&synth_prompt, ms);
                            free(ms);
                        }
                    } else {
                        str_append_cstr(&synth_prompt, "(no recent result)");
                    }
                    str_append_cstr(&synth_prompt,
                        "\n\nRetrieved memories:\n");
                    for (int mi = 0; mi < refreshed.count; mi++) {
                        str_appendf(&synth_prompt, "%d. [%s] %s\n",
                            mi + 1,
                            refreshed.entries[mi].key ?
                                refreshed.entries[mi].key : "",
                            refreshed.entries[mi].value ?
                                refreshed.entries[mi].value : "");
                    }
                    str_append_cstr(&synth_prompt,
                        "\nSynthesized guidance:");

                    /* Single LLM call for synthesis */
                    llm_chat_t *synth_chat = llm_chat_new();
                    llm_chat_add(synth_chat, "user",
                                 str_cstr(&synth_prompt));
                    char *guidance = llm_complete(
                        ctx->llm, synth_chat, NULL);
                    llm_chat_free(synth_chat);
                    str_free(&synth_prompt);

                    /* Inject guidance unless LLM responded "NONE" (semantic
                     * abstention — more nuanced than score thresholding) */
                    if (guidance && strncmp(guidance, "NONE", 4) != 0) {
                        str_t mem_msg = str_new(512);
                        str_appendf(&mem_msg,
                            "[MEMORY GUIDANCE — step %d]\n%s", step + 1,
                            guidance);
                        llm_chat_add(chat, "user", str_cstr(&mem_msg));
                        str_free(&mem_msg);
                    }
                    free(guidance);
                } else {
                    /* No synthesis — inject top memories verbatim (P1 only).
                     * Still better than session-start-only injection because
                     * the memory set is refreshed based on evolved context. */
                    str_t mem_msg = str_new(2048);
                    str_appendf(&mem_msg,
                        "[RELEVANT MEMORIES — refreshed at step %d]\n",
                        step + 1);
                    for (int mi = 0; mi < refreshed.count && mi < 3; mi++) {
                        str_appendf(&mem_msg, "\n--- %s ---\n%s\n",
                            refreshed.entries[mi].key ?
                                refreshed.entries[mi].key : "",
                            refreshed.entries[mi].value ?
                                refreshed.entries[mi].value : "");
                        /* Track for validation scoring */
                        tool_track_recalled_key(ctx->tools,
                            refreshed.entries[mi].key);
                    }
                    llm_chat_add(chat, "user", str_cstr(&mem_msg));
                    str_free(&mem_msg);
                }
            }
            memory_results_free(&refreshed);
            str_free(&context_str);
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

    /* Validation scoring: update recall_hits/misses for all recalled memories.
     * Counter bumps are written to JSON files but NOT git-committed —
     * these are high-frequency, low-value changes that pollute the git log
     * (access_count, recall_hits, recall_misses). Git history is reserved
     * for meaningful content changes (store, delete, prune, consolidate). */
    if (ctx->tools->memory && ctx->tools->n_recalled_keys > 0) {
        for (int i = 0; i < ctx->tools->n_recalled_keys; i++) {
            if (task_succeeded)
                memory_increment_hits(ctx->tools->memory,
                                      ctx->tools->recalled_keys[i]);
            else
                memory_increment_misses(ctx->tools->memory,
                                        ctx->tools->recalled_keys[i]);
        }
    }

    /* FIX D2: Skip reflection when max_reflection_steps == 0 */
    int max_refl = ctx->tools->cfg ? ctx->tools->cfg->max_reflection_steps : 4;
    if (ctx->tools->step > 2 && ctx->tools->memory && max_refl > 0) {
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
                "- tags: include 'lesson' or 'strategy' or 'skill' tag plus domain tags\n"
                "Skills are reusable multi-step procedures (e.g. skill:compile-and-test-c).\n"
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
                "- tags: include 'lesson' tag plus domain tags\n\n"
                "For anti-patterns, call memory_store with:\n"
                "- key: anti-pattern:short-name (e.g. anti-pattern:never-grep-binary-files)\n"
                "- value: Start with 'NEVER' or 'AVOID'. State: what NOT to do, WHY it "
                "fails, and what to do INSTEAD. Include the trigger condition "
                "(when_NOT_to_apply).\n"
                "- tags: include 'anti-pattern' tag plus domain tags\n\n"
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
            char *rresp = ctx->provider ?
                provider_complete_stream(ctx->provider, reflect, &rstats,
                    NULL, NULL, max_resp, rep_thresh) :
                llm_complete_stream(ctx->llm, reflect, &rstats,
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
                /* FIX #4+B4: Deduplication guard — check if a very similar memory
                 * already exists before storing. This prevents reflection from
                 * creating near-duplicate entries on every task.
                 * B4 fix: Load existing entry's cached .emb file directly instead
                 * of calling memory_recall() (which generates a query embedding)
                 * and then re-embedding the existing entry. Saves 2 API calls. */
                int should_store = 1;
                cJSON *rkey_j = cJSON_GetObjectItem(raction, "key");
                cJSON *rval_j = cJSON_GetObjectItem(raction, "value");
                if (rkey_j && rkey_j->valuestring && rval_j && rval_j->valuestring &&
                    ctx->tools->memory && ctx->tools->memory->embed &&
                    ctx->tools->memory->embed->available) {
                    /* Generate embedding for the new entry.
                     * Use model-aware max_input_chars for full fidelity. */
                    int mic = embed_max_input_chars(
                                  ctx->tools->memory->embed);
                    char *prep = embed_prepare_text(rkey_j->valuestring,
                                                     rval_j->valuestring,
                                                     NULL, 0, mic);
                    if (prep) {
                        embed_vec_t new_emb = embed_text(
                            ctx->tools->memory->embed, prep);
                        free(prep);
                        if (new_emb.data) {
                            /* Scan existing .emb files for high similarity
                             * instead of calling memory_recall + re-embedding */
                            DIR *rdir = opendir(ctx->tools->memory->dir);
                            if (rdir) {
                                struct dirent *rde;
                                while ((rde = readdir(rdir)) != NULL) {
                                    if (rde->d_name[0] == '.') continue;
                                    size_t dlen = strlen(rde->d_name);
                                    if (dlen < 4 || strcmp(rde->d_name + dlen - 4, ".emb") != 0)
                                        continue;
                                    char emb_path[4096];
                                    snprintf(emb_path, sizeof(emb_path), "%s/%s",
                                             ctx->tools->memory->dir, rde->d_name);
                                    /* FIX B1+B2: Use multi-vec loader (auto-detects
                                     * old single-vec and new multi-vec formats) and
                                     * MaxSim similarity for consistent cross-subsystem
                                     * comparison with consolidation code path. */
                                    embed_multi_vec_t exist_emb = embed_multi_vec_load(emb_path);
                                    if (!exist_emb.data) continue;
                                    if (exist_emb.dim != new_emb.dim) {
                                        embed_multi_vec_free(&exist_emb);
                                        continue;
                                    }
                                    float sim = embed_cosine_sim_multi(&new_emb, &exist_emb);
                                    embed_multi_vec_free(&exist_emb);
                                    if (sim > 0.90f) {
                                        should_store = 0;
                                        fprintf(stderr,
                                            "[reflection] skipping near-duplicate "
                                            "memory (sim=%.2f): %s\n",
                                            sim, rkey_j->valuestring);
                                        break;
                                    }
                                }
                                closedir(rdir);
                            }
                            embed_vec_free(&new_emb);
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
                "Stored. Any more causal insights? What other assumptions, "
                "hidden variables, or invariants should be captured? "
                "Call memory_store or done.");
            cJSON_Delete(raction);
            free(rresp);
        }
        llm_chat_free(reflect);
    }

    /* v5: Post-reflection scratchpad pruning — remove solved/stale data so the
     * next react loop starts with a clean, focused scratchpad. Uses an LLM call
     * to intelligently merge and prune instead of blind accumulation. */
    if (final_result && ctx->tools->scratch.count > 0) {
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
                "Output the scratchpad with resolved items removed, nothing else changed.\n",
                final_result, full_sp);

            llm_chat_t *prune_chat = llm_chat_new();
            llm_chat_add(prune_chat, "user", str_cstr(&prune_prompt));
            char *raw_cleaned = llm_complete(ctx->llm, prune_chat, NULL);
            llm_chat_free(prune_chat);
            str_free(&prune_prompt);

            /* Extract text from LLM output — accepts both plain markdown
             * and JSON tool-call format (extracts "content" field). */
            char *cleaned = extract_llm_text_output(raw_cleaned);
            free(raw_cleaned);

            if (cleaned) {
                /* Replace scratchpad with cleaned version */
                scratchpad_write(&ctx->tools->scratch, "pruned", cleaned, 1);
                /* Remove old sections that were merged into "pruned" */
                for (int i = ctx->tools->scratch.count - 1; i >= 0; i--) {
                    if (strcmp(ctx->tools->scratch.sections[i].name, "pruned") != 0) {
                        scratchpad_clear(&ctx->tools->scratch,
                                         ctx->tools->scratch.sections[i].name);
                    }
                }
                scratchpad_save(&ctx->tools->scratch, ctx->tools->session_dir);
            }
            free(cleaned);
        }
        free(full_sp);
    }

    /* v5: No longer saving last_query/last_result — scratchpad carries all state */
    if (ctx->last_query) free(ctx->last_query);
    if (ctx->last_result) free(ctx->last_result);
    ctx->last_query = NULL;
    ctx->last_result = NULL;

    /* Reset recalled keys for next query (each task is independent) */
    for (int i = 0; i < ctx->tools->n_recalled_keys; i++)
        free(ctx->tools->recalled_keys[i]);
    ctx->tools->n_recalled_keys = 0;

    /* Increment react loop counter for next query */
    ctx->tools->react_loop++;

    return final_result;
}
