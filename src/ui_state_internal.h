#ifndef UI_STATE_INTERNAL_H
#define UI_STATE_INTERNAL_H

/*
 * ui_state_internal.h — Shared declarations for the ui_state module.
 *
 * This header is included by ui_state.c, ui_md_gen.c, ui_nav.c, and
 * ui_event.c.  It exposes helpers that were formerly static in the
 * monolithic ui_state.c so they can be shared across translation units.
 *
 * NOT part of the public API — only ui_state*.c files should include this.
 */

#include "ui_state.h"
#include "tui.h"
#include "nash_limits.h"
#include "nash_log.h"
#include "md_render.h"
#include "journal.h"
#include "str.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <libgen.h>
#include <dirent.h>
#include <ctype.h>
#include <limits.h>
#include <stdint.h>

/* ── Shared helpers ──────────────────────────────────────── */

/* Case-insensitive substring search (portable, no _GNU_SOURCE needed).
 * Used by ui_md_gen.c (log filtering) and ui_nav.c (scratchpad search). */
const char *ui_ci_strstr(const char *haystack, const char *needle);

/* Check if user is viewing session.md.
 * Used by ui_event.c and ui_state.c (set_banner, load_journal). */
static inline int viewing_session(ui_state_t *ui) {
  if (!ui->current_filepath) return 0;
  const char *base = strrchr(ui->current_filepath, '/');
  base = base ? base + 1 : ui->current_filepath;
  return strcmp(base, "session.md") == 0;
}

#endif /* UI_STATE_INTERNAL_H */
