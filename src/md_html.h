/*
 * md_html.h -- Markdown-to-HTML conversion and table handling.
 *
 * Extracted from telegram.c; shared by telegram.c and matrix.c.
 */
#ifndef MD_HTML_H
#define MD_HTML_H

/* Convert markdown to Telegram-style HTML.
 * Returns heap-allocated string. Caller frees. */
char *md_to_html(const char *md);

/* Check whether markdown text contains table lines (2+ pipes per line). */
int md_has_table(const char *md);

/* Convert markdown tables to bullet-point lists.
 * Non-table content passes through unchanged.
 * Returns heap-allocated string. Caller frees. */
char *md_tables_to_bullets(const char *md);

#endif /* MD_HTML_H */
