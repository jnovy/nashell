/* md_doc.h - Parsed Markdown document types (ncurses-free).
 * This header contains the document model (md_doc_t, md_link_t) and
 * parser/query functions. It does NOT depend on ncurses, so it can be
 * included by any code that needs MD document types without pulling in
 * the rendering layer.
 *
 * For ncurses rendering functions (md_render, md_osc8_flush), include
 * md_render.h instead. */
#ifndef MD_DOC_H
#define MD_DOC_H

/* -- Hyperlink in a parsed MD document -- */
typedef struct {
  char *uri;       /* relative ("reactR0.md") or absolute path */
  char *text;      /* display text */
  int doc_line;    /* line in source where this link starts */
  int render_line; /* line in rendered output (set by md_render, accounts for skipped ``` lines) */
} md_link_t;

/* -- Parsed MD document -- */
typedef struct {
  char *source;     /* raw MD source (owned) */
  md_link_t *links; /* tracked hyperlinks */
  int link_count;
  int link_cap;
  /* Rendered lines cache (computed by md_render) */
  int total_lines;       /* total rendered lines (set after md_render) */
  int max_content_width; /* widest scrollable content in display columns (set by md_render) */
} md_doc_t;

/* Parse MD source into a document, extracting [text](uri) links.
 * Caller must free with md_doc_free(). */
md_doc_t *md_parse(const char *source);

/* Free a parsed document */
void md_doc_free(md_doc_t *doc);

/* Get the rendered line number of a link (for auto-scrolling to keep cursor visible) */
int md_link_line(md_doc_t *doc, int link_idx);

/* Find the rendered line number of a heading matching a #fragment anchor.
 * fragment: the anchor string WITHOUT the leading '#' (e.g., "1-current-state").
 * cols: terminal width used to account for word-wrapped lines.
 * Returns -1 if no matching heading found. */
int md_find_anchor(md_doc_t *doc, const char *fragment, int cols);

#endif /* MD_DOC_H */
