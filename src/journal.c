#include "journal.h"
#include "cJSON.h"
#include "str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

journal_t *journal_new(const char *session_dir) {
    journal_t *j = calloc(1, sizeof(*j));
    if (!j) return NULL;
    j->session_dir = strdup(session_dir);
    if (!j->session_dir) { free(j); return NULL; }
    char path[4096];
    snprintf(path, sizeof(path), "%s/journal.jsonl", session_dir);
    j->path = strdup(path);
    if (!j->path) { free(j->session_dir); free(j); return NULL; }
    return j;
}

void journal_free(journal_t *j) {
    if (!j) return;
    free(j->path);
    free(j->session_dir);
    free(j);
}

int journal_append(journal_t *j, int react_loop, int step, const char *tool,
                   cJSON *params, const char *ref,
                   size_t size, int lines, const char *error,
                   const char *tool_call_id) {
    FILE *f = fopen(j->path, "a");
    if (!f) return -1;

    /* Unix epoch timestamp with microsecond precision */
    struct timespec tp;
    clock_gettime(CLOCK_REALTIME, &tp);
    char ts[32];
    snprintf(ts, sizeof(ts), "%ld.%05ld", (long)tp.tv_sec, tp.tv_nsec / 10000);

    cJSON *entry = cJSON_CreateObject();
    cJSON_AddNumberToObject(entry, "react_loop", react_loop);
    cJSON_AddNumberToObject(entry, "step", step);
    cJSON_AddStringToObject(entry, "ts", ts);
    cJSON_AddStringToObject(entry, "tool", tool);
    if (params) cJSON_AddItemReferenceToObject(entry, "params", params);
    if (ref) cJSON_AddStringToObject(entry, "ref", ref);
    cJSON_AddNumberToObject(entry, "size", (double)size);
    cJSON_AddNumberToObject(entry, "lines", lines);
    cJSON_AddBoolToObject(entry, "failed", error != NULL);
    if (error) cJSON_AddStringToObject(entry, "error", error);
    if (tool_call_id) cJSON_AddStringToObject(entry, "tc_id", tool_call_id);

    char *json = cJSON_PrintUnformatted(entry);
    fprintf(f, "%s\n", json);
    free(json);
    cJSON_Delete(entry);
    fclose(f);
    return 0;
}

char *journal_manifest(journal_t *j, int max_steps) {
    FILE *f = fopen(j->path, "r");
    if (!f) return strdup("Session history: (empty — new session)");

    str_t out = str_new(2048);
    str_append_cstr(&out, "Session history:\n");

    char line[65536];
    int count = 0;
    int current_loop = -1;

    while (fgets(line, sizeof(line), f) && count < max_steps) {
        cJSON *entry = cJSON_Parse(line);
        if (!entry) continue;

        int loop = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(entry, "react_loop"));
        int step = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(entry, "step"));
        const char *tool = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "tool"));
        const char *ref = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "ref"));
        double sz = cJSON_GetNumberValue(cJSON_GetObjectItem(entry, "size"));
        cJSON *params = cJSON_GetObjectItem(entry, "params");

        /* New react loop — show header with query text */
        if (loop != current_loop) {
            current_loop = loop;
            if (loop > 0) str_append_cstr(&out, "\n");
            str_appendf(&out, "  [Query R%d]", loop);

            /* Look for the query text in this entry or next */
            if (tool && strcmp(tool, "query") == 0 && params) {
                cJSON *text = cJSON_GetObjectItem(params, "text");
                if (text && text->valuestring) {
                    char truncated[101];
                    snprintf(truncated, sizeof(truncated), "%.100s", text->valuestring);
                    str_appendf(&out, " \"%s\"", truncated);
                }
            }
            str_append_cstr(&out, "\n");
        }

        /* Skip system and query entries (already shown in header) */
        if (tool && (strcmp(tool, "system") == 0 || strcmp(tool, "query") == 0)) {
            cJSON_Delete(entry);
            count++;
            continue;
        }

        /* Extract key param for display */
        const char *key_param = "";
        if (params) {
            cJSON *cmd = cJSON_GetObjectItem(params, "command");
            cJSON *p = cJSON_GetObjectItem(params, "path");
            cJSON *pat = cJSON_GetObjectItem(params, "pattern");
            cJSON *res = cJSON_GetObjectItem(params, "result");
            if (cmd && cmd->valuestring) key_param = cmd->valuestring;
            else if (p && p->valuestring) key_param = p->valuestring;
            else if (pat && pat->valuestring) key_param = pat->valuestring;
            else if (res && res->valuestring) key_param = res->valuestring;
        }

        /* Format: "    ✓ R0S1: shell_exec "ls -la" → 473 chars"
         *         "    ✗ R0S3: web_search "query""
         * Errors NOT inlined — model can file_read(ref) if it needs details. */
        char buf[512];
        cJSON *failed_j = cJSON_GetObjectItem(entry, "failed");
        int failed = (failed_j && cJSON_IsTrue(failed_j));
        const char *mark = failed ? "✗" : "✓";

        if (tool && strcmp(tool, "done") == 0) {
            snprintf(buf, sizeof(buf), "    %s %s: → \"%.100s\"",
                     mark, ref ? ref : "?", key_param);
        } else if (failed) {
            /* Failed — just show ✗ and ref, no inline error text */
            snprintf(buf, sizeof(buf), "    %s %s: %s \"%.80s\"",
                     mark, ref ? ref : "?", tool ? tool : "?", key_param);
        } else {
            snprintf(buf, sizeof(buf), "    %s %s: %s \"%.80s\" → %d chars",
                     mark, ref ? ref : "?", tool ? tool : "?", key_param, (int)sz);
        }
        str_append_cstr(&out, buf);
        str_append_cstr(&out, "\n");

        cJSON_Delete(entry);
        count++;
    }
    fclose(f);

    return str_steal(&out);
}
