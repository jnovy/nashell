#include "react.h"
#include "journal.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_SCRATCHPAD_CHARS 8192  /* limit scratchpad injection to 8K */

/* Extract a JSON string field, returns NULL if missing */
static const char *json_get_str(cJSON *obj, const char *key) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item && cJSON_IsString(item)) return item->valuestring;
    return NULL;
}

char *react_run(react_ctx_t *ctx, const char *user_query) {
    llm_chat_t *chat = llm_chat_new();

    /* System message */
    llm_chat_add(chat, "system", tools_system_prompt());

    /* Inject journal manifest (shows what previous steps produced) */
    char *manifest = journal_manifest(ctx->tools->journal, 50);
    if (manifest) {
        llm_chat_add(chat, "user", manifest);
        free(manifest);
    }

    /* Inject scratchpad if exists (capped at MAX_SCRATCHPAD_CHARS) */
    if (ctx->tools->scratchpad && ctx->tools->scratchpad[0]) {
        size_t slen = strlen(ctx->tools->scratchpad);
        if (slen > MAX_SCRATCHPAD_CHARS) slen = MAX_SCRATCHPAD_CHARS;
        char *scratch_msg = malloc(slen + 32);
        if (scratch_msg) {
            snprintf(scratch_msg, slen + 32, "[SCRATCHPAD]\n%.*s",
                     (int)slen, ctx->tools->scratchpad);
            llm_chat_add(chat, "user", scratch_msg);
            free(scratch_msg);
        }
    }

    /* User query */
    llm_chat_add(chat, "user", user_query);

    char *final_result = NULL;
    struct timespec task_start;
    clock_gettime(CLOCK_MONOTONIC, &task_start);

    /* Action signature tracking for cycling detection */
    char last_sigs[8][256];
    int sig_count = 0;

    for (int step = 0; step < ctx->max_steps; step++) {
        ctx->tools->step = step + 1;

        if (ctx->verbose) {
            fprintf(stderr, "\r\033[K[step %d/%d] thinking...", step + 1, ctx->max_steps);
            fflush(stderr);
        }

        /* Call LLM */
        struct timespec step_start;
        clock_gettime(CLOCK_MONOTONIC, &step_start);

        char *response = llm_complete(ctx->llm, chat);
        if (!response) {
            fprintf(stderr, "[error] LLM call failed (returned NULL)\n");
            fflush(stderr);
            break;
        }

        struct timespec step_end;
        clock_gettime(CLOCK_MONOTONIC, &step_end);
        double step_elapsed = (step_end.tv_sec - step_start.tv_sec) +
                              (step_end.tv_nsec - step_start.tv_nsec) / 1e9;

        /* Parse JSON response — strip markdown fences if present */
        cJSON *action = llm_parse_action(response);

        if (!action) {
            fprintf(stderr, "\n[error] failed to parse LLM response as JSON\n");
            if (ctx->verbose) fprintf(stderr, "  raw: %.200s...\n", response);
            /* Add as assistant message and ask to retry */
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
            fprintf(stderr, "\n[error] no 'action' field in response\n");
            if (ctx->verbose) {
                char *raw = cJSON_PrintUnformatted(action);
                fprintf(stderr, "  parsed JSON: %.300s%s\n",
                        raw ? raw : "(null)", raw && strlen(raw) > 300 ? "..." : "");
                free(raw);
            }
            /* Retry like JSON parse failure — add response + correction prompt */
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

        if (ctx->verbose) {
            /* Show the key parameter for each tool, not the thought */
            const char *desc = NULL;
            if (strcmp(action_name, "shell_exec") == 0)
                desc = json_get_str(action, "command");
            else if (strcmp(action_name, "file_read") == 0 ||
                     strcmp(action_name, "file_write") == 0 ||
                     strcmp(action_name, "file_edit") == 0)
                desc = json_get_str(action, "path");
            else if (strcmp(action_name, "grep_search") == 0)
                desc = json_get_str(action, "pattern");
            else if (strcmp(action_name, "done") == 0)
                desc = thought;  /* for done, show the thought/summary */
            else if (strcmp(action_name, "notes") == 0)
                desc = "[saving notes]";
            if (!desc) desc = thought ? thought : "";
            /* Truncate long descriptions for display */
            char desc_buf[201];
            if (strlen(desc) > 200) {
                memcpy(desc_buf, desc, 197);
                desc_buf[197] = '.'; desc_buf[198] = '.'; desc_buf[199] = '.'; desc_buf[200] = '\0';
                desc = desc_buf;
            }
            fprintf(stderr, "\r\033[K[step %d] %s: %s (%.1fs)\n",
                    step + 1, action_name, desc, step_elapsed);
        }

        /* Check for done */
        if (strcmp(action_name, "done") == 0) {
            const char *result = json_get_str(action, "result");
            final_result = result ? strdup(result) : strdup("(no result)");
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
            strncpy(last_sigs[sig_count], sig, 255);
            last_sigs[sig_count][255] = '\0';
            sig_count++;
        } else {
            memmove(last_sigs, last_sigs + 1, 7 * 256);
            strncpy(last_sigs[7], sig, 255);
            last_sigs[7][255] = '\0';
        }

        if (repeated >= 2) {
            fprintf(stderr, "\n[warning] cycling detected — same action repeated %d times\n",
                    repeated + 1);
        }

        /* Execute tool */
        tool_result_t tr = tool_execute(ctx->tools, action_name, action);

        /* Build step metadata */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double total_elapsed = (now.tv_sec - task_start.tv_sec) +
                               (now.tv_nsec - task_start.tv_nsec) / 1e9;

        /* Build tool result string for context */
        char *meta_str = cJSON_PrintUnformatted(tr.meta);

        /* Debug: show tool output metadata */
        if (ctx->verbose) {
            fprintf(stderr, "  → %.*s%s\n",
                    (int)(strlen(meta_str) < 300 ? strlen(meta_str) : 300),
                    meta_str,
                    strlen(meta_str) > 300 ? "..." : "");

            /* Print stored file contents for shell_exec/grep_search */
            if (tr.store_ref) {
                char ref_path[4096];
                snprintf(ref_path, sizeof(ref_path), "%s/%s",
                         ctx->tools->session_dir, tr.store_ref);
                FILE *rf = fopen(ref_path, "r");
                if (rf) {
                    char rbuf[4096];
                    size_t total_read = 0;
                    size_t n;
                    while ((n = fread(rbuf, 1, sizeof(rbuf) - 1, rf)) > 0
                           && total_read < 8000) {
                        rbuf[n] = '\0';
                        fprintf(stderr, "%s", rbuf);
                        total_read += n;
                    }
                    if (total_read > 0 && rbuf[n > 0 ? n - 1 : 0] != '\n')
                        fprintf(stderr, "\n");
                    if (total_read >= 8000)
                        fprintf(stderr, "  ... (truncated at 8K)\n");
                    fclose(rf);
                }
            }
        }

        size_t result_len = strlen(meta_str) + 128;
        char *result_msg = malloc(result_len);
        snprintf(result_msg, result_len, "%s\n[step %d | %.1fs]",
                 meta_str, step + 1, total_elapsed);

        /* Add assistant + tool result to chat */
        llm_chat_add(chat, "assistant", response);
        llm_chat_add(chat, "user", result_msg);

        /* Cleanup */
        free(meta_str);
        free(result_msg);
        tool_result_free(&tr);
        cJSON_Delete(action);
        free(response);
    }

    if (ctx->verbose) {
        fprintf(stderr, "\r\033[K");
    }

    llm_chat_free(chat);
    return final_result ? final_result : strdup("(no result — max steps reached)");
}
