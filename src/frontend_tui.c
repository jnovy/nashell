#include "react_event.h"
#include "str.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* ── Simple TUI frontend ─────────────────────────────── */
/* Reproduces the original terminal output via react events */

static void read_and_print_store_file(const char *session_dir, const char *ref) {
    if (!session_dir || !ref) return;
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", session_dir, ref);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char buf[4096];
    size_t total = 0;
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf) - 1, f)) > 0 && total < 8000) {
        buf[n] = '\0';
        fprintf(stderr, "%s", buf);
        total += n;
    }
    if (total > 0 && buf[n > 0 ? n - 1 : 0] != '\n')
        fprintf(stderr, "\n");
    if (total >= 8000)
        fprintf(stderr, "  ... (truncated at 8K)\n");
    fclose(f);
}

void tui_on_event(const react_event_t *ev, void *userdata) {
    const char *session_dir = (const char *)userdata;

    switch (ev->type) {

    case REACT_EVENT_STEP_START:
        fprintf(stderr, "\r\033[K[step %d/%d] thinking...",
                ev->step, ev->max_steps);
        fflush(stderr);
        break;

    case REACT_EVENT_LLM_TOKEN:
        /* Streaming: print tokens as they arrive */
        if (ev->token) {
            fprintf(stderr, "%s", ev->token);
            fflush(stderr);
        }
        break;

    case REACT_EVENT_STEP_COMPLETE: {
        /* Build stats suffix */
        char stats_buf[128] = "";
        if (ev->stats.prompt_tokens > 0 || ev->stats.completion_tokens > 0) {
            char pp_str[32] = "", gen_str[32] = "", ctx_str[32] = "";
            if (ev->stats.prompt_per_second > 0)
                snprintf(pp_str, sizeof(pp_str), " | pp %.0f t/s",
                         ev->stats.prompt_per_second);
            if (ev->stats.predicted_per_second > 0)
                snprintf(gen_str, sizeof(gen_str), " | gen %.0f t/s",
                         ev->stats.predicted_per_second);
            if (ev->context_size > 0 && ev->stats.prompt_tokens > 0) {
                int pct = (int)(100.0 * ev->stats.prompt_tokens / ev->context_size);
                snprintf(ctx_str, sizeof(ctx_str), " | ctx %d%%", pct);
            }
            snprintf(stats_buf, sizeof(stats_buf), " [%d→%d tok%s%s%s]",
                     ev->stats.prompt_tokens, ev->stats.completion_tokens,
                     pp_str, gen_str, ctx_str);
        }
        /* Truncate description for display */
        const char *desc = ev->description ? ev->description : "";
        char desc_buf[201];
        if (strlen(desc) > 200) {
            memcpy(desc_buf, desc, 197);
            desc_buf[197] = '.'; desc_buf[198] = '.';
            desc_buf[199] = '.'; desc_buf[200] = '\0';
            desc = desc_buf;
        }
        { char _dur[32]; fmt_duration(ev->step_elapsed, _dur, sizeof(_dur));
        fprintf(stderr, "\r\033[K[step %d] %s: %s (%s)%s\n",
                ev->step, ev->action ? ev->action : "?",
                desc, _dur, stats_buf); }
        break;
    }

    case REACT_EVENT_TOOL_OUTPUT: {
        /* Print metadata JSON */
        if (ev->tool_meta) {
            char *meta_str = cJSON_PrintUnformatted(ev->tool_meta);
            if (meta_str) {
                fprintf(stderr, "  → %.*s%s\n",
                        (int)(strlen(meta_str) < 300 ? strlen(meta_str) : 300),
                        meta_str,
                        strlen(meta_str) > 300 ? "..." : "");
                free(meta_str);
            }
        }
        /* Print stored file contents */
        if (ev->store_ref && session_dir) {
            read_and_print_store_file(session_dir, ev->store_ref);
        }
        break;
    }

    case REACT_EVENT_ERROR:
        fprintf(stderr, "\n[error] %s\n", ev->message ? ev->message : "unknown");
        break;

    case REACT_EVENT_WARNING:
        fprintf(stderr, "\n[warning] %s\n", ev->message ? ev->message : "unknown");
        break;

    case REACT_EVENT_DONE: {
        { char _dur[32]; fmt_duration(ev->step_elapsed, _dur, sizeof(_dur));
        fprintf(stderr, "\r\033[K[step %d] done (%s)\n", ev->step, _dur); }
        /* Stats printed AFTER result (main.c prints the result between done event and this) */
        /* We print stats here since the result will be printed by main.c via printf */
        fprintf(stderr, "\r\033[K");
        if (ev->stats.prompt_tokens > 0 || ev->stats.completion_tokens > 0) {
            char pp_str[32] = "", gen_str[32] = "", ctx_str[32] = "";
            if (ev->stats.prompt_per_second > 0)
                snprintf(pp_str, sizeof(pp_str), " | pp %.0f t/s",
                         ev->stats.prompt_per_second);
            if (ev->stats.predicted_per_second > 0)
                snprintf(gen_str, sizeof(gen_str), " | gen %.0f t/s",
                         ev->stats.predicted_per_second);
            if (ev->context_size > 0 && ev->stats.prompt_tokens > 0) {
                int pct = (int)(100.0 * ev->stats.prompt_tokens / ev->context_size);
                snprintf(ctx_str, sizeof(ctx_str), " | ctx %d%%", pct);
            }
            char _tdur[32]; fmt_duration(ev->total_elapsed, _tdur, sizeof(_tdur));
            fprintf(stderr, "[%d→%d tok%s%s%s | total %s]\n",
                    ev->stats.prompt_tokens, ev->stats.completion_tokens,
                    pp_str, gen_str, ctx_str, _tdur);
        }
        break;
    }
    }
}
