/*
 * cmd_agents.c — /agent slash-command handlers.
 * Extracted from commands.c for maintainability.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "commands.h"
#include "commands_internal.h"
#include "agents.h"
#include "cJSON.h"
#include "str.h"
#include "nash_limits.h"
#include "tui.h"
#include "ui_state_internal.h"
#include "playbook.h"
#include "workspace.h"

/* ── /agent [list|show|run|history|due|result] ────────────────── */

static int cmd_agents_list(command_ctx_t *ctx) {
    ui_state_t *ui = ctx->ui;

    agent_queue_t *q = agent_scan(ctx->nash_dir);
    if (!q) {
        pthread_mutex_lock(&ui->mtx);
        ui_state_set_status(ui, STATUS_ERROR, "/agent: scan failed");
        pthread_mutex_unlock(&ui->mtx);
        tui_render(ui);
        return CMD_CONTINUE;
    }
    agent_queue_load(q, ctx->nash_dir);
    agent_queue_schedule(q, time(NULL));

    str_t display = str_new(2048);
    str_appendf(&display, "# Agents\n\n");

    if (q->n_agents == 0) {
        str_appendf(&display,
            "No agents found.\n\n"
            "Create agent definitions in "
            "`~/.nash/workspaces/<name>/agent/<agent>.yaml`\n");
    } else {
        str_appendf(&display,
            "| # | Agent | Schedule | Last Run | Status | Due |\n"
            "|---|-------|----------|----------|--------|-----|\n");

        time_t now = time(NULL);
        for (int i = 0; i < q->n_agents; i++) {
            const agent_entry_t *a = &q->agents[i];

            char last_run_str[64];
            if (a->last_run == 0)
                snprintf(last_run_str, sizeof(last_run_str), "never");
            else {
                char durbuf[32];
                fmt_duration_short((double)(now - a->last_run), durbuf, sizeof(durbuf));
                snprintf(last_run_str, sizeof(last_run_str), "%s ago", durbuf);
            }

            char status_str[64];
            if (a->last_run == 0)
                snprintf(status_str, sizeof(status_str), "-");
            else {
                char durbuf[32];
                fmt_duration((double)a->last_duration, durbuf, sizeof(durbuf));
                if (a->last_status && strcmp(a->last_status, "ok") == 0)
                    snprintf(status_str, sizeof(status_str), "ok (%s)", durbuf);
                else
                    snprintf(status_str, sizeof(status_str), "FAIL: %s (%s)",
                             a->last_status ? a->last_status : "?", durbuf);
            }

            str_appendf(&display, "| %d | %s | `%s` | %s | %s | %s |\n",
                        i + 1, a->id, a->schedule_str,
                        last_run_str, status_str,
                        a->is_due ? "**yes**" : "no");
        }

        str_appendf(&display,
            "\n**%d agents**, %d due now\n\n"
            "Commands: `/agent show ID|#`, `/agent run ID|#`, "
            "`/agent history`, `/agent result ID|#`\n",
            q->n_agents, q->n_due);
    }

    char *banner = str_steal(&display);
    pthread_mutex_lock(&ui->mtx);
    ui_state_push_content(ui, "agents", banner);
    ui_state_set_status(ui, STATUS_READY, "Agent list");
    pthread_mutex_unlock(&ui->mtx);
    free(banner);
    tui_render(ui);

    agent_queue_free(q);
    return CMD_CONTINUE;
}

static int cmd_agents_show(command_ctx_t *ctx, const char *id) {
    ui_state_t *ui = ctx->ui;
    while (*id == ' ') id++;

    agent_queue_t *q = agent_scan(ctx->nash_dir);
    if (!q) {
        pthread_mutex_lock(&ui->mtx);
        ui_state_set_status(ui, STATUS_ERROR, "/agent show: scan failed");
        pthread_mutex_unlock(&ui->mtx);
        tui_render(ui);
        return CMD_CONTINUE;
    }
    agent_queue_load(q, ctx->nash_dir);
    agent_queue_schedule(q, time(NULL));

    const agent_entry_t *found = agent_find(q, id);
    if (!found) {
        pthread_mutex_lock(&ui->mtx);
        ui_state_set_status(ui, STATUS_ERROR, "/agent show: agent not found");
        pthread_mutex_unlock(&ui->mtx);
        tui_render(ui);
        agent_queue_free(q);
        return CMD_CONTINUE;
    }

    str_t display = str_new(2048);
    str_appendf(&display, "# Agent: %s\n\n", found->id);
    if (found->summary && found->summary[0])
        str_appendf(&display, "%s\n\n", found->summary);
    str_appendf(&display, "**Workspace**: %s  \n", found->workspace_name);
    str_appendf(&display, "**File**: `%s`  \n", found->agent_file);
    str_appendf(&display, "**Schedule**: `%s`  \n", found->schedule_str);
    str_appendf(&display, "**Timeout**: %ds  \n", found->timeout);
    str_appendf(&display, "**Enabled**: %s  \n", found->enabled ? "yes" : "no");
    str_appendf(&display, "**Due now**: %s  \n\n", found->is_due ? "**yes**" : "no");

    if (found->last_run > 0) {
        char timebuf[64];
        struct tm tm;
        localtime_r(&found->last_run, &tm);
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm);
        char durbuf[32];
        fmt_duration((double)found->last_duration, durbuf, sizeof(durbuf));
        str_appendf(&display,
            "## Last Run\n"
            "**Time**: %s  \n"
            "**Duration**: %s  \n"
            "**Status**: %s  \n\n",
            timebuf, durbuf,
            found->last_status ? found->last_status : "?");
    } else {
        str_appendf(&display, "## Last Run\nNever executed\n\n");
    }

    if (found->next_due > 0 && !found->schedule.is_startup) {
        char timebuf[64];
        struct tm tm;
        localtime_r(&found->next_due, &tm);
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm);
        str_appendf(&display, "**Next due**: %s\n\n", timebuf);
    }

    if (found->description && found->description[0])
        str_appendf(&display, "## Usage\n%s\n\n", found->description);

    str_appendf(&display, "\n---\n`/agent run %s` to execute now  \n", found->id);
    str_appendf(&display, "`/agent result %s` to view latest output\n", found->id);

    char *banner = str_steal(&display);
    pthread_mutex_lock(&ui->mtx);
    ui_state_push_content(ui, "agent-detail", banner);
    ui_state_set_status(ui, STATUS_READY, "Agent detail");
    pthread_mutex_unlock(&ui->mtx);
    free(banner);
    tui_render(ui);

    agent_queue_free(q);
    return CMD_CONTINUE;
}

static int cmd_agents_run(command_ctx_t *ctx, const char *id) {
    ui_state_t *ui = ctx->ui;
    while (*id == ' ') id++;

    if (*ctx->inferring) {
        pthread_mutex_lock(&ui->mtx);
        ui_state_set_status(ui, STATUS_ERROR,
            "Wait for inference to finish before running an agent");
        pthread_mutex_unlock(&ui->mtx);
        tui_render(ui);
        return CMD_CONTINUE;
    }

    /* Split "agent_id arg1 arg2 ..." into agent_id + arguments.
     * Numeric IDs are a single token; named IDs may contain slashes
     * but not spaces, so the first space after the ID starts arguments. */
    char *id_buf = strdup(id);
    const char *agent_arguments = NULL;
    char *sp = strchr(id_buf, ' ');
    if (sp) {
        *sp = '\0';
        agent_arguments = sp + 1;
        while (*agent_arguments == ' ') agent_arguments++;
        if (*agent_arguments == '\0') agent_arguments = NULL;
    }

    agent_queue_t *q = agent_scan(ctx->nash_dir);
    if (!q) {
        pthread_mutex_lock(&ui->mtx);
        ui_state_set_status(ui, STATUS_ERROR, "/agent run: scan failed");
        pthread_mutex_unlock(&ui->mtx);
        tui_render(ui);
        free(id_buf);
        return CMD_CONTINUE;
    }
    agent_queue_load(q, ctx->nash_dir);
    agent_queue_schedule(q, time(NULL));

    const agent_entry_t *found = agent_find(q, id_buf);
    if (!found) {
        pthread_mutex_lock(&ui->mtx);
        ui_state_set_status(ui, STATUS_ERROR, "/agent run: agent not found");
        pthread_mutex_unlock(&ui->mtx);
        tui_render(ui);
        agent_queue_free(q);
        free(id_buf);
        return CMD_CONTINUE;
    }

    playbook_t *pb = agent_prepare_playbook(found, agent_arguments);
    if (!pb) {
        pthread_mutex_lock(&ui->mtx);
        ui_state_set_status(ui, STATUS_ERROR,
            "/agent run: cannot load agent playbook");
        pthread_mutex_unlock(&ui->mtx);
        tui_render(ui);
        agent_queue_free(q);
        free(id_buf);
        return CMD_CONTINUE;
    }

    /* Create workspace for agent -- two-layer memory (workspace first, global fallback) */
    workspace_t *agent_ws = NULL;
    if (found->workspace_name && found->workspace_name[0]) {
        agent_ws = workspace_new(ctx->nash_dir, found->workspace_name,
                                 0, ctx->cfg->workspace_global_weight);
        if (agent_ws) {
            workspace_set_recall_config(agent_ws, ctx->cfg->recall_min_score,
                                        ctx->cfg->recall_blend_semantic,
                                        ctx->cfg->recall_blend_substring,
                                        ctx->cfg->vscore_exponent);
            if (ctx->cfg->embedding.type &&
                strcmp(ctx->cfg->embedding.type, "none") != 0)
                workspace_init_embeddings(agent_ws, ctx->cfg->embedding.type,
                                          ctx->cfg->embedding.model,
                                          ctx->cfg->embedding.api_base,
                                          ctx->cfg->embedding.model_path,
                                          ctx->cfg->embedding.dimension,
                                          ctx->cfg->embedding.max_input_chars);
        }
    }

    *ctx->pargs = (playbook_args_t){
        .playbook     = pb,
        .nash_dir     = (char *)ctx->nash_dir,
        .store        = ctx->store,
        .memory       = agent_ws ? agent_ws->global : ctx->memory,
        .cfg          = ctx->cfg,
        .provider     = ctx->provider,
        .server_model = (char *)ctx->server_model,
        .ui           = ui,
        .playbook_ok      = 0,
        .done             = 0,
        .agent_id         = strdup(found->id),
        .agent_start_time = time(NULL),
        .workspace_override = found->workspace_name ? strdup(found->workspace_name) : NULL,
        .agent_ws         = agent_ws,
    };

    ctx->provider->abort_retry = 0;
    pthread_create(ctx->infer_tid, NULL, playbook_worker, ctx->pargs);
    *ctx->inferring = INFER_PLAYBOOK;

    pthread_mutex_lock(&ui->mtx);
    ui->agent_view = 1;     /* Mark agent run active for UI state tracking */
    ui->agent_running = 1;   /* Prevent Escape from clearing agent_view mid-run */
    char msg[256];
    snprintf(msg, sizeof(msg), "Running agent: %s", found->id);
    ui_state_set_status(ui, STATUS_RUNNING, msg);
    pthread_mutex_unlock(&ui->mtx);
    tui_render(ui);

    agent_queue_free(q);
    free(id_buf);
    return CMD_CONTINUE;
}

static int cmd_agents_history(command_ctx_t *ctx, const char *filter_id) {
    ui_state_t *ui = ctx->ui;
    if (filter_id) while (*filter_id == ' ') filter_id++;

    char path[NASH_PATH_MAX];
    snprintf(path, sizeof(path), "%s/agent/history.jsonl", ctx->nash_dir);

    FILE *f = fopen(path, "r");
    if (!f) {
        str_t display = str_new(256);
        str_appendf(&display, "# Agent History\n\nNo history yet.\n");
        char *banner = str_steal(&display);
        pthread_mutex_lock(&ui->mtx);
        ui_state_push_content(ui, "agent-history", banner);
        ui_state_set_status(ui, STATUS_READY, "Agent history");
        pthread_mutex_unlock(&ui->mtx);
        free(banner);
        tui_render(ui);
        return CMD_CONTINUE;
    }

    str_t display = str_new(4096);
    if (filter_id && *filter_id)
        str_appendf(&display, "# Agent History: %s\n\n", filter_id);
    else
        str_appendf(&display, "# Agent History\n\n");

    str_appendf(&display,
        "| Time | Agent | Status | Duration |\n"
        "|------|-------|--------|----------|\n");

    char line[4096];
    char *lines[256];
    int n_lines = 0;
    while (fgets(line, sizeof(line), f) && n_lines < 256) {
        if (filter_id && *filter_id) {
            if (!strstr(line, filter_id)) continue;
        }
        lines[n_lines++] = strdup(line);
    }
    fclose(f);

    /* Display newest first (last 50) */
    int start = n_lines > 50 ? n_lines - 50 : 0;
    for (int i = n_lines - 1; i >= start; i--) {
        cJSON *ev = cJSON_Parse(lines[i]);
        if (!ev) { free(lines[i]); continue; }

        const char *aid = cJSON_GetStringValue(cJSON_GetObjectItem(ev, "id"));
        const char *st  = cJSON_GetStringValue(cJSON_GetObjectItem(ev, "st"));
        double ts_d = 0;
        cJSON *ts_item = cJSON_GetObjectItem(ev, "ts");
        if (ts_item) ts_d = cJSON_GetNumberValue(ts_item);
        int dur = 0;
        cJSON *dur_item = cJSON_GetObjectItem(ev, "dur");
        if (dur_item) dur = (int)cJSON_GetNumberValue(dur_item);

        char timebuf[64];
        time_t t = (time_t)ts_d;
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(timebuf, sizeof(timebuf), "%m-%d %H:%M", &tm);

        char durbuf[32];
        fmt_duration((double)dur, durbuf, sizeof(durbuf));

        char status_str[128];
        if (st && strcmp(st, "ok") == 0)
            snprintf(status_str, sizeof(status_str), "ok");
        else
            snprintf(status_str, sizeof(status_str), "FAIL: %s", st ? st : "?");

        str_appendf(&display, "| %s | %s | %s | %s |\n",
                    timebuf,
                    aid ? aid : "?",
                    status_str,
                    durbuf);

        cJSON_Delete(ev);
        free(lines[i]);
    }
    for (int i = 0; i < start; i++) free(lines[i]);

    if (n_lines == 0)
        str_appendf(&display, "\nNo history entries%s\n",
                    (filter_id && *filter_id) ? " for this agent" : "");
    else
        str_appendf(&display, "\nShowing %d of %d entries\n",
                    n_lines - start, n_lines);

    char *banner = str_steal(&display);
    pthread_mutex_lock(&ui->mtx);
    ui_state_push_content(ui, "agent-history", banner);
    ui_state_set_status(ui, STATUS_READY, "Agent history");
    pthread_mutex_unlock(&ui->mtx);
    free(banner);
    tui_render(ui);

    return CMD_CONTINUE;
}

static int cmd_agents_due(command_ctx_t *ctx) {
    ui_state_t *ui = ctx->ui;

    agent_queue_t *q = agent_scan(ctx->nash_dir);
    if (!q) {
        pthread_mutex_lock(&ui->mtx);
        ui_state_set_status(ui, STATUS_ERROR, "/agent due: scan failed");
        pthread_mutex_unlock(&ui->mtx);
        tui_render(ui);
        return CMD_CONTINUE;
    }
    agent_queue_load(q, ctx->nash_dir);
    agent_queue_schedule(q, time(NULL));

    str_t display = str_new(1024);
    str_appendf(&display, "# Agents Due Now\n\n");

    int n = 0;
    for (int i = 0; i < q->n_agents; i++) {
        if (!q->agents[i].is_due) continue;
        n++;
        const agent_entry_t *a = &q->agents[i];
        char since[64] = "never run";
        if (a->last_run > 0) {
            char durbuf[32];
            fmt_duration_short((double)(time(NULL) - a->last_run), durbuf, sizeof(durbuf));
            snprintf(since, sizeof(since), "%s ago", durbuf);
        }
        str_appendf(&display, "%d. **%s** — schedule: `%s`, last: %s, timeout: %ds\n",
                    n, a->id, a->schedule_str, since, a->timeout);
    }

    if (n == 0)
        str_appendf(&display, "No agents are due right now.\n");
    else
        str_appendf(&display,
            "\n`/agent run ID` to run one, or `nash --agent` from CLI to run all.\n");

    char *banner = str_steal(&display);
    pthread_mutex_lock(&ui->mtx);
    ui_state_push_content(ui, "agents-due", banner);
    ui_state_set_status(ui, STATUS_READY, n > 0 ? "Agents due" : "No agents due");
    pthread_mutex_unlock(&ui->mtx);
    free(banner);
    tui_render(ui);

    agent_queue_free(q);
    return CMD_CONTINUE;
}

static int cmd_agents_result(command_ctx_t *ctx, const char *id) {
    ui_state_t *ui = ctx->ui;
    while (*id == ' ') id++;

    /* Try exact ID first, then resolve via scan */
    char path[NASH_PATH_MAX];
    snprintf(path, sizeof(path),
             "%s/agent/results/%s/latest.md", ctx->nash_dir, id);

    size_t clen = 0;
    char *content = slurp_file(path, &clen);
    if (!content) {
        /* Suffix resolve: scan to find full agent ID */
        agent_queue_t *q = agent_scan(ctx->nash_dir);
        if (q) {
            agent_queue_load(q, ctx->nash_dir);
            agent_queue_schedule(q, time(NULL));
            const agent_entry_t *found = agent_find(q, id);
            if (found) {
                snprintf(path, sizeof(path),
                         "%s/agent/results/%s/latest.md", ctx->nash_dir, found->id);
                content = slurp_file(path, &clen);
            }
            agent_queue_free(q);
        }
    }

    if (!content) {
        pthread_mutex_lock(&ui->mtx);
        ui_state_set_status(ui, STATUS_ERROR,
            "/agent result: no result found (agent never run or ID wrong)");
        pthread_mutex_unlock(&ui->mtx);
        tui_render(ui);
        return CMD_CONTINUE;
    }

    pthread_mutex_lock(&ui->mtx);
    ui_state_push_content(ui, "agent-result", content);
    ui_state_set_status(ui, STATUS_READY, "Agent result");
    pthread_mutex_unlock(&ui->mtx);
    free(content);
    tui_render(ui);

    return CMD_CONTINUE;
}

int cmd_agents(command_ctx_t *ctx, const char *args) {
    while (*args == ' ') args++;

    /* Default: /agent with no args → list */
    if (*args == '\0' || strcmp(args, "list") == 0) {
        return cmd_agents_list(ctx);
    }
    if (strncmp(args, "show ", 5) == 0) {
        return cmd_agents_show(ctx, args + 5);
    }
    if (strncmp(args, "run ", 4) == 0) {
        return cmd_agents_run(ctx, args + 4);
    }
    if (strcmp(args, "history") == 0) {
        return cmd_agents_history(ctx, NULL);
    }
    if (strncmp(args, "history ", 8) == 0) {
        return cmd_agents_history(ctx, args + 8);
    }
    if (strcmp(args, "due") == 0) {
        return cmd_agents_due(ctx);
    }
    if (strncmp(args, "result ", 7) == 0) {
        return cmd_agents_result(ctx, args + 7);
    }

    ui_state_t *ui = ctx->ui;
    pthread_mutex_lock(&ui->mtx);
    ui_state_set_status(ui, STATUS_ERROR,
        "/agent: unknown subcommand (list|show|run|history|due|result)");
    pthread_mutex_unlock(&ui->mtx);
    tui_render(ui);
    return CMD_CONTINUE;
}
