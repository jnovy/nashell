#ifndef BANNER_H
#define BANNER_H

#include "config.h"

/* Build banner into allocated string — shared implementation for both stdio and TUI.
 * use_ansi: 1 = include ANSI color codes (terminal), 0 = plain text (TUI).
 * session_dir: if non-NULL, appended as "[session: ...]" line. */
char *build_banner_impl(const config_t *cfg, const char *props_json,
                        const char *nash_dir, const char *session_dir,
                        const char *profile_file, int use_ansi);

/* Print banner to stdout with ANSI colors (CLI mode) */
void print_banner(const config_t *cfg, const char *props_json,
                  const char *nash_dir, const char *profile_file);

/* Build banner as a plain-text string for ncurses TUI */
char *build_banner_string(const config_t *cfg, const char *props_json,
                          const char *nash_dir, const char *session_dir,
                          const char *profile_file);

#endif /* BANNER_H */
