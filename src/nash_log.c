#include "nash_log.h"
#include "tui.h"
#include "ui_state.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

/* ── Global state ─────────────────────────────────────────── */

static journal_t *g_log_journal;
static store_t   *g_log_store;
static ui_state_t *g_log_ui;
static int         g_log_react_loop;
static int         g_log_step;

void nash_log_init(journal_t *journal, store_t *store) {
    g_log_journal = journal;
    g_log_store   = store;
}

void nash_log_set_context(int react_loop, int step) {
    g_log_react_loop = react_loop;
    g_log_step       = step;
}

void nash_log_set_ui(void *ui) {
    g_log_ui = (ui_state_t *)ui;
}

void nash_log(const char *fmt, ...) {
    /* Format the message */
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    /* Strip trailing newline for clean storage */
    int len = (int)strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r'))
        buf[--len] = '\0';

    if (!g_tui_active) {
        /* Not in TUI mode — print to stderr as before */
        fprintf(stderr, "%s\n", buf);
        return;
    }

    /* TUI mode: store + journal + refresh */

    /* 1. Save message to .store/ for audit trail */
    char *ref = NULL;
    if (g_log_store) {
        ref = store_save(g_log_store, buf);
    }

    /* 2. Append journal entry so it appears in reactRx.md */
    if (g_log_journal) {
        cJSON *params = cJSON_CreateObject();
        cJSON_AddStringToObject(params, "message", buf);
        journal_append(g_log_journal, g_log_react_loop, g_log_step,
                       "log", params, ref,
                       (size_t)len, 1, buf,  /* error = message text */
                       NULL);
        cJSON_Delete(params);
    }

    /* 3. Trigger TUI refresh so error is visible immediately */
    if (g_log_ui) {
        pthread_mutex_lock(&g_log_ui->mtx);
        ui_state_generate_react_md(g_log_ui, g_log_ui->current_react_loop);
        g_log_ui->dirty = 1;
        pthread_mutex_unlock(&g_log_ui->mtx);
    }

    free(ref);
}
