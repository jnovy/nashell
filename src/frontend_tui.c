#include "react_event.h"
#include "nash_limits.h"
#include "str.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>


/* ── Simple TUI frontend ─────────────────────────────── */
/* Reproduces the original terminal output via react events */

static void read_and_print_store_file(const char *session_dir, const char *ref) {
  if (!session_dir || !ref) return;
  char path[NASH_PATH_MAX];
  path_join(path, sizeof(path), session_dir, ref);
  FILE *f = fopen(path, "r");
  if (!f) return;
  char buf[NASH_PATH_MAX];
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
  /* Track whether streaming content is a JSON action object.
     * 0=undecided, 1=suppress (JSON), -1=show (text).
     * Reset on STEP_START, decided on first non-whitespace token. */
  static int suppress_json_stream = 0;

  switch (ev->type) {

    case REACT_EVENT_STEP_START:
      suppress_json_stream = 0; /* reset for new step */
      if (ev->pass_label && ev->pass_total > 0)
        fprintf(stderr, "\r\033[K[%d/%d %s] step %d/%d thinking...",
                ev->pass_index + 1, ev->pass_total, ev->pass_label,
                ev->step, ev->max_steps);
      else
        fprintf(stderr, "\r\033[K[step %d/%d] thinking...",
                ev->step, ev->max_steps);
      fflush(stderr);
      break;

    case REACT_EVENT_TOOL_START:
      fprintf(stderr, "\r\033[K[step %d] %s: %s",
              ev->step, ev->action ? ev->action : "?",
              ev->description ? ev->description : "");
      fflush(stderr);
      break;

    case REACT_EVENT_LLM_TOKEN:
      /* Streaming: print tokens as they arrive.
         * Suppress raw JSON action objects (e.g. {"thought":"","action":...})
         * that local models emit — the user doesn't need to see the raw JSON.
         * We detect whether the first non-whitespace char is '{' and suppress
         * all subsequent tokens for that step. */
      if (ev->token) {
        if (suppress_json_stream == 0) {
          /* First meaningful token — decide based on content */
          const char *p = ev->token;
          while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')
            p++;
          if (*p == '{')
            suppress_json_stream = 1;
          else if (*p)
            suppress_json_stream = -1;
        }
        if (suppress_json_stream != 1) {
          fprintf(stderr, "%s", ev->token);
          fflush(stderr);
        }
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
          double pctf = 100.0 * ev->stats.prompt_tokens / ev->context_size;
          if (pctf >= 1.0)
            snprintf(ctx_str, sizeof(ctx_str), " | ctx %d%%", (int)pctf);
          else
            snprintf(ctx_str, sizeof(ctx_str), " | ctx <1%%");
        }
        snprintf(stats_buf, sizeof(stats_buf), " [%d→%d tok%s%s%s]",
                 ev->stats.prompt_tokens, ev->stats.completion_tokens,
                 pp_str, gen_str, ctx_str);
      }
      /* Show description: inline if it fits, full on next line otherwise */
      const char *desc = ev->description ? ev->description : "";
      char _dur[32];
      fmt_duration(ev->step_elapsed, _dur, sizeof(_dur));
      /* Get terminal width */
      int term_cols = 120;
      {
        struct winsize ws;
        if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
          term_cols = ws.ws_col;
      }
      /* Compute prefix: "[step N] action: " */
      char prefix[128];
      snprintf(prefix, sizeof(prefix), "[step %d] %s: ",
               ev->step, ev->action ? ev->action : "?");
      /* Compute suffix: " (dur)stats" */
      char suffix[256];
      snprintf(suffix, sizeof(suffix), " (%s)%s", _dur, stats_buf);
      int avail = term_cols - (int)strlen(prefix) - (int)strlen(suffix);
      if (avail < 10) avail = 10;
      if ((int)strlen(desc) <= avail) {
        fprintf(stderr, "\r\033[K%s%s%s\n", prefix, desc, suffix);
      } else {
        fprintf(stderr, "\r\033[K%s%s\n  %s\n", prefix, suffix, desc);
      }
      break;
    }

    case REACT_EVENT_TOOL_OUTPUT: {
      /* Print metadata JSON: inline if it fits, full on next line otherwise */
      if (ev->tool_meta) {
        char *meta_str = cJSON_PrintUnformatted(ev->tool_meta);
        if (meta_str) {
          int meta_len = (int)strlen(meta_str);
          int tc = 120;
          {
            struct winsize ws;
            if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
              tc = ws.ws_col;
          }
          int meta_avail = tc - 4; /* "  → " prefix = 4 display cols */
          if (meta_avail < 10) meta_avail = 10;
          if (meta_len <= meta_avail) {
            fprintf(stderr, "  → %s\n", meta_str);
          } else {
            fprintf(stderr, "  →\n  %s\n", meta_str);
          }
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

    case REACT_EVENT_USER_ASK:
      /* In headless mode, user_ask can't work — print question and provide empty answer.
         * The react loop will receive "" as the answer and continue. */
      fprintf(stderr, "\n[user_ask] %s\n", ev->message ? ev->message : "?");
      fprintf(stderr, "[user_ask] headless mode — cannot prompt user, returning empty answer\n");
      break;

    case REACT_EVENT_PROMPT_PROGRESS:
      /* Handled by TUI (tui.c), ignored in headless mode */
      break;

    case REACT_EVENT_DONE: {
      {
        char _dur[32];
        fmt_duration(ev->step_elapsed, _dur, sizeof(_dur));
        fprintf(stderr, "\r\033[K[step %d] done (%s)\n", ev->step, _dur);
      }
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
          double pctf = 100.0 * ev->stats.prompt_tokens / ev->context_size;
          if (pctf >= 1.0)
            snprintf(ctx_str, sizeof(ctx_str), " | ctx %d%%", (int)pctf);
          else
            snprintf(ctx_str, sizeof(ctx_str), " | ctx <1%%");
        }
        char _tdur[32];
        fmt_duration(ev->total_elapsed, _tdur, sizeof(_tdur));
        fprintf(stderr, "[%d→%d tok%s%s%s | total %s]\n",
                ev->stats.prompt_tokens, ev->stats.completion_tokens,
                pp_str, gen_str, ctx_str, _tdur);
      }
      break;
    }
  }
}
