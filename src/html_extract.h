#ifndef HTML_EXTRACT_H
#define HTML_EXTRACT_H

#include <stddef.h>

/* Case-insensitive prefix match. Returns 1 if s starts with prefix (ASCII). */
int html_ci_prefix(const char *s, const char *prefix);

/* Decode one HTML entity at &...; Returns decoded char count written to out.
 * *advance = bytes consumed from src (including & and ;). */
int html_decode_entity(const char *src, char *out, int *advance);

/* Extract readable text from HTML. Strips tags, preserves <a href> as markdown
 * links [text](url), strips <script>/<style> blocks entirely, decodes entities,
 * collapses whitespace. Returns malloc'd string; caller frees. */
char *html_extract_text(const char *html, size_t len);

#endif /* HTML_EXTRACT_H */
