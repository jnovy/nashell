/*
 * ui_event.c — React event handler for the UI state.
 *
 * Extracted from ui_state.c.  Contains:
 *   - ui_state_on_event()  (handles 7 event types from the react loop)
 *   - Supporting helpers: viewing_react_file, auto_scroll_bottom
 */

#include "ui_state_internal.h"

/* ── Local helpers ───────────────────────────────────────── */

/* Check if user is currently viewing the given react loop's file.
 * When session_dir is non-NULL, also verify the file is in the
 * correct directory (critical for per-pass playbooks where multiple
 * passes use reactR0.md in different session directories). */
static int viewing_react_file(ui_state_t *ui, int react_loop,
                               const char *session_dir) {
    if (!ui->current_filepath) return 0;
    char expected[64];
    snprintf(expected, sizeof(expected), "reactR%d.md", react_loop);
    const char *base = strrchr(ui->current_filepath, '/');
    base = base ? base + 1 : ui->current_filepath;
    if (strcmp(base, expected) != 0) return 0;
    /* Basename matches — if session_dir given, verify full path */
    if (session_dir) {
        char full[NASH_PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", session_dir, expected);
        return strcmp(ui->current_filepath, full) == 0;
    }
    return 1;
}

/* ── React event handler ─────────────────────────────────── */

void ui_state_on_event(const react_event_t *ev, void *userdata) {
    ui_state_t *ui = (ui_state_t *)userdata;
    if (!ui) return;

    /* Track playbook session provenance so we read journal/react files
     * from the correct directory instead of ui->session_dir. */
    int pass_dir_changed = 0;
    if (ev->session_dir) {
        if (!ui->playbook_session_dir ||
            strcmp(ui->playbook_session_dir, ev->session_dir) != 0) {
            free(ui->playbook_session_dir);
            ui->playbook_session_dir = strdup(ev->session_dir);
            pass_dir_changed = 1;
        }
        ui->playbook_react_loop = ev->react_loop;
        /* NOTE: Do NOT set current_react_loop here — let the
         * loop_changed check below handle it so we can detect
         * transitions and trigger auto-navigation properly. */

        /* Accumulate pass info for session.md rendering.
         * Each playbook pass gets a unique (session_dir, react_loop) pair.
         * For per-pass mode, session_dir changes each pass.
         * For shared mode, session_dir stays the same but react_loop increments. */
        if (ev->pass_index >= 0) {
            int found = 0;
            for (int i = 0; i < ui->pb_pass_count; i++) {
                if (ui->pb_passes[i].pass_index == ev->pass_index) {
                    /* Update existing — session_dir/react_loop may have changed */
                    if (strcmp(ui->pb_passes[i].session_dir, ev->session_dir) != 0) {
                        free(ui->pb_passes[i].session_dir);
                        ui->pb_passes[i].session_dir = strdup(ev->session_dir);
                    }
                    ui->pb_passes[i].react_loop = ev->react_loop;
                    found = 1;
                    break;
                }
            }
            if (!found) {
                if (ui->pb_pass_count >= ui->pb_pass_cap) {
                    ui->pb_pass_cap = ui->pb_pass_cap ? ui->pb_pass_cap * 2 : 8;
                    ui->pb_passes = realloc(ui->pb_passes,
                                             (size_t)ui->pb_pass_cap * sizeof(pb_pass_info_t));
                }
                pb_pass_info_t *pi = &ui->pb_passes[ui->pb_pass_count++];
                pi->session_dir = strdup(ev->session_dir);
                pi->pass_label = ev->pass_label ? strdup(ev->pass_label) : NULL;
                pi->react_loop = ev->react_loop;
                pi->pass_index = ev->pass_index;
            }
        }
    }

    /* Sync current_react_loop from event — critical after checkpoint restore
     * which may change the react loop on the inference thread while the UI
     * still tracks the pre-restore loop number set by main.c.
     *
     * FIX: removed `ev->react_loop > 0` guard that prevented loop_changed
     * from firing for R0 (react_loop==0).  The guard was originally there
     * to avoid overwriting -1 with 0, but main.c already pre-sets the
     * value. The guard made auto-navigation dead code for R0. */
    int loop_changed = 0;
    if (ev->react_loop >= 0 && ev->react_loop != ui->current_react_loop) {
        ui->current_react_loop = ev->react_loop;
        loop_changed = 1;
    }

    /* Effective session dir: use playbook's if active, else main */
    const char *eff_session_dir = ui->playbook_session_dir
                                ? ui->playbook_session_dir
                                : ui->session_dir;

    switch (ev->type) {
    case REACT_EVENT_STEP_START: {
        char buf[128];
        if (ev->pass_label && ev->pass_total > 0) {
            if (ev->max_steps > 0)
                snprintf(buf, sizeof(buf), "[%d/%d %s] step %d/%d...",
                         ev->pass_index + 1, ev->pass_total,
                         ev->pass_label, ev->step, ev->max_steps);
            else
                snprintf(buf, sizeof(buf), "[%d/%d %s] step %d...",
                         ev->pass_index + 1, ev->pass_total,
                         ev->pass_label, ev->step);
        } else if (ev->pass_label) {
            if (ev->max_steps > 0)
                snprintf(buf, sizeof(buf), "[%s] step %d/%d...",
                         ev->pass_label, ev->step, ev->max_steps);
            else
                snprintf(buf, sizeof(buf), "[%s] step %d...",
                         ev->pass_label, ev->step);
        } else {
            if (ev->max_steps > 0)
                snprintf(buf, sizeof(buf), "Running step %d/%d...",
                         ev->step, ev->max_steps);
            else
                snprintf(buf, sizeof(buf), "Running step %d...", ev->step);
        }
        ui->status = STATUS_RUNNING;
        free(ui->status_text);
        ui->status_text = strdup(buf);
        ui->current_step = ev->step;
        ui->max_steps = ev->max_steps;
        if (ev->context_size > 0)
            ui->context_size = ev->context_size;
        /* Clear streaming tokens and tool execution state for new step */
        if (ui->stream_tokens) ui->stream_tokens[0] = '\0';
        ui->stream_len = 0;
        ui->tool_executing = 0;

        /* Initialize streaming progress timing for this step */
        clock_gettime(CLOCK_MONOTONIC, &ui->stream_step_start);
        ui->stream_first_token_seen = 0;
        ui->stream_token_count = 0;
        ui->prompt_progress_processed = 0;
        ui->prompt_progress_total = 0;

        /* Defer react MD + session MD regeneration to the main loop.
         * Previously these expensive file I/O operations ran here under
         * ui->mtx, causing mutex starvation that froze the TUI. */
        ui->needs_react_regen = 1;

        /* Reset cumulative stats and user_scrolled on first step of a
         * new react loop (or after checkpoint restore changed the loop)
         * so stats start fresh and auto-scroll is active. */
        if (ev->step == 0 || loop_changed) {
            ui->user_scrolled = 0;
            ui->cum_prompt_tokens = 0;
            ui->cum_completion_tokens = 0;
            ui->cum_predicted_per_second = 0;
            ui->cum_prompt_per_second = 0;
            ui->react_total_elapsed = 0;
            ui->cum_llm_steps = 0;
            ui->react_done = 0;
        }

        /* Auto-navigate into reactRX.md on first step, after checkpoint
         * restore changed the loop, or when a new playbook pass starts
         * in a different session directory (per-pass mode).
         *
         * Use ev->step <= 1 because react.c emits step+1 (1-based), so
         * the first step of any react loop has ev->step == 1, not 0.
         * The old ev->step == 0 condition was dead code.
         *
         * pass_dir_changed catches per-pass playbook transitions where
         * both passes use reactR0.md but in different directories. */
        if ((ev->step <= 1 || loop_changed || pass_dir_changed) &&
            !viewing_react_file(ui, ui->current_react_loop, eff_session_dir)) {
            if (ui->nav_depth >= ui->nav_cap) {
                ui->nav_cap = ui->nav_cap ? ui->nav_cap * 2 : 16;
                ui->nav_stack = realloc(ui->nav_stack,
                                         (size_t)ui->nav_cap * sizeof(nav_entry_t));
            }
            nav_entry_t *ne = &ui->nav_stack[ui->nav_depth];
            ne->filepath = ui->current_filepath ? strdup(ui->current_filepath) : NULL;
            ne->label = ui->current_label;  /* transfer ownership */
            ui->current_label = NULL;
            ne->scroll_y = ui->scroll_y;
            ne->scroll_x = ui->scroll_x;
            ne->cursor_link = ui->cursor_link;
            ne->saved_doc = NULL;
            ui->nav_depth++;

            char rpath[NASH_PATH_MAX];
            snprintf(rpath, sizeof(rpath), "%s/reactR%d.md",
                     eff_session_dir, ui->current_react_loop);
            free(ui->current_filepath);
            ui->current_filepath = strdup(rpath);
            ui->scroll_y = 0;
            ui->scroll_x = 0;
            ui->cursor_link = 0;
            ui->focus = FOCUS_JOURNAL;
        }

        /* Defer session.md update too */
        ui->needs_session_regen = 1;
        ui->needs_file_reload = 1;
        break;
    }

    case REACT_EVENT_LLM_TOKEN:
        if (ev->token && ev->token[0]) {
            int tlen = (int)strlen(ev->token);
            if (ui->stream_len + tlen >= ui->stream_cap - 1) {
                ui->stream_cap = (ui->stream_len + tlen + 1) * 2;
                ui->stream_tokens = realloc(ui->stream_tokens,
                                             (size_t)ui->stream_cap);
            }
            memcpy(ui->stream_tokens + ui->stream_len, ev->token, (size_t)tlen);
            ui->stream_len += tlen;
            ui->stream_tokens[ui->stream_len] = '\0';

            /* Track streaming progress: token count + first-token timing */
            ui->stream_token_count++;
            if (!ui->stream_first_token_seen) {
                clock_gettime(CLOCK_MONOTONIC, &ui->stream_first_token);
                ui->stream_first_token_seen = 1;
            }

            /* Defer react MD regeneration to the main loop.
             * The token buffer (ui->stream_tokens) is updated above on
             * every callback — only the expensive file generation is
             * deferred, keeping mutex hold time minimal. */
            ui->needs_react_regen = 1;
            ui->needs_file_reload = 1;
        }
        break;

    case REACT_EVENT_TOOL_START: {
        const char *action = ev->action ? ev->action : "?";
        const char *desc = ev->description ? ev->description : "";
        int dlen = (int)strlen(desc);

        /* Status bar: truncate long descriptions to fit the small bar */
        char sbuf[256];
        if (dlen > 80) {
            snprintf(sbuf, sizeof(sbuf), "[step %d] %s: %.*s...",
                     ev->step, action,
                     (int)utf8_clamp(desc, 77), desc);
        } else {
            snprintf(sbuf, sizeof(sbuf), "[step %d] %s: %s",
                     ev->step, action, desc);
        }
        free(ui->status_text);
        ui->status_text = strdup(sbuf);

        /* Full (untruncated) version for stream_tokens and tool_display
         * so the main content area and live progress show the complete
         * tool arguments without truncation (like done output). */
        char *full = NULL;
        int flen = asprintf(&full, "[step %d] %s: %s", ev->step, action, desc);
        if (flen < 0) { full = strdup(sbuf); flen = (int)strlen(full); }

        /* Show tool command in streaming area (replaces thinking text)
         * so the user sees what tool is about to run in the main content,
         * not just in the small status bar. */
        if (flen >= ui->stream_cap - 1) {
            ui->stream_cap = flen + 2;
            ui->stream_tokens = realloc(ui->stream_tokens,
                                         (size_t)ui->stream_cap);
        }
        memcpy(ui->stream_tokens, full, (size_t)flen + 1);
        ui->stream_len = flen;
        /* Track tool execution state for live elapsed-time display.
         * The main loop forces periodic regen while tool_executing=1,
         * so the elapsed time counter updates even though no events
         * fire during tool_execute(). */
        ui->tool_executing = 1;
        ui->tool_timeout_secs = ev->tool_timeout;
        clock_gettime(CLOCK_MONOTONIC, &ui->tool_start_time);
        free(ui->tool_display);
        ui->tool_display = full;  /* transfer ownership */
        ui->needs_react_regen = 1;
        ui->needs_file_reload = 1;
        break;
    }

    case REACT_EVENT_STEP_COMPLETE:
    case REACT_EVENT_TOOL_OUTPUT:
        /* NOTE: Do NOT call nash_log() here — this handler runs with
         * ui->mtx already held (via threaded_event_cb in main.c).
         * nash_log() tries to lock the same mutex → deadlock. */
        if (ev->stats.prompt_tokens > 0) {
            ui->context_used = ev->stats.prompt_tokens;
            if (ev->context_size > 0)
                ui->context_size = ev->context_size;
        }
        /* Accumulate token stats for the react loop summary */
        if (ev->type == REACT_EVENT_STEP_COMPLETE &&
            (ev->stats.prompt_tokens > 0 || ev->stats.completion_tokens > 0)) {
            ui->cum_prompt_tokens += ev->stats.prompt_tokens;
            ui->cum_completion_tokens += ev->stats.completion_tokens;
            if (ev->stats.predicted_per_second > 0)
                ui->cum_predicted_per_second = ev->stats.predicted_per_second;
            if (ev->stats.prompt_per_second > 0)
                ui->cum_prompt_per_second = ev->stats.prompt_per_second;
            ui->cum_llm_steps++;
        }
        if (ev->total_elapsed > 0)
            ui->react_total_elapsed = ev->total_elapsed;
        /* Clear streaming tokens and tool execution state */
        if (ui->stream_tokens) ui->stream_tokens[0] = '\0';
        ui->stream_len = 0;
        ui->tool_executing = 0;

        /* Defer expensive file I/O to main loop */
        ui->needs_react_regen = 1;
        ui->needs_session_regen = 1;
        ui->needs_file_reload = 1;
        break;

    case REACT_EVENT_DONE:
        if (ev->result) {
            ui->status = STATUS_DONE;
            free(ui->status_text);
            ui->status_text = strdup("Done");
        } else {
            /* Fatal error — react_run returned NULL.  Keep any prior
             * STATUS_ERROR message from REACT_EVENT_ERROR if present;
             * otherwise set a generic error status. */
            if (ui->status != STATUS_ERROR) {
                ui->status = STATUS_ERROR;
                free(ui->status_text);
                ui->status_text = strdup("Inference failed (no result)");
            }
        }
        if (ui->stream_tokens) ui->stream_tokens[0] = '\0';
        ui->stream_len = 0;
        if (ev->stats.prompt_tokens > 0) {
            ui->context_used = ev->stats.prompt_tokens;
            if (ev->context_size > 0)
                ui->context_size = ev->context_size;
        }
        /* Accumulate final step's token stats */
        if (ev->stats.prompt_tokens > 0 || ev->stats.completion_tokens > 0) {
            ui->cum_prompt_tokens += ev->stats.prompt_tokens;
            ui->cum_completion_tokens += ev->stats.completion_tokens;
            if (ev->stats.predicted_per_second > 0)
                ui->cum_predicted_per_second = ev->stats.predicted_per_second;
            if (ev->stats.prompt_per_second > 0)
                ui->cum_prompt_per_second = ev->stats.prompt_per_second;
            ui->cum_llm_steps++;
        }
        if (ev->total_elapsed > 0)
            ui->react_total_elapsed = ev->total_elapsed;
        ui->react_done = 1;

        /* React loop finished — clear user_scrolled so final
         * auto-scroll shows the completed result. */
        ui->user_scrolled = 0;

        /* Defer expensive file I/O to main loop */
        ui->needs_react_regen = 1;
        ui->needs_session_regen = 1;
        ui->needs_file_reload = 1;
        break;

    case REACT_EVENT_USER_ASK:
        ui->status = STATUS_AWAITING_INPUT;
        free(ui->status_text);
        ui->status_text = strdup("Agent is asking a question — see main pane. Type answer below.");
        /* Store full question for display in main pane */
        free(ui->user_ask_question);
        ui->user_ask_question = (ev->message && ev->message[0])
            ? strdup(ev->message) : strdup("(no question specified)");

        /* Auto-navigate to reactRX.md so the user can see the question.
         * The question is rendered only in reactRX.md (ui_md_gen.c), not
         * in session.md.  Without this, users viewing session.md get the
         * status bar hint "see main pane" but no visible question. */
        if (!viewing_react_file(ui, ui->current_react_loop, eff_session_dir)) {
            if (ui->nav_depth >= ui->nav_cap) {
                ui->nav_cap = ui->nav_cap ? ui->nav_cap * 2 : 16;
                ui->nav_stack = realloc(ui->nav_stack,
                                         (size_t)ui->nav_cap * sizeof(nav_entry_t));
            }
            nav_entry_t *ne = &ui->nav_stack[ui->nav_depth];
            ne->filepath = ui->current_filepath ? strdup(ui->current_filepath) : NULL;
            ne->label = ui->current_label;  /* transfer ownership */
            ui->current_label = NULL;
            ne->scroll_y = ui->scroll_y;
            ne->scroll_x = ui->scroll_x;
            ne->cursor_link = ui->cursor_link;
            ne->saved_doc = NULL;
            ui->nav_depth++;

            char rpath[NASH_PATH_MAX];
            snprintf(rpath, sizeof(rpath), "%s/reactR%d.md",
                     eff_session_dir, ui->current_react_loop);
            free(ui->current_filepath);
            ui->current_filepath = strdup(rpath);
            ui->scroll_y = 0;
            ui->scroll_x = 0;
            ui->cursor_link = 0;
            ui->focus = FOCUS_JOURNAL;
        }

        /* Defer expensive file I/O to main loop */
        ui->needs_react_regen = 1;
        ui->needs_session_regen = 1;
        ui->needs_file_reload = 1;
        break;

    case REACT_EVENT_PROMPT_PROGRESS:
        /* Update server-reported prompt processing progress */
        ui->prompt_progress_processed = ev->prompt_progress_processed;
        ui->prompt_progress_total = ev->prompt_progress_total;
        ui->needs_react_regen = 1;
        ui->needs_file_reload = 1;
        break;

    case REACT_EVENT_ERROR:
        /* Surface error message in the status bar so the user sees it */
        if (ev->message && ev->message[0]) {
            ui->status = STATUS_ERROR;
            free(ui->status_text);
            ui->status_text = strdup(ev->message);
        }
        /* Defer expensive file I/O to main loop */
        ui->needs_react_regen = 1;
        ui->needs_file_reload = 1;
        break;

    case REACT_EVENT_WARNING:
        /* Defer expensive file I/O to main loop */
        ui->needs_react_regen = 1;
        ui->needs_file_reload = 1;
        break;
    }

    /* Every event modifies UI state (status, stream tokens, react MD, etc.)
     * so mark dirty to ensure tui_render() actually redraws.
     * Without this, the main loop's `if (ui->dirty) tui_render(ui)` and
     * the 100ms auto-refresh both skip rendering because dirty stays 0,
     * causing the TUI to appear completely frozen during and after inference. */
    ui->dirty = 1;
}
