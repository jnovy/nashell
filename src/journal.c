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
                   size_t size, int lines, const char *error) {
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
    if (error) cJSON_AddStringToObject(entry, "error", error);

    char *json = cJSON_PrintUnformatted(entry);
    fprintf(f, "%s\n", json);
    free(json);
    cJSON_Delete(entry);
    fclose(f);
    return 0;
}

char *journal_manifest(journal_t *j, int max_steps) {
    FILE *f = fopen(j->path, "r");
    if (!f) return strdup("Journal: (empty)");

    str_t out = str_new(1024);
    str_append_cstr(&out, "Journal:\n");

    char line[65536];
    int count = 0;
    while (fgets(line, sizeof(line), f) && count < max_steps) {
        cJSON *entry = cJSON_Parse(line);
        if (!entry) continue;

        int step = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(entry, "step"));
        const char *tool = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "tool"));
        const char *ref = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "ref"));
        double sz = cJSON_GetNumberValue(cJSON_GetObjectItem(entry, "size"));
        const char *err = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "error"));

        /* Extract key param for display */
        cJSON *params = cJSON_GetObjectItem(entry, "params");
        const char *key_param = "";
        if (params) {
            cJSON *cmd = cJSON_GetObjectItem(params, "command");
            cJSON *p = cJSON_GetObjectItem(params, "path");
            cJSON *q = cJSON_GetObjectItem(params, "query");
            if (cmd && cmd->valuestring) key_param = cmd->valuestring;
            else if (p && p->valuestring) key_param = p->valuestring;
            else if (q && q->valuestring) key_param = q->valuestring;
        }

        char buf[512];
        if (err) {
            snprintf(buf, sizeof(buf), "  S%d: %s \"%.60s\" -> ERROR: %.80s (%d chars)\n",
                     step, tool ? tool : "?", key_param, err, (int)sz);
        } else {
            snprintf(buf, sizeof(buf), "  S%d: %s \"%.60s\" -> %d chars",
                     step, tool ? tool : "?", key_param, (int)sz);
            str_append_cstr(&out, buf);
            if (ref) {
                snprintf(buf, sizeof(buf), " [%s]", ref);
                str_append_cstr(&out, buf);
            }
            str_append_cstr(&out, "\n");
            cJSON_Delete(entry);
            count++;
            continue;
        }
        str_append_cstr(&out, buf);
        cJSON_Delete(entry);
        count++;
    }
    fclose(f);

    return str_steal(&out);
}
