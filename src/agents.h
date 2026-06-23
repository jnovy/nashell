#ifndef AGENTS_H
#define AGENTS_H

#include "playbook.h"
#include "workspace.h"
#include "config.h"
#include <time.h>
#include <signal.h>

/* ── Cron schedule (parsed from "M H D Mo DoW" string) ─ */
typedef struct {
    /* Bitmask arrays: 1 = this value matches */
    unsigned char minute[60];
    unsigned char hour[24];
    unsigned char dom[31];    /* day of month 1-31 (index 0=day1) */
    unsigned char month[12];  /* 1-12 (index 0=Jan) */
    unsigned char dow[7];     /* 0-6, 0=Sun */
    int is_startup;           /* @startup: always due */
} agent_schedule_t;

/* ── Single agent entry (discovered from workspace) ──── */
typedef struct {
    char *id;               /* "workspace_name/agent_name" */
    char *workspace_name;   /* workspace path component (may be nested: rh/container-tools) */
    char *agent_file;       /* full path to YAML */
    char *workspace_dir;    /* full path to workspace dir */

    /* Parsed from YAML */
    agent_schedule_t schedule;
    char *schedule_str;     /* original cron string */
    int   timeout;          /* seconds, 0 = no limit */
    int   enabled;

    /* From queue.json (persisted state) */
    time_t last_run;
    int    last_duration;   /* seconds */
    char  *last_status;     /* "ok", "fail", "timeout", "never" */

    /* Computed */
    time_t next_due;
    int    is_due;          /* 1 if next_due <= now */
} agent_entry_t;

/* ── Agent queue (full calendar) ───────────────────── */
typedef struct {
    agent_entry_t *agents;
    int n_agents;
    int n_due;
    time_t built_at;
} agent_queue_t;

/* ── API ───────────────────────────────────────────── */

/* Scan all workspaces for agent definitions.
 * Returns heap-allocated queue (caller frees with agent_queue_free). */
agent_queue_t *agent_scan(const char *nash_dir);

/* Load persisted state from queue.json, merge with scanned agents. */
int agent_queue_load(agent_queue_t *q, const char *nash_dir);

/* Save queue state to queue.json (atomic write). */
int agent_queue_save(const agent_queue_t *q, const char *nash_dir);

/* Compute next_due for all agents, mark is_due, sort by next_due. */
void agent_queue_schedule(agent_queue_t *q, time_t now);

/* Parse cron string into schedule struct. Returns 0 on success. */
int agent_parse_schedule(const char *cron_str, agent_schedule_t *sched);

/* Compute next occurrence after `after` for the given schedule. */
time_t agent_next_occurrence(const agent_schedule_t *sched, time_t after);

/* Append to history.jsonl */
int agent_history_append(const char *nash_dir, const agent_entry_t *agent,
                         int duration, const char *status,
                         const char *session_id);

/* Free queue and all entries. */
void agent_queue_free(agent_queue_t *q);

/* Print agent list table to FILE*. */
void agent_queue_print(const agent_queue_t *q, FILE *out);

/* Execute all due agents. Returns number of failures. */
int agent_execute(agent_queue_t *q, const char *nash_dir,
                  store_t *shared_store, config_t *cfg,
                  provider_t *provider, const char *server_model,
                  const char *force_id,
                  volatile sig_atomic_t *shutdown_flag);

#endif /* AGENTS_H */
