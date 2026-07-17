/*
 * agents.c — Autonomous agent scheduler for nash.
 *
 * Scans workspaces for agent definitions (YAML files with cron schedules),
 * builds a time-sorted execution calendar, and runs due agents sequentially
 * via playbook_worker(). Invoked by `nash --agent`.
 *
 * Agent YAML files live in ~/.nash/workspaces/NAME/agent/ as .yaml files.
 * They extend the standard playbook format with: schedule, timeout, enabled.
 */

#pragma GCC diagnostic ignored "-Wformat-truncation"

#include "agents.h"
#include "mailbox.h"
#include "yaml_parse.h"
#include "nash_limits.h"
#include "str.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>

/* ── helpers ──────────────────────────────────────────── */

static void mkdirp(const char *path) {
    char tmp[NASH_PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

/* ── Cron parsing ─────────────────────────────────────── */

/* Parse a single cron field into a bitmask array.
 * Supports: wildcard (*), step (star-slash-N), ranges (N-M), lists (N,M).
 * out: array of unsigned char, 1 = matches.
 * min,max: valid range for this field (e.g., 0-59 for minutes).
 * Returns 0 on success, -1 on error. */
static int parse_cron_field(const char *field, unsigned char *out,
                            int min, int max) {
    int range = max - min + 1;

    /* Wildcard */
    if (strcmp(field, "*") == 0) {
        for (int i = 0; i < range; i++)
            out[i] = 1;
        return 0;
    }

    /* Step: e.g. star-slash-5 means every 5th value */
    if (field[0] == '*' && field[1] == '/') {
        int step = atoi(field + 2);
        if (step <= 0) return -1;
        for (int i = 0; i < range; i++) {
            if ((i % step) == 0)
                out[i] = 1;
        }
        return 0;
    }

    /* Comma-separated values and/or ranges */
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", field);
    char *saveptr = NULL;
    char *token = strtok_r(buf, ",", &saveptr);
    while (token) {
        /* Range: N-M */
        char *dash = strchr(token, '-');
        if (dash) {
            *dash = '\0';
            int lo = atoi(token) - min;
            int hi = atoi(dash + 1) - min;
            if (lo < 0 || hi >= range || lo > hi) return -1;
            for (int i = lo; i <= hi; i++)
                out[i] = 1;
        } else {
            /* Single value */
            int val = atoi(token) - min;
            if (val < 0 || val >= range) return -1;
            out[val] = 1;
        }
        token = strtok_r(NULL, ",", &saveptr);
    }
    return 0;
}

int agent_parse_schedule(const char *cron_str, agent_schedule_t *sched) {
    if (!cron_str || !sched) return -1;
    memset(sched, 0, sizeof(*sched));

    /* Convenience aliases */
    if (strcmp(cron_str, "@startup") == 0) {
        sched->is_startup = 1;
        return 0;
    }
    const char *expanded = cron_str;
    if (strcmp(cron_str, "@hourly") == 0)  expanded = "0 * * * *";
    if (strcmp(cron_str, "@daily") == 0)   expanded = "0 2 * * *";
    if (strcmp(cron_str, "@weekly") == 0)  expanded = "0 2 * * 0";
    if (strcmp(cron_str, "@monthly") == 0) expanded = "0 2 1 * *";

    /* Parse 5 fields: minute hour dom month dow */
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", expanded);

    char *fields[5];
    int nf = 0;
    char *saveptr = NULL;
    char *tok = strtok_r(buf, " \t", &saveptr);
    while (tok && nf < 5) {
        fields[nf++] = tok;
        tok = strtok_r(NULL, " \t", &saveptr);
    }
    if (nf != 5) return -1;

    if (parse_cron_field(fields[0], sched->minute, 0, 59) != 0) return -1;
    if (parse_cron_field(fields[1], sched->hour,   0, 23) != 0) return -1;
    if (parse_cron_field(fields[2], sched->dom,    1, 31) != 0) return -1;
    if (parse_cron_field(fields[3], sched->month,  1, 12) != 0) return -1;
    if (parse_cron_field(fields[4], sched->dow,    0,  6) != 0) return -1;

    return 0;
}

time_t agent_next_occurrence(const agent_schedule_t *sched, time_t after) {
    if (!sched) return 0;
    if (sched->is_startup) return 0; /* always due */

    /* Start from one minute after `after`, scan forward up to 1 year */
    struct tm tm;
    time_t t = after + 60;
    localtime_r(&t, &tm);
    tm.tm_sec = 0; /* snap to minute boundary */
    t = mktime(&tm);

    /* Max iterations: ~525960 minutes in a year */
    for (int i = 0; i < 525960; i++) {
        localtime_r(&t, &tm);

        if (sched->month[tm.tm_mon] &&
            sched->dom[tm.tm_mday - 1] &&
            sched->dow[tm.tm_wday] &&
            sched->hour[tm.tm_hour] &&
            sched->minute[tm.tm_min]) {
            return t;
        }
        t += 60; /* advance one minute */
    }
    return 0; /* no match within a year */
}

/* ── Recursive workspace scanner ─────────────────────── */

/* Scan a directory for workspaces. A directory is a workspace if it contains
 * an agent/ subdirectory with .yaml files. Handles nested workspaces like
 * rh/container-tools by recursing into subdirectories.
 *
 * ws_prefix: relative workspace name prefix (e.g., "rh" when scanning rh/)
 * dir_path:  absolute path to the directory being scanned */
static void scan_workspace_dir(const char *nash_dir, const char *dir_path,
                                const char *ws_prefix,
                                agent_entry_t **agents, int *n_agents,
                                int *cap_agents) {
    /* Check if this directory has an agent/ subdirectory */
    char agents_dir[NASH_PATH_MAX];
    snprintf(agents_dir, sizeof(agents_dir), "%s/agent", dir_path);

    struct stat st;
    if (stat(agents_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
        /* Scan .yaml files in agent/ */
        DIR *dp = opendir(agents_dir);
        if (dp) {
            struct dirent *de;
            while ((de = readdir(dp)) != NULL) {
                if (de->d_name[0] == '.') continue;
                size_t nlen = strlen(de->d_name);
                if (nlen < 6 || strcmp(de->d_name + nlen - 5, ".yaml") != 0)
                    continue;

                char yaml_path[NASH_PATH_MAX];
                snprintf(yaml_path, sizeof(yaml_path), "%s/%s",
                         agents_dir, de->d_name);

                /* Parse YAML to get agent-specific fields */
                yaml_node_t *root = yaml_parse_file(yaml_path);
                if (!root) continue;

                /* Check enabled flag (default: true) */
                int enabled = yaml_bool(yaml_get(root, "enabled"), 1);
                if (!enabled) {
                    yaml_free(root);
                    continue;
                }

                /* Get schedule (NULL = manual-only agent) */
                const char *sched_str = yaml_str(yaml_get(root, "schedule"));

                /* Get name */
                const char *name = yaml_str(yaml_get(root, "name"));
                if (!name) {
                    /* Derive from filename: strip .yaml */
                    static char namebuf[256];
                    snprintf(namebuf, sizeof(namebuf), "%s", de->d_name);
                    namebuf[nlen - 5] = '\0';
                    name = namebuf;
                }

                /* Parse schedule (if provided) */
                agent_schedule_t parsed_sched;
                memset(&parsed_sched, 0, sizeof(parsed_sched));
                int has_schedule = 0;
                if (sched_str && sched_str[0] &&
                    strcmp(sched_str, "manual") != 0 &&
                    strcmp(sched_str, "none") != 0) {
                    if (agent_parse_schedule(sched_str, &parsed_sched) != 0) {
                        fprintf(stderr, "[agent] warning: bad schedule '%s' in %s\n",
                                sched_str, yaml_path);
                        yaml_free(root);
                        continue;
                    }
                    has_schedule = 1;
                }

                /* Build agent entry */
                if (*n_agents >= *cap_agents) {
                    *cap_agents = (*cap_agents == 0) ? 16 : *cap_agents * 2;
                    *agents = realloc(*agents, *cap_agents * sizeof(agent_entry_t));
                }

                agent_entry_t *a = &(*agents)[*n_agents];
                memset(a, 0, sizeof(*a));

                /* ID: workspace_name/agent_name */
                char id[NASH_PATH_MAX];
                snprintf(id, sizeof(id), "%s/%s", ws_prefix, name);
                a->id = strdup(id);
                a->workspace_name = strdup(ws_prefix);
                a->agent_file = strdup(yaml_path);
                a->workspace_dir = strdup(dir_path);
                if (has_schedule)
                    a->schedule = parsed_sched;
                a->schedule_str = strdup(has_schedule ? sched_str : "manual");
                a->timeout = yaml_int(yaml_get(root, "timeout"), 0);
                a->enabled = 1;
                a->last_status = strdup("never");
                const char *desc = yaml_str(yaml_get(root, "description"));
                a->description = strdup(desc ? desc : "");

                (*n_agents)++;
                yaml_free(root);
            }
            closedir(dp);
        }
    }

    /* Recurse into subdirectories (for nested workspaces like rh/container-tools) */
    DIR *dp = opendir(dir_path);
    if (!dp) return;

    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (strcmp(de->d_name, "agent") == 0) continue; /* skip agent dir itself */
        if (strcmp(de->d_name, ".memory") == 0) continue;
        if (strcmp(de->d_name, "memory") == 0) continue;

        char subpath[NASH_PATH_MAX];
        snprintf(subpath, sizeof(subpath), "%s/%s", dir_path, de->d_name);

        struct stat sst;
        if (stat(subpath, &sst) != 0 || !S_ISDIR(sst.st_mode)) continue;

        /* Build nested prefix */
        char subprefix[NASH_PATH_MAX];
        if (ws_prefix[0])
            snprintf(subprefix, sizeof(subprefix), "%s/%s", ws_prefix, de->d_name);
        else
            snprintf(subprefix, sizeof(subprefix), "%s", de->d_name);

        scan_workspace_dir(nash_dir, subpath, subprefix,
                           agents, n_agents, cap_agents);
    }
    closedir(dp);
}

agent_queue_t *agent_scan(const char *nash_dir) {
    agent_queue_t *q = calloc(1, sizeof(*q));
    if (!q) return NULL;

    char ws_root[NASH_PATH_MAX];
    snprintf(ws_root, sizeof(ws_root), "%s/workspaces", nash_dir);

    int cap = 0;
    scan_workspace_dir(nash_dir, ws_root, "",
                       &q->agents, &q->n_agents, &cap);

    /* Fix IDs: remove leading slash if ws_prefix was "" */
    for (int i = 0; i < q->n_agents; i++) {
        if (q->agents[i].id[0] == '/') {
            char *fixed = strdup(q->agents[i].id + 1);
            free(q->agents[i].id);
            q->agents[i].id = fixed;
        }
    }

    q->built_at = time(NULL);
    return q;
}

/* ── Queue persistence (queue.json) ───────────────────── */

int agent_queue_load(agent_queue_t *q, const char *nash_dir) {
    if (!q || !nash_dir) return -1;

    char path[NASH_PATH_MAX];
    snprintf(path, sizeof(path), "%s/agent/queue.json", nash_dir);

    cJSON *root = slurp_json(path);
    if (!root) return 0; /* no queue file yet — first run */

    cJSON *arr = cJSON_GetObjectItem(root, "agents");
    if (!arr || !cJSON_IsArray(arr)) {
        cJSON_Delete(root);
        return 0;
    }

    /* Merge persisted state into scanned agents */
    int n = cJSON_GetArraySize(arr);
    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(item, "id"));
        if (!id) continue;

        /* Find matching scanned agent */
        for (int j = 0; j < q->n_agents; j++) {
            if (strcmp(q->agents[j].id, id) != 0) continue;

            cJSON *lr = cJSON_GetObjectItem(item, "last_run");
            if (lr && cJSON_IsNumber(lr))
                q->agents[j].last_run = (time_t)cJSON_GetNumberValue(lr);

            cJSON *ld = cJSON_GetObjectItem(item, "last_duration");
            if (ld && cJSON_IsNumber(ld))
                q->agents[j].last_duration = (int)cJSON_GetNumberValue(ld);

            cJSON *ls = cJSON_GetObjectItem(item, "last_status");
            if (ls && cJSON_IsString(ls)) {
                free(q->agents[j].last_status);
                q->agents[j].last_status = strdup(cJSON_GetStringValue(ls));
            }
            break;
        }
    }

    cJSON_Delete(root);
    return 0;
}

int agent_queue_save(const agent_queue_t *q, const char *nash_dir) {
    if (!q || !nash_dir) return -1;

    /* Ensure agent/ directory exists */
    char agents_dir[NASH_PATH_MAX];
    snprintf(agents_dir, sizeof(agents_dir), "%s/agent", nash_dir);
    mkdirp(agents_dir);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddNumberToObject(root, "built_at", (double)q->built_at);

    cJSON *arr = cJSON_AddArrayToObject(root, "agents");
    for (int i = 0; i < q->n_agents; i++) {
        const agent_entry_t *a = &q->agents[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", a->id);
        cJSON_AddStringToObject(item, "workspace", a->workspace_name);
        cJSON_AddStringToObject(item, "agent_file", a->agent_file);
        cJSON_AddStringToObject(item, "schedule", a->schedule_str);
        cJSON_AddNumberToObject(item, "timeout", a->timeout);
        cJSON_AddNumberToObject(item, "last_run", (double)a->last_run);
        cJSON_AddStringToObject(item, "last_status",
                                a->last_status ? a->last_status : "never");
        cJSON_AddNumberToObject(item, "last_duration", a->last_duration);
        cJSON_AddNumberToObject(item, "next_due", (double)a->next_due);
        cJSON_AddItemToArray(arr, item);
    }

    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);
    if (!json_str) return -1;

    /* Atomic write: .tmp + rename */
    char tmp_path[NASH_PATH_MAX], final_path[NASH_PATH_MAX];
    snprintf(tmp_path, sizeof(tmp_path), "%s/agent/queue.json.tmp", nash_dir);
    snprintf(final_path, sizeof(final_path), "%s/agent/queue.json", nash_dir);

    int rc = write_file(tmp_path, json_str, strlen(json_str));
    free(json_str);
    if (rc != 0) return -1;

    if (rename(tmp_path, final_path) != 0) {
        fprintf(stderr, "[agent] error: rename %s → %s: %s\n",
                tmp_path, final_path, strerror(errno));
        return -1;
    }
    return 0;
}

/* Update queue.json with execution results for a single agent.
 * Used by the TUI /agent run completion handler which doesn't hold
 * a full agent_queue_t after the async playbook finishes. */
int agent_queue_update_run(const char *nash_dir, const char *agent_id,
                           time_t run_time, int duration,
                           const char *status) {
    if (!nash_dir || !agent_id) return -1;

    agent_queue_t *q = agent_scan(nash_dir);
    if (!q) return -1;

    agent_queue_load(q, nash_dir);

    /* Find and update the matching agent */
    for (int i = 0; i < q->n_agents; i++) {
        if (strcmp(q->agents[i].id, agent_id) != 0) continue;

        q->agents[i].last_run = run_time;
        q->agents[i].last_duration = duration;
        free(q->agents[i].last_status);
        q->agents[i].last_status = strdup(status ? status : "unknown");
        q->agents[i].next_due = agent_next_occurrence(
            &q->agents[i].schedule, run_time);
        break;
    }

    int rc = agent_queue_save(q, nash_dir);
    agent_queue_free(q);
    return rc;
}

/* ── Schedule computation ─────────────────────────────── */

static int cmp_next_due(const void *a, const void *b) {
    const agent_entry_t *ea = (const agent_entry_t *)a;
    const agent_entry_t *eb = (const agent_entry_t *)b;
    /* Agents with next_due=0 (@startup or never-run) sort first */
    if (ea->next_due == 0 && eb->next_due != 0) return -1;
    if (ea->next_due != 0 && eb->next_due == 0) return 1;
    if (ea->next_due < eb->next_due) return -1;
    if (ea->next_due > eb->next_due) return 1;
    return 0;
}

void agent_queue_schedule(agent_queue_t *q, time_t now) {
    if (!q) return;
    q->n_due = 0;

    for (int i = 0; i < q->n_agents; i++) {
        agent_entry_t *a = &q->agents[i];

        /* Manual-only agents (no schedule) are never automatically due */
        if (a->schedule_str && strcmp(a->schedule_str, "manual") == 0) {
            a->next_due = 0;
            a->is_due = 0;
            continue;
        }

        if (a->schedule.is_startup) {
            /* @startup: always due */
            a->next_due = 0;
            a->is_due = 1;
            q->n_due++;
            continue;
        }

        if (a->last_run == 0) {
            /* Never run: due immediately */
            a->next_due = 0;
            a->is_due = 1;
            q->n_due++;
            continue;
        }

        /* Compute next occurrence after last_run */
        a->next_due = agent_next_occurrence(&a->schedule, a->last_run);
        a->is_due = (a->next_due > 0 && a->next_due <= now);
        if (a->is_due) q->n_due++;
    }

    /* Sort by next_due ascending */
    qsort(q->agents, q->n_agents, sizeof(agent_entry_t), cmp_next_due);
}

/* ── History logging ──────────────────────────────────── */

int agent_history_append(const char *nash_dir, const agent_entry_t *agent,
                         int duration, const char *status,
                         const char *session_id) {
    if (!nash_dir || !agent) return -1;

    char agents_dir[NASH_PATH_MAX];
    snprintf(agents_dir, sizeof(agents_dir), "%s/agent", nash_dir);
    mkdirp(agents_dir);

    char path[NASH_PATH_MAX];
    snprintf(path, sizeof(path), "%s/agent/history.jsonl", nash_dir);

    FILE *f = fopen(path, "a");
    if (!f) return -1;

    fprintf(f, "{\"id\":\"%s\",\"ts\":%ld,\"dur\":%d,\"st\":\"%s\"",
            agent->id, (long)time(NULL), duration,
            status ? status : "unknown");
    if (session_id)
        fprintf(f, ",\"session\":\"%s\"", session_id);
    fprintf(f, "}\n");
    fclose(f);
    return 0;
}

void agent_save_result(const char *nash_dir, const char *agent_id,
                       const char *session_dir) {
    if (!nash_dir || !agent_id || !session_dir) return;

    char dir[NASH_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/agent/results/%s", nash_dir, agent_id);
    mkdirp(dir);

    /* Symlink latest.md -> session's result.md (no duplication).
     * Agent sessions run headless (no TUI) so session.md is never
     * generated -- result.md is written by the done tool handler
     * and reliably contains the final result text. */
    char link_path[NASH_PATH_MAX];
    snprintf(link_path, sizeof(link_path), "%s/latest.md", dir);
    unlink(link_path);  /* remove old symlink/file */

    char target[NASH_PATH_MAX];
    snprintf(target, sizeof(target), "%s/result.md", session_dir);
    symlink(target, link_path);
}

/* ── Queue printing ───────────────────────────────────── */

void agent_queue_print(const agent_queue_t *q, FILE *out) {
    if (!q || !out) return;

    fprintf(out, "%-40s %-14s %-16s %s\n",
            "AGENT", "SCHEDULE", "LAST RUN", "STATUS");
    fprintf(out, "%-40s %-14s %-16s %s\n",
            "─────", "────────", "────────", "──────");

    time_t now = time(NULL);
    for (int i = 0; i < q->n_agents; i++) {
        const agent_entry_t *a = &q->agents[i];

        /* Format last run as relative time */
        char last_run_str[64];
        if (a->last_run == 0) {
            snprintf(last_run_str, sizeof(last_run_str), "never");
        } else {
            int diff = (int)(now - a->last_run);
            if (diff < 60)
                snprintf(last_run_str, sizeof(last_run_str), "%ds ago", diff);
            else if (diff < 3600)
                snprintf(last_run_str, sizeof(last_run_str), "%dm ago", diff / 60);
            else if (diff < 86400)
                snprintf(last_run_str, sizeof(last_run_str), "%dh ago", diff / 3600);
            else
                snprintf(last_run_str, sizeof(last_run_str), "%dd ago", diff / 86400);
        }

        /* Format status with duration */
        char status_str[64];
        if (a->last_run == 0) {
            snprintf(status_str, sizeof(status_str), "-");
        } else {
            char durbuf[32];
            fmt_duration(a->last_duration, durbuf, sizeof(durbuf));
            snprintf(status_str, sizeof(status_str), "%s (%s)",
                     a->last_status ? a->last_status : "?", durbuf);
        }

        fprintf(out, "%-40s %-14s %-16s %s\n",
                a->id, a->schedule_str, last_run_str, status_str);
    }

    if (q->n_agents == 0)
        fprintf(out, "(no agents found)\n");
}

/* ── Helpers shared by execute + TUI ──────────────────── */

const agent_entry_t *agent_find(const agent_queue_t *q, const char *id) {
    if (!q || !id || !*id) return NULL;
    /* Numeric index: "1", "2", etc. (1-based) */
    char *endp;
    long idx = strtol(id, &endp, 10);
    if (*endp == '\0' && endp != id && idx >= 1 && idx <= q->n_agents)
        return &q->agents[idx - 1];
    /* Exact match */
    for (int i = 0; i < q->n_agents; i++)
        if (strcmp(q->agents[i].id, id) == 0) return &q->agents[i];
    /* Suffix match: "daily-ai-news" matches "ai-news/daily-ai-news" */
    int id_len = (int)strlen(id);
    const agent_entry_t *match = NULL;
    int n_matches = 0;
    for (int i = 0; i < q->n_agents; i++) {
        int aid_len = (int)strlen(q->agents[i].id);
        if (aid_len > id_len &&
            q->agents[i].id[aid_len - id_len - 1] == '/' &&
            strcmp(q->agents[i].id + aid_len - id_len, id) == 0) {
            match = &q->agents[i];
            n_matches++;
        }
    }
    return (n_matches == 1) ? match : NULL;
}

playbook_t *agent_prepare_playbook(const agent_entry_t *a,
                                   const char *arguments) {
    if (!a) return NULL;
    playbook_t *pb = playbook_load(a->agent_file);
    if (!pb) return NULL;

    /* Count how many argument tokens we have (for {{arg1}}, {{arg2}}, ...) */
    int n_arg_tokens = 0;
    char **arg_tokens = NULL;
    if (arguments && *arguments) {
        /* First pass: count tokens */
        char *tmp = strdup(arguments);
        char *saveptr;
        for (char *tok = strtok_r(tmp, " \t", &saveptr); tok;
             tok = strtok_r(NULL, " \t", &saveptr))
            n_arg_tokens++;
        free(tmp);
        /* Second pass: extract tokens */
        if (n_arg_tokens > 0) {
            arg_tokens = calloc((size_t)n_arg_tokens, sizeof(char *));
            int ti = 0;
            tmp = strdup(arguments);
            for (char *tok = strtok_r(tmp, " \t", &saveptr); tok;
                 tok = strtok_r(NULL, " \t", &saveptr))
                arg_tokens[ti++] = strdup(tok);
            free(tmp);
        }
    }

    /* Inject agent-specific template variables:
     *   3 base vars + 1 {{arguments}} (always) + n {{argN}} tokens */
    int n_extra = 3 + 1 + n_arg_tokens;
    int new_nvars = pb->n_vars + n_extra;
    pb->var_keys   = realloc(pb->var_keys,   (size_t)new_nvars * sizeof(char *));
    pb->var_values = realloc(pb->var_values,  (size_t)new_nvars * sizeof(char *));
    int vi = pb->n_vars;
    pb->var_keys[vi]       = strdup("workspace_name");
    pb->var_values[vi]     = strdup(a->workspace_name);
    pb->var_keys[vi + 1]   = strdup("workspace_dir");
    pb->var_values[vi + 1] = strdup(a->workspace_dir);
    pb->var_keys[vi + 2]   = strdup("agent_id");
    pb->var_values[vi + 2] = strdup(a->id);
    vi += 3;

    /* Always inject {{arguments}} (empty string if none provided) */
    pb->var_keys[vi]   = strdup("arguments");
    pb->var_values[vi] = strdup((arguments && *arguments) ? arguments : "");
    vi++;
    /* Inject {{arg1}}, {{arg2}}, ... for each provided token */
    for (int i = 0; i < n_arg_tokens; i++) {
        char key[32];
        snprintf(key, sizeof(key), "arg%d", i + 1);
        pb->var_keys[vi]   = strdup(key);
        pb->var_values[vi] = arg_tokens[i]; /* transfer ownership */
        vi++;
    }
    pb->n_vars = vi;

    free(arg_tokens);
    return pb;
}

/* ── Agent execution ──────────────────────────────────── */

int agent_execute(agent_queue_t *q, const char *nash_dir,
                  store_t *shared_store, config_t *cfg,
                  provider_t *provider, const char *server_model,
                  const char *force_id,
                  volatile sig_atomic_t *shutdown_flag,
                  const char *mailbox_dir) {
    if (!q || !nash_dir) return -1;

    int n_ok = 0, n_fail = 0;

    for (int i = 0; i < q->n_agents; i++) {
        if (shutdown_flag && *shutdown_flag) break;

        agent_entry_t *a = &q->agents[i];

        /* Skip agents that are not due (unless forced) */
        if (force_id) {
            if (strcmp(a->id, force_id) != 0) continue;
        } else if (!a->is_due) {
            continue;
        }

        fprintf(stderr, "[agent] ▶ %s\n", a->id);

        /* Create workspace for this agent */
        workspace_t *agent_ws = workspace_new(nash_dir, a->workspace_name,
                                               0, cfg->workspace_global_weight);
        /* Initialize recall config + embeddings on agent workspace */
        if (agent_ws) {
            workspace_set_recall_config(agent_ws, cfg->recall_min_score,
                                        cfg->recall_blend_semantic,
                                        cfg->recall_blend_substring,
                                        cfg->vscore_exponent);
            if (cfg->embedding.type &&
                strcmp(cfg->embedding.type, "none") != 0)
                workspace_init_embeddings(agent_ws, cfg->embedding.type,
                                          cfg->embedding.model,
                                          cfg->embedding.api_base,
                                          cfg->embedding.model_path,
                                          cfg->embedding.dimension,
                                          cfg->embedding.max_input_chars);
        }

        playbook_t *pb = agent_prepare_playbook(a, NULL);
        if (!pb) {
            fprintf(stderr, "[agent] ✗ failed to load agent '%s'\n", a->id);
            n_fail++;
            workspace_free(agent_ws);
            continue;
        }

        /* Run with timeout */
        struct timespec start_ts;
        clock_gettime(CLOCK_REALTIME, &start_ts);

        if (a->timeout > 0)
            alarm((unsigned)a->timeout);

        playbook_args_t pargs = {
            .playbook = pb,
            .nash_dir = (char *)nash_dir,
            .store = shared_store,
            .memory = agent_ws ? agent_ws->global : NULL,
            .cfg = cfg,
            .provider = provider,
            .server_model = (char *)server_model,
            .ui = NULL,  /* headless */
            .playbook_ok = 0,
            .done = 0,
            .workspace_override = a->workspace_name ? strdup(a->workspace_name) : NULL,
            .agent_ws = agent_ws,
        };

        playbook_worker(&pargs);
        free(pargs.workspace_override);

        if (a->timeout > 0)
            alarm(0); /* cancel alarm */

        /* Record result */
        struct timespec end_ts;
        clock_gettime(CLOCK_REALTIME, &end_ts);
        int dur = (int)(end_ts.tv_sec - start_ts.tv_sec);
        const char *status = pargs.playbook_ok ? "ok" : "fail";

        a->last_run = start_ts.tv_sec;
        a->last_duration = dur;
        free(a->last_status);
        a->last_status = strdup(status);
        a->next_due = agent_next_occurrence(&a->schedule, a->last_run);

        agent_history_append(nash_dir, a, dur, status, NULL);

        /* Symlink latest.md -> session's result.md for `/agent result` */
        if (pargs.last_session_dir)
            agent_save_result(nash_dir, a->id, pargs.last_session_dir);

        /* Route result through mailbox so bridge threads deliver it */
        if (mailbox_dir && pargs.result_text) {
            char task_id[256];
            snprintf(task_id, sizeof(task_id), "agent_%s", a->id);
            /* Replace / with _ in task_id for filename safety */
            for (char *p = task_id; *p; p++)
                if (*p == '/') *p = '_';

            /* Write query notification first (thread root) */
            char query_id[256];
            snprintf(query_id, sizeof(query_id), "agentq_%s", task_id + 6);
            mailbox_write_query(mailbox_dir, query_id, NULL,
                                a->workspace_name, NULL,
                                "root", "agent", a->id);

            /* Prefix result with workspace and agent name */
            str_t prefixed = str_new(strlen(pargs.result_text) + 128);
            if (a->workspace_name && a->workspace_name[0])
                str_appendf(&prefixed, "[workspace: %s]", a->workspace_name);
            str_appendf(&prefixed, "%s[agent: %s]\n\n",
                        prefixed.len > 0 ? " " : "", a->id);
            str_append_cstr(&prefixed, pargs.result_text);
            mailbox_write_result_routed(mailbox_dir, task_id,
                                        prefixed.data, NULL,
                                        a->workspace_name, NULL);
            str_free(&prefixed);
        }
        free(pargs.result_text);
        free(pargs.last_session_dir);

        char durbuf[32];
        fmt_duration(dur, durbuf, sizeof(durbuf));
        fprintf(stderr, "[agent] %s %s (%s, %s)\n",
                pargs.playbook_ok ? "✓" : "✗",
                a->id, status, durbuf);

        if (pargs.playbook_ok) n_ok++; else n_fail++;

        playbook_free(pb);
        workspace_free(agent_ws);
    }

    fprintf(stderr, "[agent] done: %d scanned, %d due, %d ok, %d failed\n",
            q->n_agents, q->n_due, n_ok, n_fail);

    return n_fail;
}

/* ── Unified scan + schedule + execute + save ─────────── */

int agent_run_due(const char *nash_dir, store_t *shared_store, config_t *cfg,
                  provider_t *provider, const char *server_model,
                  const char *force_id,
                  volatile sig_atomic_t *shutdown_flag,
                  const char *mailbox_dir) {
    agent_queue_t *q = agent_scan(nash_dir);
    if (!q) {
        fprintf(stderr, "[agent] error: scan failed\n");
        return -1;
    }
    agent_queue_load(q, nash_dir);
    agent_queue_schedule(q, time(NULL));

    int n_fail = agent_execute(q, nash_dir, shared_store, cfg,
                               provider, server_model,
                               force_id, shutdown_flag, mailbox_dir);
    agent_queue_save(q, nash_dir);
    agent_queue_free(q);
    return n_fail;
}

/* ── Queue cleanup ────────────────────────────────────── */

void agent_queue_free(agent_queue_t *q) {
    if (!q) return;
    for (int i = 0; i < q->n_agents; i++) {
        agent_entry_t *a = &q->agents[i];
        free(a->id);
        free(a->workspace_name);
        free(a->agent_file);
        free(a->workspace_dir);
        free(a->description);
        free(a->schedule_str);
        free(a->last_status);
    }
    free(q->agents);
    free(q);
}
